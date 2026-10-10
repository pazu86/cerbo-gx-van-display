#pragma once
/*
 * Projecta PM335 (TBB Power IoT protocol) frame codec - see PROTOCOL.md in the
 * "Projecta PM335" project for how this was derived from the IntelliJay app.
 *
 *   FE | esc(u16be(len(esc(body)))) | esc(body) | FE        esc: FE -> FD 01, FD -> FD 00
 *   body fmt 9 : 00 09 | 02 r r r | u16be(type) | payload | crc16
 *   body fmt 18: 00 12 | 02 id id id | 04 ED | 00 00 00 00 | ts32 | u16be(type) | payload | crc16
 *   crc16 = CRC-16/MODBUS of the body before it, low byte first
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM335_FMT_9   9
#define PM335_FMT_18  0x12

#define PM335_MSG_CONNECT        0x10
#define PM335_MSG_CONTROL        0x40
#define PM335_MSG_QUERY          0x50
#define PM335_MSG_HEARTBEAT      0xB0

#define PM335_MAX_BODY  240

uint16_t pm335_crc16(const uint8_t *data, size_t len);

/* Body (header + type + payload + CRC), unescaped. Returns its length, 0 if too big. */
size_t pm335_build_body(int fmt, uint16_t msg_type, const uint8_t *payload, size_t payload_len,
                        uint8_t body[PM335_MAX_BODY]);

/* Body -> wire frame (FE, escaped length, escaped body, FE). 0 if out is too small. */
size_t pm335_wrap(const uint8_t *body, size_t n, uint8_t *out, size_t out_size);

/* Builds a complete wire frame. Returns its length, 0 if it doesn't fit. */
size_t pm335_encode(int fmt, uint16_t msg_type, const uint8_t *payload, size_t payload_len,
                    uint8_t *out, size_t out_size);

typedef struct {
    uint8_t body[PM335_MAX_BODY];  /* unescaped, without the length field */
    size_t body_len;
    bool crc_ok;
    int fmt;                       /* -1 unknown */
    int msg_type;                  /* -1 unknown */
    size_t payload_off, payload_len;
} pm335_frame_t;

/* Stream decoder for notification chunks (frames can span several) */
typedef struct {
    uint8_t buf[512];
    size_t len;
} pm335_decoder_t;

/* Feeds a chunk; calls cb for every complete frame. */
void pm335_decode(pm335_decoder_t *d, const uint8_t *chunk, size_t len,
                  void (*cb)(const pm335_frame_t *f, void *arg), void *arg);

/* Fills fmt/msg_type/payload from body (used by the decoder and for TX logging) */
void pm335_parse_body(pm335_frame_t *f);
