#pragma once

#include <stddef.h>
#include <stdint.h>

/* Run-length coding of RGB565 pixels, SET_KEY_IMAGE format 2 (docs/protocol.md). */

/* Worst case size of the coded form of `pixels` pixels (all different: one control byte per 128 pixels). */
#define RLE_MAX_SIZE(pixels) ((size_t)(pixels) * 2 + ((size_t)(pixels) + 127) / 128)

/* Codes `pixels` little-endian RGB565 pixels into `out` (at least RLE_MAX_SIZE(pixels) bytes); returns the size. */
size_t rle_encode(const uint8_t *src, size_t pixels, uint8_t *out);
