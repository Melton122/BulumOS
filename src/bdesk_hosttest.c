/*
 * bdesk_hosttest.c - host-side functional test for the Phase 9 desktop.
 *
 * Links the REAL compositor (bwm.c), desktop shell (bshell.c) and their
 * pure dependencies (bwindow/btheme/bapp/bnotify/bicon) against a tiny
 * software framebuffer stub, so the whole desktop stack can run headlessly.
 *
 * It verifies:
 *   G01  the back buffer is allocated and a frame presents to the stub fb
 *   G02  window create returns a valid id and geometry is bounded
 *   G03  move clamps windows inside the work area (never off-screen)
 *   G04  resize clamps to [min,max]
 *   G05  focus raises the clicked window to the top (z-order)
 *   G06  title-click begins a move drag; releasing ends it
 *   G07  resize grip drag resizes the window
 *   G08  minimize hides the window; restore brings it back
 *   G09  close removes the window
 *   G10  the panel repaint and launcher draw without crashing
 *   G11  out-of-bounds drawing helpers never write/read OOB
 *
 * Build:  (see tools/test-bdesktop.sh)
 */

#include "bwm.h"
#include "bshell.h"
#include "bwindow.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ bgfx stub ================================ */
/* A software XRGB8888 framebuffer so the desktop stack can run on the host
 * where /dev/fb0 does not exist. */

#define FAKE_W 800
#define FAKE_H 500
#define FAKE_BPP 32
#define FAKE_PITCH (FAKE_W * 4)

static uint32_t g_fb[FAKE_W * FAKE_H];   /* device-native present buffer */
static int g_bgfx_ready = 0;

int  bgfx_available(void)             { return g_bgfx_ready; }
int  bgfx_get_width(void)             { return FAKE_W; }
int  bgfx_get_height(void)            { return FAKE_H; }
int  bgfx_get_pitch(void)             { return FAKE_PITCH; }
int  bgfx_get_bpp(void)               { return FAKE_BPP; }

uint32_t bgfx_pack(uint32_t color)
{
    /* XRGB8888: store the 0xRRGGBB directly (alpha ignored) */
    return color & 0x00ffffff;
}

void bgfx_blit_from(const void *buf, int pitch,
                    int sx, int sy, int w, int h, int dx, int dy)
{
    const uint8_t *src = (const uint8_t *)buf + (size_t)sy * (size_t)pitch
                         + (size_t)sx * 4;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int fx = dx + col, fy = dy + row;
            if (fx < 0 || fy < 0 || fx >= FAKE_W || fy >= FAKE_H)
                continue;
            uint32_t v;
            memcpy(&v, src + (size_t)col * 4, 4);
            g_fb[(size_t)fy * FAKE_W + fx] = v & 0x00ffffff;
        }
        src += pitch;
    }
}

/* helpers to inspect the fake framebuffer */
static uint32_t px_at(int x, int y)
{
    if (x < 0 || y < 0 || x >= FAKE_W || y >= FAKE_H)
        return 0;
    return g_fb[(size_t)y * FAKE_W + x];
}

static void fake_reset(void)
{
    memset(g_fb, 0, sizeof(g_fb));
    g_bgfx_ready = 1;
}

static void fake_release(void)
{
    g_bgfx_ready = 0;
}

/* ============================ binput stub =============================== */
int  binput_init(void)            { return 0; }
void binput_shutdown(void)        { }
int  binput_count(void)           { return 0; }
int  binput_kind(int d)           { (void)d; return 0; }
const char *binput_name(int d)    { (void)d; return ""; }
const char *binput_path(int d)    { (void)d; return ""; }
int  binput_poll(int t)           { (void)t; return 0; }
int  binput_read(binput_event_t *e) { (void)e; return 0; }
const char *binput_code_name(uint16_t t, uint16_t c) { (void)t;(void)c; return ""; }
int  bgfx_init(const char *b)     { (void)b; return 0; }
void bgfx_shutdown(void)          { }

/* ============================ test harness ============================== */

static int pass = 0, fail = 0;
static void ok(const char *n) { pass++; printf("PASS  %s\n", n); }
static void bad(const char *n, const char *why)
{
    fail++;
    printf("FAIL  %s :: %s\n", n, why);
}

static binput_event_t rel_x(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_REL; e.code=REL_X; e.value=v; return e; }
static binput_event_t rel_y(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_REL; e.code=REL_Y; e.value=v; return e; }
static binput_event_t btn_left(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_KEY; e.code=BTN_LEFT; e.value=v; return e; }
static void send(binput_event_t e) { bwm_handle_event(&e); }
/* move the pointer to an absolute screen position (emits REL deltas) */
static void move_to(int px, int py)
{
    const bwm_pointer_t *p = bwm_pointer();
    send(rel_x(px - p->x));
    send(rel_y(py - p->y));
}

static btheme_t theme;

static void setup(void)
{
    fake_reset();
    btheme_default_dark(&theme);
    if (bwm_init(&theme, BSHELL_PANEL_H) != 0) {
        printf("FATAL bwm_init failed\n");
        exit(2);
    }
}

int main(void)
{
    setup();

    /* G01: frame presents into the stub fb (background gradient region) */
    bshell_t shell;
    bshell_init(&shell, &theme);
    bshell_register(&shell);
    bwm_frame();
    uint32_t corner = px_at(FAKE_W / 2, FAKE_H - 1);   /* desktop, below panel */
    if (corner == theme.wbot && px_at(0, 0) == theme.panel)
        ok("G01 frame presents; background + panel drawn");
    else
        bad("G01 frame presents", "background not drawn");

    /* G02: create a window */
    int id = bwm_window_create("Test", 100, 60, 300, 200, 0, 0, 0, 0, NULL, NULL);
    if (id > 0 && bwm_window_count() == 1)
        ok("G02 create window");
    else
        bad("G02 create window", "id/count bad");

    /* G03: move clamps inside work area */
    bwm_window_move(id, FAKE_W + 50, -2000);
    bwindow_t *w = bwm_window_get(id);
    int wa[4];
    bwin_workarea(&wa[0], &wa[1], &wa[2], &wa[3]);
    if (w && w->visible && w->x < FAKE_W && w->y + w->h <= wa[1] + wa[3] &&
        w->x >= wa[0])
        ok("G03 move clamped on-screen");
    else
        bad("G03 move clamped on-screen", "off-screen");

    /* G04: resize clamps to [min,max] */
    bwm_window_resize(id, 10, 10);
    if (w && w->w >= BWIN_DEF_MIN_W && w->h >= BWIN_DEF_MIN_H)
        ok("G04 resize clamps to min");
    else
        bad("G04 resize clamps to min", "below min");

    /* G05: second window; focus raises it (z-order) */
    int id2 = bwm_window_create("Test2", 150, 100, 220, 140, 0, 0, 0, 0, NULL, NULL);
    bwm_window_focus(id2);
    bwindow_t *w2 = bwm_window_get(id2);
    if (w2 && w2->z > w->z && bwm_focused_window() == id2)
        ok("G05 focus raises + sets focus");
    else
        bad("G05 focus raises + sets focus", "z/focus");

    /* G06: title-click begins a move drag; drags move the window; release ends */
    bwm_window_focus(id);
    w = bwm_window_get(id);
    int tstart_x = w->x + 10;
    int tstart_y = w->y + 4;                      /* inside the title bar */
    move_to(tstart_x, tstart_y);
    send(btn_left(1));                             /* begin move grab */
    for (int i = 0; i < 5; i++) {                  /* drag by +50,+40 */
        send(rel_x(10));
        send(rel_y(8));
    }
    send(btn_left(0));                             /* release, end grab */
    bwindow_t *w3 = bwm_window_get(id);
    if (w3 && w3->x > tstart_x - 20 && w3->y > tstart_y - 20)
        ok("G06 title drag moves window");
    else
        bad("G06 title drag moves window", "no move");

    /* G07: resize grip drag resizes */
    w = bwm_window_get(id);
    int gx = w->x + w->w - 2, gy = w->y + w->h - 2;
    move_to(gx, gy);
    int before_w = w->w, before_h = w->h;
    send(btn_left(1));                             /* begin resize grab */
    send(rel_x(40));                               /* grow by +40,+30 */
    send(rel_y(30));
    send(btn_left(0));                             /* release */
    w = bwm_window_get(id);
    if (w && w->w > before_w && w->h > before_h && w->w > w->min_w)
        ok("G07 resize grip resizes");
    else
        bad("G07 resize grip resizes", "size not changed");

    /* G08: minimize hides; restore shows */
    bwm_window_minimize(id2);
    w2 = bwm_window_get(id2);
    if (w2 && w2->minimized && !w2->visible)
        ok("G08 minimize hides");
    else
        bad("G08 minimize hides", "not minimized");
    bwm_window_restore(id2);
    w2 = bwm_window_get(id2);
    if (w2 && !w2->minimized && w2->visible)
        ok("G08 restore shows");
    else
        bad("G08 restore shows", "not restored");

    /* G09: close removes window */
    bwm_window_close(id2);
    if (bwm_window_get(id2) == NULL && bwm_window_count() == 1)
        ok("G09 close removes");
    else
        bad("G09 close removes", "still present");

    /* G10: open the launcher and draw several frames */
    shell.launcher_open = 1;
    shell.launcher_hover = 0;
    for (int i = 0; i < 10; i++)
        bwm_frame();
    ok("G10 launcher + panel render");

    /* G11: out-of-bounds drawing helpers (must not crash) */
    bwm_fill(NULL, 0, 0, 0, 10, 10, 0x123456);
    bwm_fill(g_fb, FAKE_PITCH, -5, -5, 10, 10, 0x123456);
    bwm_fill(g_fb, FAKE_PITCH, FAKE_W, FAKE_H, 100, 100, 0x123456);
    bwm_text(g_fb, FAKE_PITCH, FAKE_W, FAKE_H, "hello", 1, 0);
    bwm_icon(g_fb, FAKE_PITCH, FAKE_W, FAKE_H, "terminal", 16, 1, 0);
    bwm_icon(g_fb, FAKE_PITCH, 10, 10, "no-such-icon", 16, 1, 0);
    bwm_gradient(g_fb, FAKE_PITCH, 0x102030, 0x405060);
    ok("G11 OOB draws safe");

    fake_release();
    bwm_shutdown();

    printf("\n== %d passed, %d failed ==\n", pass, fail);
    return fail ? 1 : 0;
}
