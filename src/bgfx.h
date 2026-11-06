/*
 * bgfx.h - BulumOS graphics abstraction (Phase 8, extended Phase 13)
 *
 * Small internal graphics API.  Applications do not see DRM/fbdev
 * details: the backend is chosen at init and may change in a later
 * phase without rewriting applications.
 *
 * Backends:
 *   BGFX_BACKEND_FB   framebuffer (/dev/fb0) - primary backend
 *   BGFX_BACKEND_DRM  direct DRM/KMS (dumb buffers + modeset) - Phase 13
 *   BGFX_BACKEND_MEM  headless software surface (no device node; used by
 *                     the compositor's self-test and host unit tests)
 *
 * Phase 13 additions:
 *   - a renderer surface (ARGB/XRGB8888, malloc'd) that the existing
 *     primitives can target, so client-side code (e.g. the Wayland
 *     desktop client drawing into a wl_shm buffer) reuses this exact
 *     drawing code with no per-backend branches;
 *   - a real DRM/KMS backend (mode resource scan, dumb buffer, addfb,
 *     setcrtc) so the compositor owns the scanout through DRM rather
 *     than the legacy fbdev emulation.
 *
 * Threading: not thread-safe.  One renderer at a time.
 */

#ifndef BGFX_H
#define BGFX_H

#include <stdint.h>

#define BGFX_VERSION "1.1.0"

#define BGFX_BACKEND_NONE 0
#define BGFX_BACKEND_FB   1
#define BGFX_BACKEND_DRM  2
#define BGFX_BACKEND_MEM  3

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

/* ---- Phase 13: renderer surfaces + target indirection -----------------
 *
 * A surface is a pitch-aligned ARGB/XRGB8888 pixel block owned by the
 * caller (typically malloc'd, e.g. a Wayland wl_shm buffer).  Targeting a
 * surface redirects the primitives above (blit/clear/put_pixel/
 * fill_rect/draw_char/draw_text and the geometry accessors) into it, so
 * the compositor and the client both render with one drawing core and no
 * knowledge of each other's buffers.  This is the documented seam that
 * lets the Wayland layer later be swapped for a native Bulum compositor
 * without touching applications.  bgfx_target_surface(NULL) restores
 * on-screen drawing. */
typedef struct {
    uint8_t *pixels;   /* malloc'd pixel block (ARGB/XRGB8888) */
    int width;
    int height;
    int pitch;         /* bytes per row */
    int bpp;           /* 32 */
    int format;        /* BGFX_FMT_XRGB8888 */
} bgfx_surface_t;

int  bgfx_surface_create(bgfx_surface_t *s, int w, int h); /* 0 = ok */
void bgfx_surface_destroy(bgfx_surface_t *s);
/* 0 = ok, -1 = rejected (NULL = target the screen) */
int  bgfx_target_surface(bgfx_surface_t *s);
/* Returns the renderer's active target (a snapshot it owns; identify it
 * by the `pixels` pointer), or NULL when drawing to the screen. */
const bgfx_surface_t *bgfx_target(void);

/* Read one pixel back from the active canvas (target surface, or the
 * screen when no target is set) as 0xRRGGBB.  Used by unit tests and the
 * compositor's self-test.  Returns 0 on success, -1 if out of range. */
int  bgfx_read_pixel(int x, int y, uint32_t *out);

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