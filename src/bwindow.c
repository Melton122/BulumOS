/*
 * bwindow.c - BulumOS native window model (Phase 9)
 *
 * Geometry is always validated against the configured work area and the
 * per-window min/max.  Every helper is defensive: a window can never leave
 * the screen bounds or exceed its size limits, and callers can never pass
 * coordinates that would produce an out-of-bounds framebuffer write.
 */

#include "bwindow.h"

#include <stdio.h>
#include <string.h>

static int g_wx = 0, g_wy = 0, g_ww = 800, g_wh = 600;

void bwin_set_workarea(int x, int y, int w, int h)
{
    g_wx = x;
    g_wy = y;
    /* keep sane minimums so clamping never produces empty geometry */
    if (w < BWIN_DEF_MIN_W + 16)
        w = BWIN_DEF_MIN_W + 16;
    if (h < BWIN_DEF_MIN_H + 16)
        h = BWIN_DEF_MIN_H + 16;
    g_ww = w;
    g_wh = h;
}

void bwin_workarea(int *x, int *y, int *w, int *h)
{
    if (x) *x = g_wx;
    if (y) *y = g_wy;
    if (w) *w = g_ww;
    if (h) *h = g_wh;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

int bwin_init_win(bwindow_t *win, int id, const char *title,
                  int x, int y, int w, int h)
{
    if (!win || id <= 0 || !title)
        return -1;

    memset(win, 0, sizeof(*win));
    win->id         = id;
    win->min_w      = BWIN_DEF_MIN_W;
    win->min_h      = BWIN_DEF_MIN_H;
    win->max_w      = BWIN_DEF_MAX_W;
    win->max_h      = BWIN_DEF_MAX_H;
    snprintf(win->title, sizeof(win->title), "%s", title);

    /* clamp size into [min,max] */
    w = clamp_int(w, win->min_w, g_ww);
    h = clamp_int(h, win->min_h, g_wh);
    if (win->max_w < w) w = win->max_w;
    if (win->max_h < h) h = win->max_h;
    if (w < win->min_w) w = win->min_w;
    if (h < win->min_h) h = win->min_h;
    win->w = w;
    win->h = h;

    /* clamp position so at least the title bar is reachable/on-screen */
    x = clamp_int(x, g_wx - w + BWIN_DEF_MIN_W,
                  g_wx + g_ww - BWIN_DEF_MIN_W);
    y = clamp_int(y, g_wy, g_wy + g_wh - BWIN_TITLE_H);
    win->x = x;
    win->y = y;

    win->visible  = 1;
    win->nrm_x = x; win->nrm_y = y; win->nrm_w = w; win->nrm_h = h;
    return 0;
}

void bwin_deinit_win(bwindow_t *win)
{
    if (win)
        memset(win, 0, sizeof(*win));
}

int bwin_move(bwindow_t *win, int x, int y)
{
    if (!win)
        return -1;
    x = clamp_int(x, g_wx - win->w + BWIN_DEF_MIN_W,
                  g_wx + g_ww - BWIN_DEF_MIN_W);
    y = clamp_int(y, g_wy, g_wy + g_wh - BWIN_TITLE_H);
    win->x = x;
    win->y = y;
    return 0;
}

int bwin_resize(bwindow_t *win, int w, int h)
{
    if (!win)
        return -1;
    w = clamp_int(w, win->min_w, win->max_w);
    h = clamp_int(h, win->min_h, win->max_h);
    w = clamp_int(w, win->min_w, g_ww);
    h = clamp_int(h, win->min_h, g_wh);
    win->w = w;
    win->h = h;
    /* keep the window on screen after a size change */
    bwin_move(win, win->x, win->y);
    return 0;
}

void bwin_set_minimized(bwindow_t *win, int on)
{
    if (!win)
        return;
    if (on) {
        win->minimized = 1;
        win->visible = 0;
    } else {
        win->minimized = 0;
        win->visible = 1;
    }
}

void bwin_set_maximized(bwindow_t *win, int on)
{
    if (!win || win->minimized)
        return;
    if (on && !win->maximized) {
        win->nrm_x = win->x; win->nrm_y = win->y;
        win->nrm_w = win->w; win->nrm_h = win->h;
        win->x = g_wx; win->y = g_wy;
        win->w = g_ww; win->h = g_wh;
        win->maximized = 1;
    } else if (!on && win->maximized) {
        win->x = win->nrm_x; win->y = win->nrm_y;
        win->w = win->nrm_w; win->h = win->nrm_h;
        win->maximized = 0;
    }
}

void bwin_restore(bwindow_t *win)
{
    if (!win)
        return;
    bwin_set_maximized(win, 0);
    bwin_set_minimized(win, 0);
    win->visible = 1;
}

void bwin_toggle_maximize(bwindow_t *win)
{
    if (win)
        bwin_set_maximized(win, !win->maximized);
}

int bwin_contains(const bwindow_t *win, int x, int y)
{
    if (!win || !win->visible)
        return 0;
    return x >= win->x && x < win->x + win->w &&
           y >= win->y && y < win->y + win->h;
}

int bwin_title_hit(const bwindow_t *win, int x, int y)
{
    if (!win || !win->visible)
        return 0;
    return x >= win->x && x < win->x + win->w &&
           y >= win->y && y < win->y + BWIN_TITLE_H;
}

int bwin_body_hit(const bwindow_t *win, int x, int y)
{
    if (!win || !win->visible)
        return 0;
    return x >= win->x && x < win->x + win->w &&
           y >= win->y + BWIN_TITLE_H && y < win->y + win->h;
}

int bwin_title_button(const bwindow_t *win, int x, int y)
{
    if (!win || !win->visible || !bwin_title_hit(win, x, y))
        return 0;
    int rx = x - win->x;
    int ry = y - win->y;
    if (ry < 2 || ry >= BWIN_TITLE_H - 2)
        return 0;
    /* buttons: [min][max][close] packed on the right */
    int base = win->w - 3 * BWIN_BTN_W - 4;
    if (rx >= base && rx < base + BWIN_BTN_W)
        return 1;                       /* minimize  - */
    if (rx >= base + BWIN_BTN_W && rx < base + 2 * BWIN_BTN_W)
        return 2;                       /* maximize  [] */
    if (rx >= base + 2 * BWIN_BTN_W && rx < base + 3 * BWIN_BTN_W)
        return 3;                       /* close     x  */
    return 0;
}
