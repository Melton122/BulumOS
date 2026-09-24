/*
 * bwindow.h - BulumOS native window model (Phase 9)
 *
 * Pure data model + bounds-checked geometry operations.  No dependency on
 * bgfx/binput, so this module is unit-testable on the host without a
 * framebuffer.  The window manager (bwm) owns the list; applications and the
 * desktop shell operate through these helpers which never accept or produce
 * invalid framebuffer coordinates.
 *
 * Invariants enforced here:
 *   - every window has clamped min/max size (min <= size <= max)
 *   - x/y always keep the window on (or mostly on) the screen
 *   - windows never exceed the screen work area (screen minus panel)
 */

#ifndef BWINDOW_H
#define BWINDOW_H

#include <stdint.h>

#define BWIN_MAX_TITLE 128
#define BWIN_MAX_WINDOWS 64   /* bounded window list (no unbounded growth) */

/* default bounds (px).  A host/VM test can override via bwin_set_bounds. */
#define BWIN_DEF_MIN_W 120
#define BWIN_DEF_MIN_H 80
#define BWIN_DEF_MAX_W 4096
#define BWIN_DEF_MAX_H 4096

typedef struct {
    int id;             /* unique, non-reused id (>= 1)            */
    int x, y;           /* top-left (title bar origin)             */
    int w, h;           /* current size (>= min, <= max)           */
    int min_w, min_h;
    int max_w, max_h;

    char title[BWIN_MAX_TITLE];

    unsigned visible   : 1;
    unsigned focused   : 1;
    unsigned minimized : 1;
    unsigned maximized : 1;
    int z;              /* z-index: higher = on top                */

    /* saved geometry used by maximize/restore */
    int nrm_x, nrm_y, nrm_w, nrm_h;

    /* app binding: index into the launcher registry (>0) or 0 = none */
    int app_id;
    long pid;           /* pid of the process hosting the app (0 = in-proc) */

    /* generic per-window user data pointer (content renderer/handler) */
    void *user;
} bwindow_t;

/* global screen work-area used for clamping.  Defaults to the (small)
 * minimums; the desktop sets these to the real framebuffer geometry. */
void bwin_set_workarea(int x, int y, int w, int h);
void bwin_workarea(int *x, int *y, int *w, int *h);

/* window lifecycle + geometry -------------------------------------------- */
/* All return 0 on success, -1 on invalid input / geometry violation.       */
int bwin_init_win(bwindow_t *win, int id, const char *title,
                  int x, int y, int w, int h);
void bwin_deinit_win(bwindow_t *win);

int bwin_move(bwindow_t *win, int x, int y);       /* clamped to work area */
int bwin_resize(bwindow_t *win, int w, int h);     /* clamped to [min,max] */

/* minimize/maximize/restore weave a window in and out of the normal layout
 * without losing its geometry. */
void bwin_set_minimized(bwindow_t *win, int on);
void bwin_set_maximized(bwindow_t *win, int on);
void bwin_restore(bwindow_t *win);                 /* clear min+max */
void bwin_toggle_maximize(bwindow_t *win);

/* hit testing ------------------------------------------------------------- */
/* Title bar hit at (x,y)?  Buttons live in the title bar (right side). */
int bwin_title_hit(const bwindow_t *win, int x, int y);
int bwin_body_hit(const bwindow_t *win, int x, int y);
int bwin_contains(const bwindow_t *win, int x, int y);

/* title-bar control buttons layout (coordinates relative to window) */
#define BWIN_BTN_W 18
#define BWIN_BTN_H 14
/* returns which button (0=none, 1=min, 2=max, 3=close) at client (x,y) */
int bwin_title_button(const bwindow_t *win, int x, int y);

/* title bar height used by the window manager */
#define BWIN_TITLE_H 18

#endif /* BWINDOW_H */
