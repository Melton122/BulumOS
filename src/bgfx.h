/*
 * bgfx.h - BulumOS graphics abstraction (Phase 8)
 *
 * Small internal graphics API.  Applications do not see DRM/fbdev
 * details: the backend is chosen at init and may change in a later
 * phase without rewriting applications.
 *
 * Backends:
 *   BGFX_BACKEND_FB   framebuffer (/dev/fb0) - Phase 8 primary
 *   BGFX_BACKEND_DRM  direct DRM/KMS         - future backend
 *
 * Threading: not thread-safe.  One renderer at a time.
 */

#ifndef BGFX_H
#define BGFX_H

#include <stdint.h>

#define BGFX_VERSION "1.0.0"

#define BGFX_BACKEND_NONE 0
#define BGFX_BACKEND_FB   1
#define BGFX_BACKEND_DRM  2

/* pixel formats actually supported by the framebuffer backend */
#define BGFX_FMT_UNKNOWN  0
#define BGFX_FMT_XRGB8888 1
#define BGFX_FMT_BGRX8888 2
#define BGFX_FMT_RGB888   3
#define BGFX_FMT_RGB565   4

#define BGFX_MAX_DEVNAME 128

typedef struct {
    int backend;          /* BGFX_BACKEND_* */
    int width;            /* visible width in pixels        */
    int height;           /* visible height in pixels       */
    int pitch;            /* line length in bytes           */
    int bpp;              /* bits per pixel                 */
    int format;           /* BGFX_FMT_*                     */
    char fb_path[BGFX_MAX_DEVNAME];  /* /dev/fb0 or ""       */
    char drm_path[BGFX_MAX_DEVNAME]; /* /dev/dri/card0 or "" */
    char gpu[BGFX_MAX_DEVNAME];      /* e.g. virtio-gpu      */
    char driver[BGFX_MAX_DEVNAME];   /* e.g. virtio_gpu      */
    char error[256];      /* last error message              */
} bgfx_info_t;

/* lifecycle -------------------------------------------------- */
/* backend: "auto", "framebuffer", "drm" (case-insensitive)     */
/* Returns 0 on success, -1 on failure (info.error set).        */
int  bgfx_init(const char *backend);
void bgfx_shutdown(void);
int  bgfx_available(void);
const bgfx_info_t *bgfx_info(void);

/* geometry ---------------------------------------------------- */
int bgfx_get_width(void);
int bgfx_get_height(void);
int bgfx_get_pitch(void);
int bgfx_get_bpp(void);

/* off-screen back-buffer support (used by the Phase 9 compositor) ------ */
/* Pack an 0xRRGGBB colour into a device-native pixel word.  Combined with
 * the pitch/bpp accessors this lets a caller build an off-screen buffer in
 * the exact device pixel format, then present it in one pass with blit. */
uint32_t bgfx_pack(uint32_t color);

/* Copy a block of device-native pixels from a caller buffer into the
 * framebuffer.  `buf` is a pitch-aligned rectangle starting at (sx,sy);
 * the region (sx,sy,w,h) is written at destination (dx,dy).  Clipped to
 * the visible area.  Used to present a composed back buffer without
 * per-pixel overhead. */
void bgfx_blit_from(const void *buf, int pitch,
                    int sx, int sy, int w, int h, int dx, int dy);

/* rendering --------------------------------------------------- */
void bgfx_begin(void);   /* start a frame (no-op on fb backend) */
void bgfx_end(void);     /* end a frame   (no-op on fb backend) */
void bgfx_clear(uint32_t color);
int  bgfx_put_pixel(int x, int y, uint32_t color);
int  bgfx_fill_rect(int x, int y, int w, int h, uint32_t color);
int  bgfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
int  bgfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* detection (works without initializing a backend) ------------ */
/* Fills info with whatever graphics hardware is present.        */
int bgfx_detect(bgfx_info_t *info);
int bgfx_count_inputs(void);

#endif /* BGFX_H */