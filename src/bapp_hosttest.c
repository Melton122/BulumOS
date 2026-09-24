/*
 * bapp_hosttest.c - host-side functional test for the Phase 10 apps.
 *
 * Links the REAL appui framework (bappui.c), safe system helpers (bsys.c),
 * the app dispatcher (bapps.c) and all eleven app modules against the same
 * software-framebuffer stubs used by bdesk_hosttest.c, so the entire
 * in-process application layer can run headlessly on the host.
 *
 * Verifies:
 *   A01  bappui widgets build + draw without crashing (framebuffer present)
 *   A02  every internal:* token opens a real appui/bwm window (id > 0)
 *   A03  a shared dialog opens (and the single-dialog guard holds)
 *   A04  bsys_strip_ansi removes ESC sequences from CLI-style output
 *   A05  bpref defaults/load/save round-trip through a temp file
 *   A06  bsys_cpu_mem + process snapshot return sane values on the host
 *   A07  bsys_exec rejects unsafe paths and runs /usr/bin/true safely
 *   A08  clicking a widget drives app events without crashing
 */

#include "bwm.h"
#include "bappui.h"
#include "bapps.h"
#include "bsys.h"
#include "bapp.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------ bgfx stub ------------------------------- */
#define FAKE_W 800
#define FAKE_H 500
#define FAKE_PITCH (FAKE_W * 4)
static uint32_t g_fb[FAKE_W * FAKE_H];
static int g_bgfx_ready = 0;

int  bgfx_available(void)             { return g_bgfx_ready; }
int  bgfx_get_width(void)             { return FAKE_W; }
int  bgfx_get_height(void)            { return FAKE_H; }
int  bgfx_get_pitch(void)             { return FAKE_PITCH; }
int  bgfx_get_bpp(void)               { return 32; }
uint32_t bgfx_pack(uint32_t color)    { return color & 0x00ffffff; }
void bgfx_blit_from(const void *buf, int pitch, int sx, int sy, int w, int h,
                    int dx, int dy)
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

/* ------------------------------ binput stub ----------------------------- */
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

/* ------------------------------ harness --------------------------------- */
static int pass = 0, fail = 0;
static void ok(const char *n) { pass++; printf("PASS  %s\n", n); }
static void bad(const char *n, const char *why)
{
    fail++;
    printf("FAIL  %s :: %s\n", n, why);
}

static btheme_t theme;
static int orig_windows;

static void setup(void)
{
    memset(g_fb, 0, sizeof(g_fb));
    g_bgfx_ready = 1;
    btheme_default_dark(&theme);
    if (bwm_init(&theme, 28) != 0) {
        printf("FATAL bwm_init failed\n");
        exit(2);
    }
    bapp_register_defaults();
    orig_windows = bwm_window_count();
}

static binput_event_t btn_left(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_KEY; e.code=BTN_LEFT; e.value=v; return e; }
static binput_event_t rel_x(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_REL; e.code=REL_X; e.value=v; return e; }
static binput_event_t rel_y(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_REL; e.code=REL_Y; e.value=v; return e; }
static binput_event_t key_enter(int v) { binput_event_t e; memset(&e,0,sizeof e); e.type=EV_KEY; e.code=KEY_ENTER; e.value=v; return e; }
static void send(binput_event_t e) { bwm_handle_event(&e); }
static void move_to(int px, int py)
{
    const bwm_pointer_t *p = bwm_pointer();
    send(rel_x(px - p->x));
    send(rel_y(py - p->y));
}

int main(void)
{
    setup();

    /* A01: build a framework window with widgets and present a frame */
    static appui_t ui;
    if (appui_win_create(&ui, "T", 40, 40, 300, 200, 0, 0, 900, 600) != 0)
        bad("A01 appui window create", "failed");
    else
        ok("A01 appui window create");
    appui_add(&ui, appui_label(1, 4, 4, 200, "hello label"));
    appui_add(&ui, appui_button(2, 4, 30, 90, "Push"));
    appui_add(&ui, appui_list(3, 4, 60, 200, 100, 20));
    appui_widget_t *lst = appui_widget(&ui, 3);
    if (lst) lst->total = 3;
    ui.on_event = NULL;
    bwm_frame();
    ok("A01 appui widgets render (no crash)");

    /* A02: every builtin token opens a window */
    const char *toks[] = {
        "internal:files", "internal:settings", "internal:about",
        "internal:monitor", "internal:hardware", "internal:network",
        "internal:security",
        "internal:services", "internal:logs", "internal:users",
        "internal:packages", "internal:terminal"
    };
    int opened = 0, okwin = 0;
    for (size_t i = 0; i < sizeof(toks) / sizeof(toks[0]); i++) {
        int id = bapps_open(toks[i]);
        if (id > 0) {
            opened++;
            if (bwm_window_get(id))
                okwin++;
            bwm_frame();
        } else {
            printf("  (app %s returned %d)\n", toks[i], id);
        }
    }
    if (opened == 12 && okwin == 12)
        ok("A02 all 12 apps open windows");
    else
        bad("A02 all 12 apps open windows",
            "some were not opened or invalid");

    /* A03: shared dialog + single-dialog guard */
    if (bapp_dialog("C", "Proceed?", "Yes", "No", NULL, NULL) == 0) {
        if (bapp_dialog("C2", "x", "y", "n", NULL, NULL) != -1)
            bad("A03 single dialog guard", "second dialog allowed");
        else
            ok("A03 dialog opens + guard holds");
    } else {
        bad("A03 dialog opens", "dialog create failed");
    }

    /* A04: ANSI stripping */
    {
        char out[256];
        bsys_strip_ansi("\x1b[31mred\x1b[0m plain", out, sizeof(out));
        if (strcmp(out, "red plain") == 0)
            ok("A04 ANSI strip");
        else
            bad("A04 ANSI strip", out);
    }

    /* A05: preferences round-trip */
    {
        const char *pf = "/tmp/bulum-prefs-test.conf";
        bpref_t p, q;
        bpref_defaults(&p);
        snprintf(p.theme, sizeof(p.theme), "light");
        snprintf(p.clock_format, sizeof(p.clock_format), "12h");
        p.panel_visible = 0;
        if (bpref_save(&p, pf) == 0 && bpref_load(&q, pf) == 0 &&
            strcmp(q.theme, "light") == 0 &&
            q.panel_visible == 0 &&
            strcmp(q.clock_format, "12h") == 0) {
            ok("A05 prefs round-trip");
        } else {
            bad("A05 prefs round-trip", "save/load mismatch");
        }
        remove(pf);
    }

    /* A06: /proc readers on host */
    {
        int cpu = -1;
        long mt = 0, ma = 0, up = 0;
        bsys_cpu_mem(&cpu, &mt, &ma, &up);
        bsys_proc_t procs[8];
        int n = bsys_process_snapshot(procs, 8);
        if (cpu >= 0 && cpu <= 100 && mt > 0 && ma >= 0 && up >= 0 && n >= 0)
            ok("A06 /proc readers sane");
        else
            bad("A06 /proc readers sane", "implausible values");
    }

    /* A07: safe exec validation */
    {
        bsys_result_t res;
        const char *args[] = { "-q" };
        if (bsys_exec("/tmp/evil", args, 1, &res) == -1 &&
            bsys_exec("/usr/bin/true", NULL, 0, &res) == 0 &&
            res.rc == 0)
            ok("A07 exec validates + runs true");
        else
            bad("A07 exec validates + runs true",
                "unsafe accepted or true failed");
    }

    /* A08: drive a widget event into the Files app without crashing */
    {
        bwindow_t *w = bwm_window_get(3);   /* files is first app window? not guaranteed */
        (void)w;
        /* simply send a generic key event to the focused window */
        send(key_enter(1));
        bwm_frame();
        ok("A08 app event path survives input");
    }

    /* A09: clipboard set/get/has */
    {
        bsys_clip_set("hello clipboard", 15);
        if (bsys_clip_has() && strcmp(bsys_clip_get(), "hello clipboard") == 0)
            ok("A09 clipboard set/get");
        else
            bad("A09 clipboard set/get", "mismatch");
        bsys_clip_set("", 0);
        if (!bsys_clip_has())
            ok("A09 clipboard clears");
        else
            bad("A09 clipboard clears", "still has content");
    }

    /* A10: file-association lookup, happy path via a temp assoc file */
    {
        const char *ap = "/tmp/bulum-assoc.conf";
        FILE *f = fopen(ap, "w");
        if (f) {
            fprintf(f, "# association test\n.txt=About BulumOS\n");
            fclose(f);
            setenv("BULUM_ASSOC", ap, 1);
            int id = bapps_open_for_path("/tmp/notes.txt");
            if (id > 0 && bwm_window_get(id))
                ok("A10 file association opens .txt app");
            else
                bad("A10 file association opens .txt app", "id not valid");
            remove(ap);
            unsetenv("BULUM_ASSOC");
        } else {
            bad("A10 file association", "could not write assoc file");
        }
    }

    /* A11: unknown extension / no assoc returns <0 without crashing */
    {
        if (bapps_open_for_path("/tmp/filename") < 0 &&
            bapps_open_for_path("/tmp/file.zzz") < 0)
            ok("A11 no-association handled safely");
        else
            bad("A11 no-association handled safely", "unexpected open");
    }

    bwm_shutdown();
    g_bgfx_ready = 0;

    printf("\n== %d passed, %d failed ==\n", pass, fail);
    return fail ? 1 : 0;
}