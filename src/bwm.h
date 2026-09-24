/*
 * bwm.h - BulumOS window manager / compositor (Phase 9)
 *
 * A small, self-contained, BulumOS-native compositor.  It owns a bounded
 * window list (bwindow_t), the z-order, focus, an off-screen back buffer
 * (double buffering) and the hit-testing that turns raw binput events into
 * geometry actions (move / resize / focus / close / minimize / maximize).
 *
 * Rendering is software-composed into a back buffer that matches the device
 * pixel format (built via bgfx_pack / bgfx_get_pitch), then presented to the
 * compositor's front buffer in a single pass (bgfx_blit_from).  This avoids
 * the per-pixel overhead of drawing straight to the mapped fbdev and gives
 * the best available anti-tearing behaviour on the framebuffer backend.
 *
 * The desktop shell (bshell) drives this compositor through a small surface
 * API and provides the "content renderer" for each window (windows are
 * in-process in this phase; no cross-process IPC).
 */

#ifndef BWM_H
#define BWM_H

#include "bgfx.h"
#include "bwindow.h"
#include "btheme.h"
#include "binput.h"

/* surface region a window's content renderer may draw into */
typedef struct {
    int x, y;      /* absolute top-left of the window body (below title bar) */
    int w, h;      /* body size in pixels                                   */
} bwm_rect_t;

/* Function pointer a window uses to draw its own content into the back
 * buffer.  `bb` is a device-native back buffer of the full screen with the
 * given pitch; the window must only write within rect.  `focused` tells the
 * app whether it is the active window. */
typedef void (*bwm_draw_fn)(void *user, const void *bb, int pitch,
                            bwm_rect_t rect, bwm_rect_t clip, int focused);

/* A window's application event handler (Phase 10).  Receives raw binput
 * events that belong to the window's body interaction: keyboard key press /
 * release (EV_KEY, non-button) when the window is focused, and pointer
 * motion / button clicks while the pointer is over the window body (the
 * compositor still owns title-drag, resize grip and the title chrome
 * buttons).  `x`,`y` are the pointer position at the event.  Return 1 if the
 * app consumed the event (so the shell won't interpret it), 0 otherwise. */
typedef int (*bwm_event_fn)(void *user, const binput_event_t *ev, int x, int y);

/* Widget/bounds groups a window may interact with on drags/buttons. */
enum {
    BWM_HIT_NONE = 0,
    BWM_HIT_WINDOW,
    BWM_HIT_TITLE,
    BWM_HIT_CLOSE,
    BWM_HIT_MINIMIZE,
    BWM_HIT_MAXIMIZE,
    BWM_HIT_RESIZE,   /* bottom-right corner resize grip */
    BWM_HIT_PANEL,    /* captured by the desktop shell */
    BWM_HIT_LAUNCHER, /* launcher overlay */
    BWM_HIT_TRAP,     /* menus / overlays grab pointer */
};

/* geometry of the whole virtual screen */
typedef struct {
    int w, h;         /* framebuffer dimensions */
    int pitch;        /* framebuffer line pitch in bytes */
    int bpp;          /* bits per pixel */
} bwm_screen_t;

#define BWM_MAX_WINDOWS BWIN_MAX_WINDOWS

/* ---- lifecycle --------------------------------------------------------- */
/* Initialise the compositor for the current bgfx backend.  Allocates the
 * back buffer and sets the window work area below the panel.  Returns 0 on
 * success, -1 on failure (e.g. out of memory). */
int  bwm_init(const btheme_t *theme, int panel_h);
void bwm_shutdown(void);
int  bwm_ready(void);

const bwm_screen_t *bwm_screen(void);

/* Active theme the compositor was built with. */
const btheme_t *bwm_theme_cur(void);

/* ---- window management ------------------------------------------------- */
/* Create a window and register it with the compositor.  `title` is copied.
 * `min_w/min_h`/`max_w/max_h` may be 0 to use defaults.  `draw` is the
 * content renderer, `user` its context.  Returns a window id (>0) or -1. */
int  bwm_window_create(const char *title, int x, int y, int w, int h,
                       int min_w, int min_h, int max_w, int max_h,
                       bwm_draw_fn draw, void *user);
void bwm_window_destroy(int id);
bwindow_t *bwm_window_get(int id);
/* iterate the compositor's packed window table: index in [0, count). */
bwindow_t *bwm_window_at(int index);   /* NULL if index out of range */
int  bwm_window_find_at(int x, int y);   /* topmost visible window at (x,y) */

void bwm_window_move(int id, int x, int y);
void bwm_window_resize(int id, int w, int h);
void bwm_window_focus(int id);
void bwm_window_raise(int id);
void bwm_window_minimize(int id);
void bwm_window_maximize(int id);
void bwm_window_restore(int id);
void bwm_window_close(int id);

/* Attach an application event handler to a window (Phase 10).  Returns 0 on
 * success, -1 if the window id is invalid. */
int  bwm_window_set_event(int id, bwm_event_fn fn);

int  bwm_window_count(void);
int  bwm_focused_window(void);           /* id, or 0 */
void bwm_redraw(int id);                 /* request a repaint of this window */

/* ---- shell hook-ups ------------------------------------------------------- */
typedef void (*bwm_overlay_fn)(void *user, const void *bb, int pitch);
void bwm_set_background_fn(bwm_overlay_fn fn, void *user); /* desktop bg */
void bwm_set_foreground_fn(bwm_overlay_fn fn, void *user); /* panel/launcher */

/* Declare the launcher overlay's clickable region to the compositor so
 * pointer hits on it are reported as BWM_HIT_LAUNCHER (and forwarded to the
 * shell) instead of falling through to windows/desktop.  Set open=0 to
 * remove the trap region. */
void bwm_set_launcher(int open, int x, int y, int w, int h);

void bwm_frame(void);
void bwm_clear_contents(int x, int y, int w, int h); /* repaint region dirty */

/* ---- input dispatch ----------------------------------------------------- */
/* Feed a raw binput event to the compositor.  Returns a BWM_HIT_* describing
 * what the event hit (after handling geometry).  The shell inspects the
 * return to drive its own UI. */
int  bwm_handle_event(const binput_event_t *ev);

/* pointer + modifier state for the shell */
typedef struct {
    int x, y;          /* current pointer position (screen coords) */
    int lbtn, rbtn;    /* left/right button pressed state */
    int grabbed;       /* window id currently being moved/resized (0 = none) */
} bwm_pointer_t;
const bwm_pointer_t *bwm_pointer(void);

/* ---- helpers used by the shell to draw into the back buffer ------------- */
/* The shell and windows draw with these so they can produce device-native
 * pixels in the back buffer regardless of the framebuffer format. */

/* Clear a region of the back buffer to a solid 0xRRGGBB colour. */
void bwm_fill(const void *bb, int pitch, int x, int y, int w, int h,
              uint32_t color);
/* Draw the full-width horizontal gradient background between two colours.
 * `cur` is the current row (clamped); used for a live gradient sweep. */
void bwm_gradient(const void *bb, int pitch, uint32_t top, uint32_t bot);
/* Draw a 1px horizontal/vertical line. */
void bwm_hline(const void *bb, int pitch, int x, int y, int w, uint32_t c);
void bwm_vline(const void *bb, int pitch, int x, int y, int h, uint32_t c);
/* Draw a rectangle outline (1px). */
void bwm_rect(const void *bb, int pitch, int x, int y, int w, int h,
              uint32_t c);
/* Draw a single 8x16 glyph (font from bgfx_font.h) into the back buffer. */
void bwm_char(const void *bb, int pitch, int x, int y, char c,
              uint32_t fg, uint32_t bg);
/* Draw text (no wrapping, no clipping) into the back buffer. */
void bwm_text(const void *bb, int pitch, int x, int y, const char *s,
              uint32_t fg, uint32_t bg);
/* Measure text width in pixels. */
int  bwm_text_width(const char *s);
/* Draw a 16x16 icon (bicon) scaled to `size`, tinted `on` colour, with
 * `off` as the transparent/background colour. */
void bwm_icon(const void *bb, int pitch, int x, int y, const char *name,
              int size, uint32_t on, uint32_t off);

#endif /* BWM_H */
