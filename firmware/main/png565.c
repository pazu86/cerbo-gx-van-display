#include "png565.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "png565";

struct png565 {
    tinfl_decompressor inflater;
    uint8_t *raw;          /* inflated, filtered scanlines */
    size_t raw_cap;
    uint16_t palette[256];
};

png565_t *png565_create(int max_w, int max_h)
{
    png565_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!d) {
        return NULL;
    }
    d->raw_cap = (size_t)max_h * (1 + (size_t)max_w * 4);   /* worst case: RGBA8 */
    d->raw = heap_caps_malloc(d->raw_cap, MALLOC_CAP_SPIRAM);
    if (!d->raw) {
        free(d);
        return NULL;
    }
    return d;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* Undo the per-row PNG filters in place. */
static bool unfilter(uint8_t *raw, int h, size_t stride, int bpp)
{
    const uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + y * (stride + 1);
        uint8_t type = row[0];
        uint8_t *px = row + 1;
        for (size_t i = 0; i < stride; i++) {
            uint8_t a = i >= (size_t)bpp ? px[i - bpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            uint8_t c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: px[i] += a; break;
            case 2: px[i] += b; break;
            case 3: px[i] += (uint8_t)((a + b) >> 1); break;
            case 4: px[i] += paeth(a, b, c); break;
            default: return false;
            }
        }
        prev = px;
    }
    return true;
}

bool png565_decode(png565_t *d, uint8_t *png, size_t len, uint16_t *out, int want_w, int want_h)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (len < 8 || memcmp(png, sig, 8) != 0) {
        ESP_LOGW(TAG, "not a PNG");
        return false;
    }

    int w = 0, h = 0, depth = 0, ctype = -1;
    size_t idat_len = 0;
    size_t pos = 8;
    bool ihdr = false;
    while (pos + 12 <= len) {
        uint32_t clen = be32(png + pos);
        const uint8_t *type = png + pos + 4;
        uint8_t *data = png + pos + 8;
        if (pos + 12 + clen > len) {
            break;
        }
        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            w = be32(data);
            h = be32(data + 4);
            depth = data[8];
            ctype = data[9];
            if (data[12] != 0) {
                ESP_LOGW(TAG, "interlaced PNG not supported");
                return false;
            }
            ihdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t i = 0; i < clen / 3 && i < 256; i++) {
                d->palette[i] = rgb565(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            /* Compact all IDAT payloads to the front of the buffer; the
             * write position never passes the read position. */
            memmove(png + idat_len, data, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + clen;
    }
    if (!ihdr || w != want_w || h != want_h) {
        ESP_LOGW(TAG, "unexpected size %dx%d", w, h);
        return false;
    }

    int channels;
    switch (ctype) {
    case 0: channels = 1; break;   /* grey */
    case 2: channels = 3; break;   /* RGB */
    case 3: channels = 1; break;   /* palette */
    case 4: channels = 2; break;   /* grey + alpha */
    case 6: channels = 4; break;   /* RGBA */
    default: return false;
    }
    bool depth_ok = depth == 8 || (ctype == 3 && (depth == 1 || depth == 2 || depth == 4));
    if (!depth_ok) {
        ESP_LOGW(TAG, "unsupported PNG type %d depth %d", ctype, depth);
        return false;
    }
    size_t stride = ((size_t)w * channels * depth + 7) / 8;
    size_t raw_len = (size_t)h * (stride + 1);
    if (raw_len > d->raw_cap) {
        return false;
    }

    tinfl_init(&d->inflater);
    size_t in_sz = idat_len, out_sz = raw_len;
    tinfl_status st = tinfl_decompress(&d->inflater, png, &in_sz, d->raw, d->raw, &out_sz,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || out_sz != raw_len) {
        ESP_LOGW(TAG, "inflate failed (%d, %u/%u bytes)", st, (unsigned)out_sz, (unsigned)raw_len);
        return false;
    }

    int bpp = (channels * depth + 7) / 8;
    if (!unfilter(d->raw, h, stride, bpp < 1 ? 1 : bpp)) {
        ESP_LOGW(TAG, "bad filter type");
        return false;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *px = d->raw + y * (stride + 1) + 1;
        uint16_t *o = out + y * w;
        switch (ctype) {
        case 3:
            if (depth == 8) {
                for (int x = 0; x < w; x++) o[x] = d->palette[px[x]];
            } else {
                int per = 8 / depth, mask = (1 << depth) - 1;
                for (int x = 0; x < w; x++) {
                    int shift = 8 - depth * (1 + x % per);
                    o[x] = d->palette[(px[x / per] >> shift) & mask];
                }
            }
            break;
        case 0: for (int x = 0; x < w; x++) o[x] = rgb565(px[x], px[x], px[x]); break;
        case 4: for (int x = 0; x < w; x++) o[x] = rgb565(px[x * 2], px[x * 2], px[x * 2]); break;
        case 2: for (int x = 0; x < w; x++) o[x] = rgb565(px[x * 3], px[x * 3 + 1], px[x * 3 + 2]); break;
        case 6: for (int x = 0; x < w; x++) o[x] = rgb565(px[x * 4], px[x * 4 + 1], px[x * 4 + 2]); break;
        }
    }
    return true;
}
