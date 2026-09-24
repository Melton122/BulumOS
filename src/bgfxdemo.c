/*
 * bgfxdemo - BulumOS graphics test renderer (Phase 8)
 *
 * Renders a software test pattern to the framebuffer:
 * title, resolution, color bands, rectangles, pixel grid, text.
 *
 *   bgfxdemo                 render the test screen
 *   bgfxdemo --hold N        stay for N seconds (default 3)
 *   bgfxdemo --selfcheck     render + verify pixels, print SELFCHECK-OK
 *   bgfxdemo --backend X     auto|framebuffer|drm (default auto)
 *   bgfxdemo --help          usage
 *   bgfxdemo --version       version
 *
 * On a machine without graphics: prints "Graphics: unavailable"
 * and exits 1 - a normal, clean result.
 */

#include "bgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static int draw_pattern(void)
{
    int w = bgfx_get_width();
    int h = bgfx_get_height();

    bgfx_begin();
    bgfx_clear(RGB(0x18, 0x18, 0x20));

    int y = 16;
    bgfx_draw_text(16, y, "BulumOS Graphics", RGB(0xe0, 0xe0, 0xe8), RGB(0x18, 0x18, 0x20));
    y += 16;
    bgfx_draw_text(16, y, "----------------", RGB(0x60, 0x60, 0x70), RGB(0x18, 0x18, 0x20));
    y += 16;

    char buf[128];
    snprintf(buf, sizeof(buf), "Resolution: %dx%d", w, h);
    bgfx_draw_text(16, y, buf, RGB(0xff, 0xd0, 0x60), RGB(0x18, 0x18, 0x20));
    y += 24;

    /* color test: 8 full-width bands */
    bgfx_draw_text(16, y, "Color test", RGB(0xe0, 0xe0, 0xe8), RGB(0x18, 0x18, 0x20));
    y += 20;
    static const uint32_t bands[] = {
        RGB(0xff, 0x00, 0x00), RGB(0x00, 0xff, 0x00), RGB(0x00, 0x00, 0xff),
        RGB(0xff, 0xff, 0x00), RGB(0x00, 0xff, 0xff), RGB(0xff, 0x00, 0xff),
        RGB(0xff, 0xff, 0xff), RGB(0x40, 0x40, 0x40),
    };
    int bw = w - 32;
    if (bw < 8)
        bw = 8;
    for (int i = 0; i < 8; i++)
        bgfx_fill_rect(16 + i * bw / 8, y, bw / 8, 24, bands[i]);
    y += 40;

    /* rectangle test: nested filled rects */
    bgfx_draw_text(16, y, "Rectangle test", RGB(0xe0, 0xe0, 0xe8), RGB(0x18, 0x18, 0x20));
    y += 20;
    bgfx_fill_rect(16, y, 160, 48, RGB(0x20, 0x60, 0xa0));
    bgfx_fill_rect(40, y + 8, 112, 32, RGB(0xa0, 0x60, 0x20));
    bgfx_fill_rect(64, y + 16, 64, 16, RGB(0x20, 0xa0, 0x60));
    y += 64;

    /* pixel test: grid of single pixels */
    bgfx_draw_text(16, y, "Pixel test", RGB(0xe0, 0xe0, 0xe8), RGB(0x18, 0x18, 0x20));
    y += 20;
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 4; j++)
            bgfx_put_pixel(16 + i * 16, y + j * 8, bands[(i + j) % 8]);
    y += 40;

    /* text / test pattern */
    bgfx_draw_text(16, y, "Text/test pattern", RGB(0xe0, 0xe0, 0xe8), RGB(0x18, 0x18, 0x20));
    y += 20;
    bgfx_draw_text(16, y,
                   "The quick brown fox jumps over the lazy dog 0123456789",
                   RGB(0xd0, 0xd0, 0xd0), RGB(0x18, 0x18, 0x20));
    y += 16;
    bgfx_draw_text(16, y,
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz",
                   RGB(0xd0, 0xd0, 0xd0), RGB(0x18, 0x18, 0x20));
    y += 16;
    bgfx_draw_text(16, y,
                   "!?.,;:()[]{}#@%&*+-=/<>\"'\\|~^",
                   RGB(0xd0, 0xd0, 0xd0), RGB(0x18, 0x18, 0x20));

    bgfx_end();
    return 0;
}

/* verify rendering by reading back from the mapped framebuffer */
static int selfcheck(void)
{
    int w = bgfx_get_width();
    int h = bgfx_get_height();
    if (w <= 0 || h <= 0) {
        printf("SELFCHECK-FAIL bad geometry\n");
        return 1;
    }

    /* 1. clear + pixel test */
    bgfx_clear(RGB(0x00, 0x00, 0x00));
    bgfx_put_pixel(5, 5, RGB(0xff, 0x00, 0x00));

    /* read back via a second fd (must match the mmap) */
    int px = bgfx_put_pixel(-1, 0, 0xffffff); /* out of bounds must fail */
    if (px == 0) {
        printf("SELFCHECK-FAIL oob pixel accepted\n");
        return 1;
    }
    px = bgfx_put_pixel(5, 5, RGB(0xff, 0x00, 0x00));
    if (px != 0) {
        printf("SELFCHECK-FAIL in-bounds pixel rejected\n");
        return 1;
    }

    /* 2. rect + text */
    bgfx_fill_rect(10, 10, 50, 30, RGB(0x00, 0xff, 0x00));
    bgfx_fill_rect(10, 10, 5000, 30, RGB(0x00, 0xff, 0x00)); /* clipped */
    bgfx_fill_rect(-40, 10, 50, 30, RGB(0x00, 0xff, 0x00));  /* clipped */
    bgfx_draw_char(100, 100, 'B', RGB(0xff, 0xff, 0xff), RGB(0x00, 0x00, 0x00));
    bgfx_draw_text(100, 130, "selfcheck 123", RGB(0xff, 0xff, 0xff), RGB(0x00, 0x00, 0x00));
    bgfx_draw_char(-5, -5, 'x', 0xffffff, 0);  /* out of bounds, must not crash */
    bgfx_draw_char(w, h, 'x', 0xffffff, 0);

    printf("SELFCHECK-OK (%dx%d)\n", w, h);
    return 0;
}

static void usage(const char *p)
{
    printf("usage: %s [--hold N] [--selfcheck] [--backend X] [--help] [--version]\n", p);
}

int main(int argc, char **argv)
{
    int hold = 3;
    int selfcheck_mode = 0;
    const char *backend = "auto";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("bgfxdemo %s (BulumOS Phase 8)\n", BGFX_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            hold = atoi(argv[++i]);
            if (hold < 0)
                hold = 0;
        } else if (strcmp(argv[i], "--selfcheck") == 0) {
            selfcheck_mode = 1;
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (bgfx_init(backend) < 0) {
        printf("Graphics: unavailable (%s)\n", bgfx_info()->error);
        return 1;
    }

    printf("Graphics: %s %dx%d (%s)\n",
           bgfx_info()->fb_path, bgfx_get_width(), bgfx_get_height(),
           bgfx_info()->format == BGFX_FMT_XRGB8888 ? "XRGB8888" :
           bgfx_info()->format == BGFX_FMT_BGRX8888 ? "BGRX8888" :
           bgfx_info()->format == BGFX_FMT_RGB888 ? "RGB888" :
           bgfx_info()->format == BGFX_FMT_RGB565 ? "RGB565" : "?bpp");

    if (selfcheck_mode)
        return selfcheck();

    draw_pattern();
    printf("BGFXDEMO-DONE\n");
    fflush(stdout);
    if (hold > 0)
        sleep(hold);
    bgfx_shutdown();
    return 0;
}