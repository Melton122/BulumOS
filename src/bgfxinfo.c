/*
 * bgfxinfo - BulumOS graphics device detection (Phase 8)
 *
 * Reports what graphics/input hardware is present.  "Graphics:
 * unavailable" on a headless machine is a normal result, not an error.
 *
 *   bgfxinfo            full report
 *   bgfxinfo --help     usage
 *   bgfxinfo --version  version
 */

#include "bgfx.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_input(void)
{
    DIR *d = opendir("/dev/input");
    if (!d) {
        printf("Input devices: none\n");
        return;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        /* name via /sys/class/input/eventX/device/../inputY/name */
        char path[256], name[128] = "unknown";
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(name, sizeof(name), f))
                name[strcspn(name, "\n")] = 0;
            fclose(f);
        }
        printf("Input devices: /dev/input/%s (%s)\n", e->d_name, name);
        n++;
    }
    closedir(d);
    if (n == 0)
        printf("Input devices: none\n");
}

static int report(void)
{
    bgfx_info_t info;
    int have = bgfx_detect(&info);

    printf("BulumOS Graphics\n");
    printf("----------------\n");
    if (!have) {
        printf("Graphics: unavailable\n");
        printf("Console mode: active\n");
        if (info.error[0])
            printf("Note: %s\n", info.error);
        return 1;
    }
    printf("GPU: %s\n", info.gpu[0] ? info.gpu : "(unknown)");
    printf("Driver: %s\n", info.driver[0] ? info.driver : "(unknown)");
    printf("DRM device: %s\n", info.drm_path[0] ? info.drm_path : "(none)");
    printf("Framebuffer: %s\n", info.fb_path[0] ? info.fb_path : "(none)");
    if (info.width > 0)
        printf("Resolution: %dx%d\n", info.width, info.height);
    else
        printf("Resolution: (unknown)\n");
    switch (info.format) {
    case BGFX_FMT_XRGB8888: printf("Pixel format: XRGB8888 (%d bpp)\n", info.bpp); break;
    case BGFX_FMT_BGRX8888: printf("Pixel format: BGRX8888 (%d bpp)\n", info.bpp); break;
    case BGFX_FMT_RGB888:   printf("Pixel format: RGB888 (%d bpp)\n", info.bpp); break;
    case BGFX_FMT_RGB565:   printf("Pixel format: RGB565 (%d bpp)\n", info.bpp); break;
    default:
        printf("Pixel format: %d bpp\n", info.bpp);
        break;
    }
    print_input();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("usage: bgfxinfo [--help] [--version]\n"
               "Reports BulumOS graphics and input hardware detection.\n"
               "Exit status: 0 = graphics available, 1 = unavailable.\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("bgfxinfo %s (BulumOS Phase 8)\n", BGFX_VERSION);
        return 0;
    }
    if (argc != 1) {
        printf("usage: bgfxinfo [--help] [--version]\n");
        return 2;
    }
    return report();
}