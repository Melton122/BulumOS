/*
 * test-bgfx-surface.c - Phase 13 renderer-surface unit test (host).
 *
 * Exercises the headless memory backend plus the target-surface
 * indirection that the Phase 13 Wayland stack depends on:
 *
 *   - memory backend init / size parsing / geometry
 *   - drawing primitives against the screen canvas
 *   - target surface: geometry, isolation from the screen, blit, text
 *   - restore-to-screen via bgfx_target_surface(NULL)
 *   - invalid arguments rejected instead of crashing
 *   - clean shutdown
 *
 * Build:  gcc -O2 -Wall -o /tmp/tfx tools/test-bgfx-surface.c src/bgfx.c
 * Run:    BGFX_MEM_SIZE=320x200 /tmp/tfx      (exit 0 = all checks pass)
 */

#include "../src/bgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;

static void ck(const char *name, int cond)
{
    if (cond) {
        pass++;
        printf("PASS  %s\n", name);
    } else {
        fail++;
        printf("FAIL  %s\n", name);
    }
}

static uint32_t rd(int x, int y)
{
    uint32_t v = 0;
    bgfx_read_pixel(x, y, &v);
    return v;
}

int main(void)
{
    /* --- memory backend init ---------------------------------------- */
    ck("memory init", bgfx_init("memory") == 0);
    ck("backend is MEM", bgfx_info()->backend == BGFX_BACKEND_MEM);
    ck("size honours BGFX_MEM_SIZE", bgfx_info()->width == 320 &&
                                     bgfx_info()->height == 200);
    ck("geometry accessors", bgfx_get_width() == 320 &&
                             bgfx_get_height() == 200 &&
                             bgfx_get_pitch() == 320 * 4 &&
                             bgfx_get_bpp() == 32);
    ck("available", bgfx_available() == 1);

    /* --- screen canvas drawing -------------------------------------- */
    bgfx_clear(0xFF0000);
    ck("clear fills corner 0,0", rd(0, 0) == 0xFF0000);
    ck("clear fills far corner", rd(319, 199) == 0xFF0000);
    ck("read out of range", bgfx_read_pixel(400, 199, &(uint32_t){0}) == -1);
    ck("read negative rejected", bgfx_read_pixel(-1, 0, &(uint32_t){0}) == -1);

    bgfx_fill_rect(10, 10, 20, 20, 0x0000FF);
    ck("fill_rect inside", rd(15, 15) == 0x0000FF);
    ck("fill_rect leaves outside", rd(9, 9) == 0xFF0000);
    ck("fill_rect clips right edge", bgfx_fill_rect(310, 190, 100, 100, 0x00FF00) == 0 &&
                                      rd(319, 199) == 0x00FF00);
    ck("put_pixel bounds", bgfx_put_pixel(300, 5, 0x123456) == 0 &&
                           rd(300, 5) == 0x123456 &&
                           bgfx_put_pixel(320, 5, 0x000000) == -1);
    ck("screen sample recorded", rd(15, 15) == 0x0000FF);

    /* --- target surface --------------------------------------------- */
    bgfx_surface_t s1, s2;
    ck("surface create 64x48", bgfx_surface_create(&s1, 64, 48) == 0);
    ck("surface fields", s1.pixels && s1.width == 64 && s1.height == 48 &&
                         s1.pitch == 64 * 4 && s1.bpp == 32 &&
                         s1.format == BGFX_FMT_XRGB8888);
    ck("target surface", bgfx_target_surface(&s1) == 0);
    ck("target reports itself", bgfx_target() &&
                                bgfx_target()->pixels == s1.pixels &&
                                bgfx_target()->width == 64);
    ck("geometry follows target", bgfx_get_width() == 64 && bgfx_get_height() == 48);

    bgfx_clear(0x00FF00);
    ck("clear draws into surface", rd(1, 1) == 0x00FF00);
    ck("clear fills surface far corner", rd(63, 47) == 0x00FF00);

    bgfx_draw_text(4, 4, "Hi", 0xFFFFFF, 0x00FF00);
    int saw_fg = 0, saw_bg = 0;
    for (int y = 4; y < 14; y++)
        for (int x = 4; x < 24; x++) {
            uint32_t v = rd(x, y);
            if (v == 0xFFFFFF) saw_fg = 1;
            if (v == 0x00FF00) saw_bg = 1;
        }
    ck("draw_text fg pixels in surface", saw_fg);
    ck("draw_text bg pixels in surface", saw_bg);

    /* blit an 8x8 red block into the surface at (40,40) */
    ck("second surface create", bgfx_surface_create(&s2, 8, 8) == 0);
    bgfx_target_surface(&s2);
    bgfx_clear(0xFF0000);
    ck("second surface filled", rd(0, 0) == 0xFF0000);
    bgfx_target_surface(&s1);
    bgfx_blit_from(s2.pixels, s2.pitch, 0, 0, 8, 8, 40, 40);
    ck("blit into target surface", rd(44, 44) == 0xFF0000);
    ck("blit respects offset", rd(39, 44) == 0x00FF00);

    /* --- restore to screen ------------------------------------------ */
    ck("target NULL restores screen", bgfx_target_surface(NULL) == 0);
    ck("no active target", bgfx_target() == NULL);
    ck("geometry back to screen", bgfx_get_width() == 320 && bgfx_get_height() == 200);
    /* every screen pixel drawn before targeting must be untouched by the
     * whole surface session (clear, text, blit on two surfaces) */
    ck("screen intact after all surface ops",
       rd(0, 0) == 0xFF0000 && rd(15, 15) == 0x0000FF &&
       rd(300, 5) == 0x123456 && rd(319, 199) == 0x00FF00);

    bgfx_put_pixel(15, 15, 0xFFFFFF);
    ck("screen draws after restore", rd(15, 15) == 0xFFFFFF);

    /* --- invalid input ---------------------------------------------- */
    ck("create rejects zero size", bgfx_surface_create(&s1, 0, 48) == -1);
    ck("create rejects NULL", bgfx_surface_create(NULL, 8, 8) == -1);
    bgfx_surface_t bad;
    memset(&bad, 0, sizeof(bad));
    ck("target rejects empty surface", bgfx_target_surface(&bad) == -1);
    bgfx_surface_destroy(NULL); /* must not crash */
    ck("no target after rejected input", bgfx_target() == NULL);

    /* --- malformed BGFX_MEM_SIZE falls back to default ---------------- */
    bgfx_shutdown();
    ck("shutdown clears available", bgfx_available() == 0);
    ck("read after shutdown fails", bgfx_read_pixel(0, 0, &(uint32_t){0}) == -1);
    setenv("BGFX_MEM_SIZE", "garbage", 1);
    ck("init with bad size", bgfx_init("memory") == 0);
    ck("bad size falls back to 1024x768",
       bgfx_info()->width == 1024 && bgfx_info()->height == 768);
    ck("unknown backend rejected", bgfx_init("wibble") == -1);
    bgfx_shutdown();

    bgfx_surface_destroy(&s1);
    bgfx_surface_destroy(&s2);
    ck("destroyed surfaces cleared", s1.pixels == NULL && s2.pixels == NULL);

    printf("\n== %d passed, %d failed ==\n", pass, fail);
    return fail ? 1 : 0;
}
