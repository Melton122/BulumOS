/*
 * bgfx - BulumOS graphics CLI (Phase 8)
 *
 *   bgfx status              graphics availability summary
 *   bgfx info                detailed detection report
 *   bgfx test                render the test pattern (bgfxdemo)
 *   bgfx input               monitor input devices (binputtest)
 *   bgfx --help              usage
 *   bgfx --version           version
 *
 * Reads /etc/bulum/graphics.conf:
 *   backend=auto|framebuffer|drm     (drm is detection-only in Phase 8)
 *   resolution=auto                  (only "auto" is supported)
 *   input=auto                       (only "auto" is supported)
 *
 * Exit status: 0 = graphics available, 1 = unavailable, 2 = usage error.
 */

#include "bgfx.h"
#include "binput.h"
#include "bgfxcli_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BGFX_VERSION_STR "1.0.0"
#define CONF_PATH "/etc/bulum/graphics.conf"

static void print_detailed(void)
{
    bgfx_info_t info;
    int have = bgfx_detect(&info);
    printf("BulumOS Graphics\n");
    printf("----------------\n");
    if (!have) {
        printf("Graphics: unavailable\n");
        printf("Console mode: active\n");
        return;
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
    default:                printf("Pixel format: %d bpp\n", info.bpp); break;
    }
    printf("Input devices: %d\n", bgfx_count_inputs());
}

static int cmd_status(const char *backend)
{
    bgfx_info_t info;
    int have = bgfx_detect(&info);

    if (backend && strcasecmp(backend, "drm") == 0) {
        printf("Backend: drm (not implemented in Phase 8; framebuffer is primary)\n");
        return 1;
    }

    if (!have) {
        printf("Graphics: unavailable\n");
        printf("Backend: none\n");
        printf("Console mode: active\n");
        return 1;
    }
    printf("Graphics: available\n");
    printf("Backend: %s\n", info.fb_path[0] ? "framebuffer" : "none");
    if (info.fb_path[0])
        printf("Framebuffer: %s\n", info.fb_path);
    if (info.drm_path[0])
        printf("DRM device: %s\n", info.drm_path);
    if (info.width > 0)
        printf("Resolution: %dx%d\n", info.width, info.height);
    printf("Input devices: %d\n", bgfx_count_inputs());
    return 0;
}

static void usage(const char *p)
{
    printf("usage: %s <command> [args]\n"
           "  status            graphics availability summary\n"
           "  info              detailed detection report\n"
           "  test              render the test pattern (bgfxdemo)\n"
           "  input             monitor input devices (binputtest)\n"
           "  --help, --version\n", p);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        printf("bgfx %s (BulumOS Phase 8)\n", BGFX_VERSION_STR);
        return 0;
    }

    char backend[32] = "auto";
    /* test hook: BGFX_CONF overrides the config path */
    const char *conf = getenv("BGFX_CONF");
    if (bgfxcli_read_config(conf && conf[0] ? conf : CONF_PATH,
                            backend, sizeof(backend)) < 0)
        fprintf(stderr, "bgfx: warning: using defaults (%s)\n", CONF_PATH);

    if (strcmp(argv[1], "status") == 0)
        return cmd_status(backend);
    if (strcmp(argv[1], "info") == 0) {
        print_detailed();
        return 0;
    }
    if (strcmp(argv[1], "test") == 0) {
        /* pass through remaining args to bgfxdemo */
        extern char **environ;
        char *a[16];
        int n = 0;
        a[n++] = (char *)"/usr/bin/bgfxdemo";
        if (strcasecmp(backend, "drm") == 0)
            a[n++] = (char *)"--backend";
        if (strcasecmp(backend, "drm") == 0)
            a[n++] = (char *)"drm";
        for (int i = 2; i < argc && n < 14; i++)
            a[n++] = argv[i];
        a[n] = NULL;
        execve(a[0], a, environ);
        perror("execve bgfxdemo");
        return 1;
    }
    if (strcmp(argv[1], "input") == 0) {
        extern char **environ;
        char *a[16];
        int n = 0;
        a[n++] = (char *)"/usr/bin/binputtest";
        for (int i = 2; i < argc && n < 14; i++)
            a[n++] = argv[i];
        a[n] = NULL;
        execve(a[0], a, environ);
        perror("execve binputtest");
        return 1;
    }
    usage(argv[0]);
    return 2;
}