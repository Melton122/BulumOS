/*
 * bwm.c - BulumOS window manager / compositor (Phase 9)
 *
 * Owns the bounded window list, z-order, focus, an off-screen back buffer
 * (software double buffering) and the hit-testing/dispatch that turns raw
 * binput events into window geometry actions.  The desktop shell draws a
 * "front buffer" (panel / launcher / desktop) that is composed into the back
 * buffer and presented each frame with a single blit (bgfx_blit_from).
 *
 * Back buffer layout: a malloc'd buffer of the device pixel format, one
 * pitch-aligned row per screen line.  All screen drawing helpers write into
 * this buffer; bgfx computes the exact device bytes via bgfx_pack at present
 * time.  This keeps window/shell rendering independent of the fbdev format.
 *
 * Threading: single-threaded (the desktop shell runs the event loop).
 */

#include "bwm.h"

#include "bgfx_font.h"
#include "bicon.h"

#include <linux/input.h>
#include <stdlib.h>
#include <string.h>

/* bounded static window table (no heap growth) */
static bwindow_t g_wins[BWM_MAX_WINDOWS];
static bwm_draw_fn g_draws[BWM_MAX_WINDOWS];   /* draw fn per table row */
static bwm_event_fn g_events[BWM_MAX_WINDOWS]; /* app event fn per row */
static int g_nwins = 0;
static int g_next_id = 1;

/* back buffer: device-native pixels, pitch bytes per row */
static uint8_t *g_bb = NULL;
static size_t  g_bb_len = 0;
static bwm_screen_t g_scrn;
static int g_ready = 0;

static const btheme_t *g_theme;

/* panel reserved at the top of the screen; desktop work area sits below it */
static int g_panel_h = 0;

/* pointer state */
static bwm_pointer_t g_ptr;
static int g_grab_mode = 0;        /* 1 = moving, 2 = resizing */
static int g_grb_id = 0;           /* window id being dragged */
static int g_grab_off_x = 0, g_grab_off_y = 0;
static int g_grb_x = 0, g_grb_y = 0;   /* pointer pos when grab began */
static int g_grab_start_w = 0, g_grab_start_h = 0;

/* shell overlays */
static bwm_overlay_fn g_bg_fn = NULL;
static void *g_bg_user = NULL;
static bwm_overlay_fn g_fg_fn = NULL;
static void *g_fg_user = NULL;

/* launcher trap region (open + geometry) */
static int g_launcher_open = 0;
static int g_launcher_x, g_launcher_y, g_launcher_w, g_launcher_h;

#define RESIZE_GRIP 6

/* ---------------------------------------------------------------- */

const bwm_screen_t *bwm_screen(void)
{
    return &g_scrn;
}

int bwm_ready(void)
{
    return g_ready;
}

const btheme_t *bwm_theme_cur(void)
{
    return g_theme;
}

int bwm_text_width(const char *s)
{
    if (!s)
        return 0;
    return (int)strlen(s) * BGFX_FONT_W;
}

/* ---- back-buffer pixel writer (device-native) --------------------------- */

static void bb_px(uint8_t *dst, int pitch, int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= g_scrn.w || y >= g_scrn.h)
        return;
    int bpp = g_scrn.bpp / 8;
    uint32_t w = bgfx_pack(color);
    uint8_t *p = dst + (size_t)y * (size_t)pitch + (size_t)x * (size_t)bpp;
    if (bpp == 4) {
        memcpy(p, &w, 4);
    } else if (bpp == 2) {
        uint16_t v = (uint16_t)w;
        memcpy(p, &v, 2);
    } else {
        p[0] = w & 0xff;
        p[1] = (w >> 8) & 0xff;
        p[2] = (w >> 16) & 0xff;
    }
}

void bwm_fill(const void *bb, int pitch, int x, int y, int w, int h,
              uint32_t color)
{
    if (!bb || w <= 0 || h <= 0)
        return;
    /* clip to the back buffer (full screen) - never write OOB */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0 || x >= g_scrn.w || y >= g_scrn.h)
        return;
    if (w > g_scrn.w - x) w = g_scrn.w - x;
    if (h > g_scrn.h - y) h = g_scrn.h - y;
    uint32_t pw = bgfx_pack(color);
    int bpp = g_scrn.bpp / 8;
    for (int yy = 0; yy < h; yy++) {
        uint8_t *p = (uint8_t *)bb + (size_t)(y + yy) * (size_t)pitch +
                     (size_t)x * (size_t)bpp;
        if (bpp == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int xx = 0; xx < w; xx++)
                q[xx] = pw;
        } else if (bpp == 2) {
            uint16_t *q = (uint16_t *)p;
            uint16_t v = (uint16_t)pw;
            for (int xx = 0; xx < w; xx++)
                q[xx] = v;
        } else {
            for (int xx = 0; xx < w; xx++) {
                p[0] = pw & 0xff;
                p[1] = (pw >> 8) & 0xff;
                p[2] = (pw >> 16) & 0xff;
                p += 3;
            }
        }
    }
}

void bwm_gradient(const void *bb, int pitch, uint32_t top, uint32_t bot)
{
    if (!bb)
        return;
    int h = g_scrn.h;
    int w = g_scrn.w;
    int tr = (top >> 16) & 0xff, tg = (top >> 8) & 0xff, tb = top & 0xff;
    int br = (bot >> 16) & 0xff, bg = (bot >> 8) & 0xff, bb2 = bot & 0xff;
    int bpp = g_scrn.bpp / 8;
    for (int y = 0; y < h; y++) {
        int r = tr + ((br - tr) * y) / (h > 1 ? h - 1 : 1);
        int gv = tg + ((bg - tg) * y) / (h > 1 ? h - 1 : 1);
        int b = tb + ((bb2 - tb) * y) / (h > 1 ? h - 1 : 1);
        uint32_t pw = bgfx_pack(((uint32_t)r << 16) | ((uint32_t)gv << 8) | (uint32_t)b);
        uint8_t *dst = (uint8_t *)bb + (size_t)y * (size_t)pitch;
        if (bpp == 4) {
            uint32_t *q = (uint32_t *)dst;
            for (int x = 0; x < w; x++)
                q[x] = pw;
        } else if (bpp == 2) {
            uint16_t v = (uint16_t)pw;
            for (int x = 0; x < w; x++)
                memcpy(dst + (size_t)x * 2, &v, 2);
        } else {
            for (int x = 0; x < w; x++) {
                dst[0] = pw & 0xff; dst[1] = (pw >> 8) & 0xff; dst[2] = (pw >> 16) & 0xff;
                dst += 3;
            }
        }
    }
}

void bwm_hline(const void *bb, int pitch, int x, int y, int w, uint32_t c)
{
    if (!bb || w <= 0)
        return;
    if (y < 0 || y >= g_scrn.h)
        return;
    if (x < 0) { w += x; x = 0; }
    if (w <= 0 || x >= g_scrn.w)
        return;
    if (w > g_scrn.w - x) w = g_scrn.w - x;
    uint32_t pw = bgfx_pack(c);
    int bpp = g_scrn.bpp / 8;
    uint8_t *p = (uint8_t *)bb + (size_t)y * (size_t)pitch + (size_t)x * (size_t)bpp;
    if (bpp == 4) {
        uint32_t *q = (uint32_t *)p;
        for (int i = 0; i < w; i++)
            q[i] = pw;
    } else if (bpp == 2) {
        uint16_t v = (uint16_t)pw;
        for (int i = 0; i < w; i++)
            memcpy(p + (size_t)i * 2, &v, 2);
    } else {
        for (int i = 0; i < w; i++) {
            p[0] = pw & 0xff; p[1] = (pw >> 8) & 0xff; p[2] = (pw >> 16) & 0xff;
            p += 3;
        }
    }
}

void bwm_vline(const void *bb, int pitch, int x, int y, int h, uint32_t c)
{
    for (int i = 0; i < h; i++)
        bb_px((uint8_t *)bb, pitch, x, y + i, c);
}

void bwm_rect(const void *bb, int pitch, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0)
        return;
    bwm_hline(bb, pitch, x, y, w, c);
    bwm_hline(bb, pitch, x, y + h - 1, w, c);
    bwm_vline(bb, pitch, x, y, h, c);
    bwm_vline(bb, pitch, x + w - 1, y, h, c);
}

void bwm_char(const void *bb, int pitch, int x, int y, char c,
              uint32_t fg, uint32_t bg)
{
    int idx = (unsigned char)c - BGFX_FONT_FIRST;
    if (idx < 0 || idx > BGFX_FONT_LAST - BGFX_FONT_FIRST)
        return;
    for (int row = 0; row < BGFX_FONT_H; row++) {
        uint8_t bits = bgfx_font_data[idx][row];
        for (int col = 0; col < BGFX_FONT_W; col++) {
            int on = (bits >> (7 - col)) & 1;
            bb_px((uint8_t *)bb, pitch, x + col, y + row, on ? fg : bg);
        }
    }
}

void bwm_text(const void *bb, int pitch, int x, int y, const char *s,
              uint32_t fg, uint32_t bg)
{
    if (!s)
        return;
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            y += BGFX_FONT_H;
            continue;
        }
        bwm_char(bb, pitch, cx, y, *s, fg, bg);
        cx += BGFX_FONT_W;
    }
}

void bwm_icon(const void *bb, int pitch, int x, int y, const char *name,
              int size, uint32_t on, uint32_t off)
{
    const uint16_t *glyph = bicon_glyph(name);
    if (!glyph || size <= 0)
        return;
    for (int yy = 0; yy < size; yy++) {
        int sy = yy * BICON_GRID / size;
        for (int xx = 0; xx < size; xx++) {
            int sx = xx * BICON_GRID / size;
            uint16_t row = glyph[sy];
            int bit = (row & (0x8000u >> sx)) ? 1 : 0;
            bb_px((uint8_t *)bb, pitch, x + xx, y + yy, bit ? on : off);
        }
    }
}

/* ---- window table ------------------------------------------------------ */

static int alloc_id(void)
{
    for (int tries = 0; tries < 10000; tries++) {
        int id = g_next_id++;
        if (g_next_id < 0)
            g_next_id = 1;
        int used = 0;
        for (int i = 0; i < g_nwins; i++)
            if (g_wins[i].id == id) { used = 1; break; }
        if (!used)
            return id;
    }
    return -1;
}

static bwindow_t *find_win(int id)
{
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id == id && g_wins[i].id != 0)
            return &g_wins[i];
    return NULL;
}

/* ---- lifecycle --------------------------------------------------------- */

int bwm_init(const btheme_t *theme, int panel_h)
{
    if (g_ready)
        return 0;
    if (!bgfx_available())
        return -1;
    memset(g_wins, 0, sizeof(g_wins));
    memset(g_draws, 0, sizeof(g_draws));
    memset(g_events, 0, sizeof(g_events));
    g_nwins = 0;
    g_next_id = 1;
    memset(&g_ptr, 0, sizeof(g_ptr));
    g_grab_mode = 0;

    g_scrn.w = bgfx_get_width();
    g_scrn.h = bgfx_get_height();
    g_scrn.pitch = bgfx_get_pitch();
    g_scrn.bpp = bgfx_get_bpp();
    if (g_scrn.w <= 0 || g_scrn.h <= 0 || g_scrn.pitch <= 0 || g_scrn.bpp <= 0)
        return -1;
    g_theme = theme;
    g_panel_h = panel_h > 0 ? panel_h : 0;

    g_bb_len = (size_t)g_scrn.pitch * (size_t)g_scrn.h;
    g_bb = malloc(g_bb_len);
    if (!g_bb)
        return -1;

    bwin_set_workarea(0, g_panel_h, g_scrn.w, g_scrn.h - g_panel_h);
    g_ready = 1;
    return 0;
}

void bwm_shutdown(void)
{
    if (g_bb) {
        free(g_bb);
        g_bb = NULL;
        g_bb_len = 0;
    }
    g_ready = 0;
    g_nwins = 0;
}

int bwm_window_count(void)      { return g_nwins; }

int bwm_focused_window(void)
{
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id && g_wins[i].focused && g_wins[i].visible)
            return g_wins[i].id;
    return 0;
}

/* deliver a body/keyboard event to the app of the given window id; returns
 * 1 if the app consumed it.  The pointer x,y are screen coordinates. */
static int app_event(int id, const binput_event_t *ev, int x, int y)
{
    if (id <= 0)
        return 0;
    bwindow_t *w = find_win(id);
    if (!w)
        return 0;
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id == id)
            return g_events[i] && g_events[i](w->user, ev, x, y);
    return 0;
}

/* is the focused window's app event handler attached? */
static int focused_has_app(void)
{
    int id = bwm_focused_window();
    if (id <= 0)
        return 0;
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id == id)
            return g_events[i] != NULL;
    return 0;
}

const bwm_pointer_t *bwm_pointer(void)
{
    return &g_ptr;
}

/* Windows are drawn in increasing z-order; the topmost has the highest z. */
static int z_of(const bwindow_t *w)
{
    return w->z;
}

static bwindow_t *topmost_visible(void)
{
    bwindow_t *best = NULL;
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id && g_wins[i].visible &&
            (!best || z_of(&g_wins[i]) > z_of(best)))
            best = &g_wins[i];
    return best;
}

/* ---- window management ------------------------------------------------- */

int bwm_window_create(const char *title, int x, int y, int w, int h,
                      int min_w, int min_h, int max_w, int max_h,
                      bwm_draw_fn draw, void *user)
{
    if (!g_ready || g_nwins >= BWM_MAX_WINDOWS)
        return -1;
    bwindow_t *win = &g_wins[g_nwins];
    int id = alloc_id();
    if (id < 0)
        return -1;
    if (bwin_init_win(win, id, title ? title : "", x, y, w, h) != 0)
        return -1;
    if (min_w > 0) win->min_w = min_w;
    if (min_h > 0) win->min_h = min_h;
    if (max_w > 0) win->max_w = max_w;
    if (max_h > 0) win->max_h = max_h;
    bwin_resize(win, win->w, win->h);
    g_draws[g_nwins] = draw;
    g_events[g_nwins] = NULL;
    win->user = user;
    g_nwins++;
    return id;
}

int bwm_window_set_event(int id, bwm_event_fn fn)
{
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id == id) {
            g_events[i] = fn;
            return 0;
        }
    return -1;
}

void bwm_window_destroy(int id)
{
    for (int i = 0; i < g_nwins; i++) {
        if (g_wins[i].id == id) {
            bwin_deinit_win(&g_wins[i]);
            memmove(&g_wins[i], &g_wins[i + 1],
                    (size_t)(g_nwins - i - 1) * sizeof(g_wins[0]));
            memmove(&g_draws[i], &g_draws[i + 1],
                    (size_t)(g_nwins - i - 1) * sizeof(g_draws[0]));
            memmove(&g_events[i], &g_events[i + 1],
                    (size_t)(g_nwins - i - 1) * sizeof(g_events[0]));
            g_nwins--;
            return;
        }
    }
}

bwindow_t *bwm_window_get(int id)
{
    return find_win(id);
}

bwindow_t *bwm_window_at(int index)
{
    if (index < 0 || index >= g_nwins)
        return NULL;
    return &g_wins[index];
}

void bwm_window_move(int id, int x, int y)
{
    bwindow_t *w = find_win(id);
    if (w)
        bwin_move(w, x, y);
}

void bwm_window_resize(int id, int w, int h)
{
    bwindow_t *win = find_win(id);
    if (win)
        bwin_resize(win, w, h);
}

void bwm_window_raise(int id)
{
    bwindow_t *win = find_win(id);
    if (!win || !win->visible)
        return;
    int maxz = 0;
    for (int i = 0; i < g_nwins; i++)
        if (g_wins[i].id && z_of(&g_wins[i]) > maxz)
            maxz = g_wins[i].z;
    win->z = maxz + 1;
}

void bwm_window_focus(int id)
{
    bwindow_t *win = find_win(id);
    if (!win)
        return;
    for (int i = 0; i < g_nwins; i++)
        g_wins[i].focused = (g_wins[i].id == id);
    if (win->visible)
        bwm_window_raise(id);
}

void bwm_window_minimize(int id)
{
    bwindow_t *win = find_win(id);
    if (!win)
        return;
    bwin_set_minimized(win, 1);
    if (win->focused)
        win->focused = 0;
    /* focus falls to the next visible window */
    bwindow_t *next = topmost_visible();
    if (next) {
        for (int i = 0; i < g_nwins; i++)
            g_wins[i].focused = (g_wins[i].id == next->id);
    }
}

void bwm_window_maximize(int id)
{
    bwindow_t *win = find_win(id);
    if (win && !win->minimized)
        bwin_set_maximized(win, 1);
}

void bwm_window_restore(int id)
{
    bwindow_t *win = find_win(id);
    if (win)
        bwin_restore(win);
}

void bwm_window_close(int id)
{
    bwm_window_destroy(id);
}

int bwm_window_find_at(int x, int y)
{
    for (int i = g_nwins - 1; i >= 0; i--) {
        bwindow_t *w = &g_wins[i];
        if (w->id && w->visible && bwin_contains(w, x, y))
            return w->id;
    }
    return 0;
}

void bwm_redraw(int id)
{
    (void)id; /* frame repaint covers everything in this phase */
}

void bwm_set_background_fn(bwm_overlay_fn fn, void *user)
{
    g_bg_fn = fn; g_bg_user = user;
}
void bwm_set_foreground_fn(bwm_overlay_fn fn, void *user)
{
    g_fg_fn = fn; g_fg_user = user;
}

void bwm_set_launcher(int open, int x, int y, int w, int h)
{
    g_launcher_open = open ? 1 : 0;
    g_launcher_x = x; g_launcher_y = y;
    g_launcher_w = w; g_launcher_h = h;
}

/* draw the chrome (title bar + border + buttons) for one window */
static void draw_window_chrome(const bwindow_t *win, int focused)
{
    const btheme_t *t = g_theme;
    uint32_t bar = focused ? t->titlebar_active : t->titlebar;
    uint32_t txt = focused ? t->text : t->muted;

    /* shadow under the window */
    bwm_fill(g_bb, g_scrn.pitch, win->x + 2, win->y + win->h, win->w, 2,
             t->background);

    /* title bar: -1 so the border below it reads cleanly */
    bwm_fill(g_bb, g_scrn.pitch, win->x, win->y, win->w, BWIN_TITLE_H, bar);
    bwm_rect(g_bb, g_scrn.pitch, win->x, win->y, win->w, BWIN_TITLE_H,
             focused ? t->border : t->border);

    /* buttons: [min][max][close] right-aligned */
    int base = win->x + win->w - 3 * BWIN_BTN_W - 4;
    int by = win->y + (BWIN_TITLE_H - BWIN_BTN_H) / 2;
    bwm_rect(g_bb, g_scrn.pitch, base, by, BWIN_BTN_W, BWIN_BTN_H, t->border);
    bwm_rect(g_bb, g_scrn.pitch, base + BWIN_BTN_W, by, BWIN_BTN_W, BWIN_BTN_H,
             t->border);
    bwm_rect(g_bb, g_scrn.pitch, base + 2 * BWIN_BTN_W, by, BWIN_BTN_W,
             BWIN_BTN_H, t->error);

    /* glyphs: minimise '-', maximise '[]', close 'x' */
    int cx = base + (BWIN_BTN_W - 3) / 2;
    int cy = by + (BWIN_BTN_H - 3) / 2;
    bwm_hline(g_bb, g_scrn.pitch, cx, cy + 1, 5, txt);
    int mx = base + BWIN_BTN_W + (BWIN_BTN_W - 5) / 2;
    bwm_rect(g_bb, g_scrn.pitch, mx, cy, 5, 4, txt);
    int c = base + 2 * BWIN_BTN_W + (BWIN_BTN_W - 5) / 2;
    bwm_hline(g_bb, g_scrn.pitch, c, cy, 5, t->surface);
    bwm_hline(g_bb, g_scrn.pitch, c, cy + 4, 5, t->surface);
    bwm_vline(g_bb, g_scrn.pitch, c + 2, cy, 5, t->surface);
    bwm_vline(g_bb, g_scrn.pitch, c + 2, cy + 1, 3, t->error);

    /* title text, left-aligned after a small pad; clip to buttons */
    int tx = win->x + 6;
    int tw = bwm_text_width(win->title);
    if (tw > win->w - 3 * BWIN_BTN_W - 16)
        tw = win->w - 3 * BWIN_BTN_W - 16;
    /* draw glyph-by-glyph so we can stop at the button margin */
    int ypos = win->y + (BWIN_TITLE_H - BGFX_FONT_H) / 2;
    int limit = win->x + win->w - 3 * BWIN_BTN_W - 12;
    for (const char *s = win->title; *s && tx < limit; s++)
        bwm_char(g_bb, g_scrn.pitch, tx, ypos, *s, txt, bar);

    /* window body background + border */
    bwm_fill(g_bb, g_scrn.pitch, win->x, win->y + BWIN_TITLE_H,
             win->w, win->h - BWIN_TITLE_H, t->window);
    bwm_rect(g_bb, g_scrn.pitch, win->x, win->y + BWIN_TITLE_H,
             win->w, win->h - BWIN_TITLE_H, t->border);
}

/* ---- rendering loop ----------------------------------------------------- */

void bwm_clear_contents(int x, int y, int w, int h)
{
    bwm_fill(g_bb, g_scrn.pitch, x, y, w, h,
             g_theme ? g_theme->window : 0x262630);
}

void bwm_frame(void)
{
    if (!g_ready)
        return;
    const btheme_t *t = g_theme;

    /* 1. desktop background (full screen, behind the panel) */
    if (g_bg_fn) {
        g_bg_fn(g_bg_user, g_bb, g_scrn.pitch);
    } else {
        bwm_fill(g_bb, g_scrn.pitch, 0, 0, g_scrn.w, g_scrn.h,
                 t ? t->background : 0x16161e);
    }

    /* 2. windows in z-order */
    for (int i = 0; i < g_nwins; i++) {
        bwindow_t *w = &g_wins[i];
        if (!w->id || !w->visible)
            continue;
        draw_window_chrome(w, w->focused);
        bwm_rect_t body, clip;
        body.x = w->x;
        body.y = w->y + BWIN_TITLE_H;
        body.w = w->w;
        body.h = w->h - BWIN_TITLE_H;
        clip.x = 0;
        clip.y = g_panel_h;
        clip.w = g_scrn.w;
        clip.h = g_scrn.h - g_panel_h;
        if (g_draws[i])
            g_draws[i](w->user, g_bb, g_scrn.pitch, body, clip, w->focused);
    }

    /* 3. foreground overlay (panel / launcher) on top */
    if (g_fg_fn)
        g_fg_fn(g_fg_user, g_bb, g_scrn.pitch);

    /* 4. present the whole back buffer in one pass */
    bgfx_blit_from(g_bb, g_scrn.pitch, 0, 0, g_scrn.w, g_scrn.h, 0, 0);
}

/* ---- input dispatch ----------------------------------------------------- */

static void focus_top(void)
{
    bwindow_t *t = topmost_visible();
    for (int i = 0; i < g_nwins; i++)
        g_wins[i].focused = (t && g_wins[i].id == t->id);
}

/* which interactive element is at pointer (x,y) - consults the shell first
 * through the foreground overlay pointer, then chrome, then body. */
static int hit_test(int x, int y, const bwindow_t **out_win)
{
    *out_win = NULL;
    /* panel / overlays capture the pointer */
    if (y < g_panel_h)
        return BWM_HIT_PANEL;
    /* open launcher menu traps the pointer over the desktop/windows */
    if (g_launcher_open && x >= g_launcher_x && x < g_launcher_x + g_launcher_w &&
        y >= g_launcher_y && y < g_launcher_y + g_launcher_h)
        return BWM_HIT_LAUNCHER;
    for (int i = g_nwins - 1; i >= 0; i--) {
        const bwindow_t *w = &g_wins[i];
        if (!w->id || !w->visible)
            continue;
        if (!bwin_contains(w, x, y))
            continue;
        /* resize grip on the bottom-right corner */
        int grip = RESIZE_GRIP;
        if (x >= w->x + w->w - grip && y >= w->y + w->h - grip) {
            *out_win = w;
            return BWM_HIT_RESIZE;
        }
        int btn = bwin_title_button(w, x, y);
        if (btn == 1) { *out_win = w; return BWM_HIT_MINIMIZE; }
        if (btn == 2) { *out_win = w; return BWM_HIT_MAXIMIZE; }
        if (btn == 3) { *out_win = w; return BWM_HIT_CLOSE; }
        if (bwin_title_hit(w, x, y)) { *out_win = w; return BWM_HIT_TITLE; }
        if (bwin_body_hit(w, x, y)) { *out_win = w; return BWM_HIT_WINDOW; }
    }
    return BWM_HIT_NONE;
}

int bwm_handle_event(const binput_event_t *ev)
{
    if (!ev)
        return BWM_HIT_NONE;

    /* relative pointer movement */
    if (ev->type == EV_REL) {
        if (ev->code == REL_X) {
            g_ptr.x += ev->value;
            if (g_ptr.x < 0) g_ptr.x = 0;
            if (g_ptr.x >= g_scrn.w) g_ptr.x = g_scrn.w - 1;
        } else if (ev->code == REL_Y) {
            g_ptr.y += ev->value;
            if (g_ptr.y < 0) g_ptr.y = 0;
            if (g_ptr.y >= g_scrn.h) g_ptr.y = g_scrn.h - 1;
        }

        /* while dragging a window, move/resize it */
        if (g_grab_mode) {
            bwindow_t *w = find_win(g_grb_id);
            if (!w) {
                g_grab_mode = 0;
                return BWM_HIT_NONE;
            }
            if (g_grab_mode == 1) {
                bwin_move(w, g_ptr.x - g_grab_off_x, g_ptr.y - g_grab_off_y);
            } else {
                bwin_resize(w,
                            g_grab_start_w + (g_ptr.x - g_grb_x),
                            g_grab_start_h + (g_ptr.y - g_grb_y));
            }
            return BWM_HIT_WINDOW;
        }
        /* hover motion over the focused window's body reaches its app */
        if (focused_has_app()) {
            bwindow_t *w = find_win(bwm_focused_window());
            if (w && bwin_body_hit(w, g_ptr.x, g_ptr.y)) {
                int c = app_event(w->id, ev, g_ptr.x, g_ptr.y);
                if (c)
                    return BWM_HIT_WINDOW;
            }
        }
        return BWM_HIT_NONE;
    }

    if (ev->type == EV_KEY) {
        int btn_down = (ev->value == 1);
        if (ev->code == BTN_LEFT) {
            g_ptr.lbtn = btn_down;
            if (btn_down) {
                if (g_grab_mode)
                    return BWM_HIT_WINDOW;
                const bwindow_t *w = NULL;
                int h = hit_test(g_ptr.x, g_ptr.y, &w);
                if (h == BWM_HIT_TITLE) {
                    /* begin moving */
                    if (w) {
                        bwm_window_focus(w->id);
                        g_grb_id = w->id;
                        g_grab_mode = 1;
                        g_grab_off_x = g_ptr.x - w->x;
                        g_grab_off_y = g_ptr.y - w->y;
                    }
                    return BWM_HIT_TITLE;
                } else if (h == BWM_HIT_RESIZE) {
                    if (w) {
                        bwm_window_raise(w->id);
                        g_grb_id = w->id;
                        g_grab_mode = 2;
                        g_grb_x = g_ptr.x;
                        g_grb_y = g_ptr.y;
                        bwindow_t *ww = find_win(w->id);
                        g_grab_start_w = ww ? ww->w : w->w;
                        g_grab_start_h = ww ? ww->h : w->h;
                        for (int i = 0; i < g_nwins; i++)
                            g_wins[i].focused = (g_wins[i].id == w->id);
                    }
                    return BWM_HIT_RESIZE;
                } else if (h == BWM_HIT_MINIMIZE) {
                    if (w) bwm_window_minimize(w->id);
                    return BWM_HIT_MINIMIZE;
                } else if (h == BWM_HIT_MAXIMIZE) {
                    if (w) bwm_window_maximize(w->id);
                    return BWM_HIT_MAXIMIZE;
                } else if (h == BWM_HIT_CLOSE) {
                    if (w) bwm_window_close(w->id);
                    return BWM_HIT_CLOSE;
                } else if (h == BWM_HIT_WINDOW) {
                    if (w) {
                        bwm_window_focus(w->id);
                        app_event(w->id, ev, g_ptr.x, g_ptr.y);
                    }
                    return BWM_HIT_WINDOW;
                } else if (h == BWM_HIT_PANEL) {
                    /* hand off to the shell */
                    return BWM_HIT_PANEL;
                }
                /* click on empty desktop: clear focus */
                focus_top();
                return BWM_HIT_NONE;
            } else {
                /* release: end drag */
                if (g_grab_mode)
                    g_grab_mode = 0;
                /* forward the release to the window whose body it is over */
                const bwindow_t *w = NULL;
                int h = hit_test(g_ptr.x, g_ptr.y, &w);
                if (h == BWM_HIT_WINDOW && w)
                    app_event(w->id, ev, g_ptr.x, g_ptr.y);
            }
            return BWM_HIT_NONE;
        }
        if (ev->code == BTN_RIGHT) {
            g_ptr.rbtn = btn_down;
            if (focused_has_app())
                app_event(bwm_focused_window(), ev, g_ptr.x, g_ptr.y);
            return BWM_HIT_NONE;
        }
        /* keyboard events go to the focused window's application */
        if (focused_has_app())
            app_event(bwm_focused_window(), ev, g_ptr.x, g_ptr.y);
        return BWM_HIT_NONE;
    }

    return BWM_HIT_NONE;
}
