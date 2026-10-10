#include "pm335_codec.h"

#include <string.h>

#include "esp_attr.h"
#include <time.h>

#include "esp_random.h"

#define APP_SRC_ID       0x02     /* TUa.app */
#define PROTO_VERSION    0x04ED   /* what the app sends */

uint16_t pm335_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static size_t put_escaped(uint8_t *out, size_t pos, size_t size, const uint8_t *in, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        if (b == 0xFE || b == 0xFD) {
            if (pos + 2 > size) return 0;
            out[pos++] = 0xFD;
            out[pos++] = b == 0xFE ? 0x01 : 0x00;
        } else {
            if (pos + 1 > size) return 0;
            out[pos++] = b;
        }
    }
    return pos;
}

static uint8_t rand_byte(void)
{
    return (uint8_t)(esp_random() % 255);
}

size_t pm335_build_body(int fmt, uint16_t msg_type, const uint8_t *payload, size_t payload_len,
                        uint8_t body[PM335_MAX_BODY])
{
    size_t n = 0;
    uint32_t ts = (uint32_t)time(NULL);

    if (payload_len > PM335_MAX_BODY - 24) return 0;
    /* Confirmed against a capture of the app (2026-10-05):
     * fmt 18: 00 12 | 02 id id id | 04 ED | 00 00 00 00 | ts32 | type | payload | crc
     * The PM echoes the 3-byte id in its reply. */
    body[n++] = fmt >> 8;
    body[n++] = fmt & 0xFF;
    body[n++] = APP_SRC_ID;
    for (int i = 0; i < 3; i++) body[n++] = rand_byte();
    if (fmt == PM335_FMT_18) {
        body[n++] = PROTO_VERSION >> 8;
        body[n++] = PROTO_VERSION & 0xFF;
        memset(body + n, 0, 4);
        n += 4;
        for (int s = 24; s >= 0; s -= 8) body[n++] = ts >> s;
    }
    body[n++] = msg_type >> 8;
    body[n++] = msg_type & 0xFF;
    memcpy(body + n, payload, payload_len);
    n += payload_len;
    uint16_t crc = pm335_crc16(body, n);
    body[n++] = crc & 0xFF;
    body[n++] = crc >> 8;
    return n;
}

size_t pm335_wrap(const uint8_t *body, size_t n, uint8_t *out, size_t out_size)
{
    /* Escaped body first (its length goes in front), then assemble */
    uint8_t esc[PM335_MAX_BODY * 2];
    size_t esc_len = put_escaped(esc, 0, sizeof(esc), body, n);
    if (!esc_len) return 0;
    uint8_t lenb[2] = {esc_len >> 8, esc_len & 0xFF};

    size_t pos = 0;
    if (out_size < 1) return 0;
    out[pos++] = 0xFE;
    pos = put_escaped(out, pos, out_size, lenb, 2);
    if (!pos || pos + esc_len + 1 > out_size) return 0;
    memcpy(out + pos, esc, esc_len);
    pos += esc_len;
    out[pos++] = 0xFE;
    return pos;
}

size_t pm335_encode(int fmt, uint16_t msg_type, const uint8_t *payload, size_t payload_len,
                    uint8_t *out, size_t out_size)
{
    uint8_t body[PM335_MAX_BODY];
    size_t n = pm335_build_body(fmt, msg_type, payload, payload_len, body);
    return n ? pm335_wrap(body, n, out, out_size) : 0;
}

void pm335_parse_body(pm335_frame_t *f)
{
    f->fmt = f->msg_type = -1;
    f->payload_off = f->payload_len = 0;
    f->crc_ok = false;
    if (f->body_len < 6) return;
    uint16_t crc = pm335_crc16(f->body, f->body_len - 2);
    f->crc_ok = f->body[f->body_len - 2] == (crc & 0xFF) && f->body[f->body_len - 1] == (crc >> 8);
    f->fmt = (f->body[0] << 8) | f->body[1];
    size_t hdr = f->fmt == PM335_FMT_9 ? 4 : f->fmt == PM335_FMT_18 ? 14 : 0;
    if (!hdr || f->body_len < 2 + hdr + 2 + 2) return;
    f->msg_type = (f->body[2 + hdr] << 8) | f->body[3 + hdr];
    f->payload_off = 4 + hdr;
    f->payload_len = f->body_len - 2 - f->payload_off;
}

void pm335_decode(pm335_decoder_t *d, const uint8_t *chunk, size_t len,
                  void (*cb)(const pm335_frame_t *f, void *arg), void *arg)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = chunk[i];
        if (b == 0xFE) {
            if (d->len > 0) {
                /* End of a frame: unescape, drop the 2-byte length */
                EXT_RAM_BSS_ATTR static pm335_frame_t f;   /* only ever called from the NimBLE host task */
                uint8_t un[sizeof(d->buf)];
                size_t n = 0;
                for (size_t k = 0; k < d->len; k++) {
                    if (d->buf[k] == 0xFD && k + 1 < d->len) {
                        un[n++] = d->buf[++k] == 0x01 ? 0xFE : 0xFD;
                    } else {
                        un[n++] = d->buf[k];
                    }
                }
                if (n > 2 && n - 2 <= sizeof(f.body)) {
                    memcpy(f.body, un + 2, n - 2);
                    f.body_len = n - 2;
                    pm335_parse_body(&f);
                    cb(&f, arg);
                }
            }
            d->len = 0;   /* FE also starts the next frame */
        } else if (d->len < sizeof(d->buf)) {
            d->buf[d->len++] = b;
        } else {
            d->len = 0;   /* runaway: resync on the next FE */
        }
    }
}
