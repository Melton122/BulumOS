/*
 * bicon.h - BulumOS native icon system (Phase 9.7)
 *
 * Tiny, dependency-free icon set.  Icons are single-plane 16x16 glyphs
 * (uint16 per row, MSB=leftmost pixel) plus a colour role; the desktop scales
 * them to the requested size (16/24/32/48/64) with nearest-neighbour
 * resampling, so there is no PNG decoder or icon framework to drag in.
 *
 * Supported names (subset is fine to grow):
 *   terminal files settings network security packages monitor users power
 *   search help about notifications volume wifi battery clock launcher
 */

#ifndef BICON_H
#define BICON_H

#include <stdint.h>

#define BICON_GRID 16

/* returns pointer to a 16x16 glyph (BICON_GRID uint16_t rows) for `name`,
 * or NULL if the name is unknown. */
const uint16_t *bicon_glyph(const char *name);

/* colour role: 0 = use accent-style, 1 = use text-style, 2 = use warning,
 * 3 = use error, 4 = use success (hint only; desktop decides tint) */
int bicon_role(const char *name);

/* does an icon with this name exist? */
int bicon_exists(const char *name);

/* nearest-neighbour scale a 16x16 glyph into a caller buffer.
 * out must be size*size.  `on` = the pixel colour to draw the glyph,
 * `off` = transparent/background colour. */
void bicon_scaled(const uint16_t *glyph, int size,
                  uint32_t on, uint32_t off, uint32_t *out);

#endif /* BICON_H */
