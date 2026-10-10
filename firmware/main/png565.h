#pragma once
/*
 * Minimal PNG -> RGB565 decoder for map tiles.
 *
 * Uses the zlib inflater in the ESP32-S3 ROM and a work buffer allocated
 * once, so decoding a tile never touches the heap (lodepng's growing
 * reallocs fragmented PSRAM until tiles failed to decode).
 *
 * Supports non-interlaced 8-bit grey/RGB/RGBA/grey+alpha and 1/2/4/8-bit
 * palette images - everything OpenStreetMap serves. Alpha is ignored.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct png565 png565_t;

/* Decoder for images up to max_w x max_h. NULL on out of memory. */
png565_t *png565_create(int max_w, int max_h);

/* png is modified in place (IDAT chunks are compacted). out must hold
 * want_w * want_h pixels; the image must be exactly that size. */
bool png565_decode(png565_t *d, uint8_t *png, size_t len, uint16_t *out, int want_w, int want_h);
