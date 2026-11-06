/*
 * bgfx.c - BulumOS graphics abstraction (Phase 8, extended Phase 13)
 *
 * Backends:
 *   framebuffer: /dev/fb0 via open/ioctl/mmap.  Primary backend.
 *   drm:         /dev/dri/card0 with DRM/KMS mode resource scan, dumb
 *                buffer, addfb and setcrtc (software scanout, Phase 13).
 *   mem:         headless software surface (no device node; unit tests).
 *
 * Phase 13 renderer-surface indirection: every drawing primitive renders
 * into the "active canvas" - either the screen (framebuffer/DRM/mem) or a
 * caller-owned target surface (e.g. a Wayland wl_shm buffer).  This lets
 * the compositor and the client share one drawing core and keeps the
 * future compositor-protocol swap local to this file.
 *
 * Safety (Phase 8 stage 18):
 *   - all pixel offsets are bounds-checked against width/height
 *   - pitch is validated against width*bpp/8 at init
 *   - pixel formats other than 16/24/32 bpp with sane channel
 *     offsets are rejected with a useful error
 *   - no dynamic allocation; fixed-size structs only
 */

#define _GNU_SOURCE

/* kernel UAPI headers (vendored in src/uapi/drm) use kernel-only markers */
#ifndef __user
#define __user
#endif
#ifndef __kernel
#define __kernel
#endif

#include "bgfx.h"
#include "bgfx_font.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include "uapi/drm/drm.h"
#include "uapi/drm/drm_mode.h"
#include "uapi/drm/drm_fourcc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FB_PATH "/dev/fb0"
#define DRM_PATH "/dev/dri/card0"
#define SYS_DRM "/sys/class/drm"
#define SYS_GRAPHICS "/sys/class/graphics"
#define SYS_INPUT "/sys/class/input"

/* ---------- renderer state ---------- */

static bgfx_info_t g_info;
static int g_fd = -1;
static uint8_t *g_map = NULL;
static size_t g_map_len = 0;

/* screen canvas (framebuffer mmap, DRM dumb buffer, or mem surface) */
typedef struct {
    uint8_t *px;
    int w, h;
    int pitch;
    int bytes_pp;
} canvas_t;
static canvas_t g_scr = { 0, 0, 0, 0, 4 };

/* Phase 13 target surface: when active, primitives render here */
static bgfx_surface_t g_tgt;
static int g_tgt_active = 0;

/* owned heap buffer for the headless memory backend */
static uint8_t *g_mem = NULL;

/* channel packing for the current format */
typedef struct {
    int r_off, r_len;
    int g_off, g_len;
    int b_off, b_len;
    int bytes_pp; /* 2, 3 or 4 */
} pixfmt_t;

static pixfmt_t g_fmt;

/* saved channel packing so we can restore the screen format when the
 * target surface is cleared */
static pixfmt_t g_fmt_saved;

static canvas_t canvas(void)
{
    if (g_tgt_active && g_tgt.pixels)
        return (canvas_t){ g_tgt.pixels, g_tgt.width, g_tgt.height, g_tgt.pitch, 4 };
    return g_scr;
}

/* ---------- helpers ---------- */

static void set_error(bgfx_info_t *info, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(info->error, sizeof(info->error), fmt, ap);
    va_end(ap);
}

static char *read_sysfs(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    return buf;
}

/* driver name for a drm card: /sys/class/drm/card0/device/driver -> basename */
static void card_driver(bgfx_info_t *info)
{
    char path[256], buf[256];
    snprintf(path, sizeof(path), "%s/card0/device/driver", SYS_DRM);
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const char *b = strrchr(buf, '/');
        snprintf(info->driver, sizeof(info->driver), "%.*s",
                 (int)sizeof(info->driver) - 1, b ? b + 1 : buf);
    }
    /* gpu name: prefer uevent DRIVER= line, prettified */
    snprintf(path, sizeof(path), "%s/card0/device/uevent", SYS_DRM);
    if (read_sysfs(path, buf, sizeof(buf))) {
        char *d = strstr(buf, "DRIVER=");
        if (d) {
            snprintf(info->gpu, sizeof(info->gpu), "%s", d + 7);
            /* virtio_gpu -> virtio-gpu for display */
            for (char *p = info->gpu; *p; p++)
                if (*p == '_')
                    *p = '-';
        }
    }
    if (!info->gpu[0])
        snprintf(info->gpu, sizeof(info->gpu), "%s", info->driver);
}

/* ---------- detection (no backend opened) ---------- */

int bgfx_detect(bgfx_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->backend = BGFX_BACKEND_NONE;

    /* DRM card? */
    char path[256], buf[256];
    snprintf(path, sizeof(path), "%s/card0", SYS_DRM);
    struct stat st;
    if (stat(path, &st) == 0) {
        snprintf(info->drm_path, sizeof(info->drm_path), "%s", DRM_PATH);
        card_driver(info);
    }

    /* framebuffer? */
    int have_fb = 0;
    if (stat(FB_PATH, &st) == 0) {
        snprintf(info->fb_path, sizeof(info->fb_path), "%s", FB_PATH);
        have_fb = 1;
    } else {
        /* also accept a DRM fbdev-emulated fb, e.g. /sys/class/graphics/fb0 */
        snprintf(path, sizeof(path), "%s/fb0", SYS_GRAPHICS);
        if (stat(path, &st) == 0 && stat(FB_PATH, &st) != 0)
            snprintf(info->error, sizeof(info->error),
                     "framebuffer device node missing (fb0 present in sysfs)");
    }

    /* resolution via sysfs (works without opening the device) */
    if (have_fb) {
        snprintf(path, sizeof(path), "%s/fb0/virtual_size", SYS_GRAPHICS);
        if (read_sysfs(path, buf, sizeof(buf))) {
            int w = 0, h = 0;
            if (sscanf(buf, "%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
                info->width = w;
                info->height = h;
            }
        }
        snprintf(path, sizeof(path), "%s/fb0/bits_per_pixel", SYS_GRAPHICS);
        if (read_sysfs(path, buf, sizeof(buf)))
            info->bpp = atoi(buf);
        snprintf(path, sizeof(path), "%s/fb0/name", SYS_GRAPHICS);
        if (read_sysfs(path, buf, sizeof(buf))) {
            if (!info->gpu[0])
                snprintf(info->gpu, sizeof(info->gpu), "%.*s",
                         (int)sizeof(info->gpu) - 1, buf);
        }
    }

    if (have_fb) {
        info->backend = BGFX_BACKEND_FB;
        info->format = BGFX_FMT_UNKNOWN;
        info->pitch = info->width * (info->bpp / 8); /* best guess; real value from ioctl */
    }
    return have_fb || info->drm_path[0];
}

int bgfx_count_inputs(void)
{
    DIR *d = opendir("/dev/input");
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) == 0)
            n++;
    }
    closedir(d);
    return n;
}

/* ---------- format handling ---------- */

static int channel(int off, int len)
{
    return (off >= 0 && off < 32 && len > 0 && len <= 8 && off + len <= 32) ? 1 : 0;
}

/* derive channel layout from fb_var_screeninfo, validate it */
static int setup_format(bgfx_info_t *info, const struct fb_var_screeninfo *v)
{
    g_fmt.r_off = v->red.offset;
    g_fmt.r_len = v->red.length;
    g_fmt.g_off = v->green.offset;
    g_fmt.g_len = v->green.length;
    g_fmt.b_off = v->blue.offset;
    g_fmt.b_len = v->blue.length;

    if (!channel(g_fmt.r_off, g_fmt.r_len) ||
        !channel(g_fmt.g_off, g_fmt.g_len) ||
        !channel(g_fmt.b_off, g_fmt.b_len)) {
        set_error(info, "unsupported pixel format: %d bpp red(%d,%d) green(%d,%d) blue(%d,%d)",
                  v->bits_per_pixel, g_fmt.r_off, g_fmt.r_len,
                  g_fmt.g_off, g_fmt.g_len, g_fmt.b_off, g_fmt.b_len);
        return -1;
    }

    switch (v->bits_per_pixel) {
    case 16: g_fmt.bytes_pp = 2; break;
    case 24: g_fmt.bytes_pp = 3; break;
    case 32: g_fmt.bytes_pp = 4; break;
    default:
        set_error(info, "unsupported bits-per-pixel: %d (supported: 16/24/32)",
                  v->bits_per_pixel);
        return -1;
    }

    info->bpp = v->bits_per_pixel;
    info->pitch = v->xres_virtual * g_fmt.bytes_pp;
    if (v->xres_virtual > 0)
        info->pitch = v->xres_virtual * g_fmt.bytes_pp;
    else
        info->pitch = v->xres * g_fmt.bytes_pp;

    /* validate pitch: at least one full visible line must fit */
    if (info->pitch < info->width * g_fmt.bytes_pp) {
        set_error(info, "invalid pitch %d for %dx%d %d-bpp framebuffer",
                  info->pitch, info->width, info->height, v->bits_per_pixel);
        return -1;
    }

    if (v->bits_per_pixel == 32 && g_fmt.r_off == 16 && g_fmt.b_off == 0)
        info->format = BGFX_FMT_XRGB8888;
    else if (v->bits_per_pixel == 32 && g_fmt.r_off == 0 && g_fmt.b_off == 16)
        info->format = BGFX_FMT_BGRX8888;
    else if (v->bits_per_pixel == 24)
        info->format = BGFX_FMT_RGB888;
    else if (v->bits_per_pixel == 16 && g_fmt.r_len == 5 && g_fmt.g_len == 6)
        info->format = BGFX_FMT_RGB565;
    else
        info->format = BGFX_FMT_UNKNOWN; /* generic offsets still work */

    return 0;
}

/* convert 0xRRGGBB to the device pixel word */
static uint32_t pack_pixel(uint32_t color)
{
    uint32_t r = (color >> 16) & 0xff;
    uint32_t g = (color >> 8) & 0xff;
    uint32_t b = color & 0xff;
    uint32_t v = 0;
    v |= ((r >> (8 - g_fmt.r_len)) & ((1u << g_fmt.r_len) - 1)) << g_fmt.r_off;
    v |= ((g >> (8 - g_fmt.g_len)) & ((1u << g_fmt.g_len) - 1)) << g_fmt.g_off;
    v |= ((b >> (8 - g_fmt.b_len)) & ((1u << g_fmt.b_len) - 1)) << g_fmt.b_off;
    return v;
}

/* ---------- lifecycle ---------- */

/* ---------- DRM/KMS backend (Phase 13) ----------------------------------
 * Software scanout: enumerate mode resources, create a dumb 32bpp buffer,
 * addfb it and program the CRTC.  The dumb buffer stays mapped, so draws
 * into the screen canvas are scanned out directly - no page flip is needed
 * on the single-buffer path.
 *
 * Written directly against the kernel UAPI headers vendored in
 * src/uapi/drm (struct drm_mode_card_res, drm_mode_get_connector,
 * drm_mode_get_blob, drm_mode_crtc, drm_mode_fb_cmd).  DRM_FORMAT_XRGB8888
 * is documented in drm_fourcc.h; dumb buffers are always 32bpp here. */

static int bgfx_drm_try_mode(int fd, uint32_t crtc_id, uint32_t conn_id,
                             const struct drm_mode_modeinfo *mode)
{
    int w = mode->hdisplay;
    int h = mode->vdisplay;
    if (w <= 0 || h <= 0)
        return -1;

    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = (uint32_t)w;
    create.height = (uint32_t)h;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return -1;

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        return -1;

    /* use the stride the driver allocated, not width*4 */
    int pitch = create.pitch ? (int)create.pitch : w * 4;
    if (pitch < w * 4)
        pitch = w * 4;
    size_t need = (size_t)pitch * (size_t)h;
    if (create.size && (size_t)create.size > need)
        need = (size_t)create.size;
    uint8_t *px = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                       (off_t)map.offset);
    if (px == MAP_FAILED)
        return -1;

    struct drm_mode_fb_cmd fb;
    memset(&fb, 0, sizeof(fb));
    fb.width = (uint32_t)w;
    fb.height = (uint32_t)h;
    fb.pitch = (uint32_t)pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = create.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        munmap(px, need);
        return -1;
    }

    /* setcrtc: modern UAPI takes a drm_mode_crtc with a connector id array */
    uint32_t connectors[1];
    connectors[0] = conn_id;
    struct drm_mode_crtc setc;
    memset(&setc, 0, sizeof(setc));
    setc.set_connectors_ptr = (uint64_t)(uintptr_t)connectors;
    setc.count_connectors = 1;
    setc.crtc_id = crtc_id;
    setc.fb_id = fb.fb_id;
    setc.x = 0;
    setc.y = 0;
    setc.mode_valid = 1;
    setc.mode = *mode;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &setc) < 0) {
        munmap(px, need);
        return -1;
    }

    g_fd = fd;
    g_map = px;
    g_map_len = need;
    g_info.backend = BGFX_BACKEND_DRM;
    g_info.width = w;
    g_info.height = h;
    g_info.pitch = pitch;
    g_info.bpp = 32;
    g_info.format = BGFX_FMT_XRGB8888;
    g_fmt.r_off = 16; g_fmt.r_len = 8;
    g_fmt.g_off = 8;  g_fmt.g_len = 8;
    g_fmt.b_off = 0;  g_fmt.b_len = 8;
    g_fmt.bytes_pp = 4;
    g_scr.px = px;
    g_scr.w = w;
    g_scr.h = h;
    g_scr.pitch = pitch;
    g_scr.bytes_pp = 4;
    return 0;
}

static int bgfx_init_drm(void)
{
    const char *path = DRM_PATH;
    const char *alt = getenv("BGFX_DRM_PATH");
    if (alt && alt[0])
        path = alt;

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        set_error(&g_info, "drm card %s unavailable: %s", path, strerror(errno));
        return -1;
    }
    snprintf(g_info.drm_path, sizeof(g_info.drm_path), "%s", path);

    /* ---- mode resources: query counts, allocate, then fill ---- */
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        set_error(&g_info, "DRM_IOCTL_MODE_GETRESOURCES failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    int nc = (int)res.count_connectors;
    int nk = (int)res.count_crtcs;
    int ne = (int)res.count_encoders;
    if (nc <= 0 || nk <= 0) {
        set_error(&g_info, "DRM card exposes no connectors/crtcs");
        close(fd);
        return -1;
    }

    uint32_t *conn_ids = calloc((size_t)nc, sizeof(uint32_t));
    uint32_t *crtc_ids = calloc((size_t)nk, sizeof(uint32_t));
    uint32_t *enc_ids = calloc((size_t)ne ? (size_t)ne : 1, sizeof(uint32_t));
    if (!conn_ids || !crtc_ids || !enc_ids) {
        set_error(&g_info, "drm resource allocation failed");
        free(conn_ids); free(crtc_ids); free(enc_ids);
        close(fd);
        return -1;
    }

    memset(&res, 0, sizeof(res));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        set_error(&g_info, "DRM_IOCTL_MODE_GETRESOURCES (ids) failed: %s", strerror(errno));
        free(conn_ids); free(crtc_ids); free(enc_ids);
        close(fd);
        return -1;
    }

    /* prefer a crtc that is already scanning out something, else the first */
    uint32_t crtc_id = 0;
    for (int i = 0; i < nk; i++) {
        struct drm_mode_crtc cur;
        memset(&cur, 0, sizeof(cur));
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &(struct drm_mode_crtc){ .crtc_id = crtc_ids[i] }) < 0)
            continue;
        if (cur.fb_id) { crtc_id = crtc_ids[i]; break; }
        if (!crtc_id) crtc_id = crtc_ids[i];
    }
    if (!crtc_id)
        crtc_id = crtc_ids[0];

    /* walk connectors: take the first mode of each until one programs ok */
    int rc = -1;
    for (int i = 0; i < nc && rc < 0; i++) {
        struct drm_mode_get_connector gc;
        memset(&gc, 0, sizeof(gc));
        gc.connector_id = conn_ids[i];
        /* first call reports count_modes/count_encoders */
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
            continue;
        if (gc.count_modes == 0 || gc.count_modes > 512)
            continue;

        struct drm_mode_modeinfo *modes =
            calloc(gc.count_modes, sizeof(struct drm_mode_modeinfo));
        if (!modes)
            continue;
        memset(&gc, 0, sizeof(gc));
        gc.connector_id = conn_ids[i];
        gc.modes_ptr = (uint64_t)(uintptr_t)modes;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) {
            free(modes);
            continue;
        }

        /* mode[0] is the driver-preferred mode; try it, then the rest */
        int attempts = gc.count_modes < 4 ? gc.count_modes : 4;
        for (int m = 0; m < attempts && rc < 0; m++)
            rc = bgfx_drm_try_mode(fd, crtc_id, conn_ids[i], &modes[m]);
        free(modes);
    }

    free(conn_ids); free(crtc_ids); free(enc_ids);

    if (rc < 0) {
        set_error(&g_info, "no DRM connector accepted a dumb-buffer mode");
        close(fd);
        return -1;
    }

    card_driver(&g_info);
    return 0;
}

int bgfx_init(const char *backend)
{
    const char *want = backend ? backend : "auto";

    memset(&g_info, 0, sizeof(g_info));
    g_info.backend = BGFX_BACKEND_NONE;
    g_scr.px = NULL; g_scr.w = 0; g_scr.h = 0; g_scr.pitch = 0; g_scr.bytes_pp = 4;

    if (strcasecmp(want, "drm") == 0) {
        if (bgfx_init_drm() == 0)
            return 0;
        return -1;
    }
    if (strcasecmp(want, "memory") == 0 || strcasecmp(want, "mem") == 0) {
        /* headless software surface (no device node) - unit tests + selftest */
        int w = 1024, h = 768;
        const char *gs = getenv("BGFX_MEM_SIZE");
        if ((!gs || sscanf(gs, "%dx%d", &w, &h) != 2) || w <= 0 || h <= 0) {
            w = 1024; h = 768;
        }
        g_mem = calloc(1, (size_t)w * 4 * (size_t)h);
        if (!g_mem) {
            set_error(&g_info, "memory backend: surface alloc failed");
            return -1;
        }
        g_scr.px = g_mem;
        g_scr.w = w;
        g_scr.h = h;
        g_scr.pitch = w * 4;
        g_scr.bytes_pp = 4;
        g_info.backend = BGFX_BACKEND_MEM;
        g_info.width = w;
        g_info.height = h;
        g_info.pitch = w * 4;
        g_info.bpp = 32;
        g_info.format = BGFX_FMT_XRGB8888;
        g_fmt.r_off = 16; g_fmt.r_len = 8;
        g_fmt.g_off = 8;  g_fmt.g_len = 8;
        g_fmt.b_off = 0;  g_fmt.b_len = 8;
        g_fmt.bytes_pp = 4;
        snprintf(g_info.gpu, sizeof(g_info.gpu), "memory");
        return 0;
    }
    if (strcasecmp(want, "framebuffer") != 0 && strcasecmp(want, "auto") != 0) {
        set_error(&g_info, "unknown backend '%s' (supported: auto, framebuffer, drm, memory)", want);
        return -1;
    }

    /* auto: prefer DRM/KMS when a card exists, else framebuffer */
    if (strcasecmp(want, "auto") == 0) {
        struct stat st;
        if (stat(DRM_PATH, &st) == 0 || (getenv("BGFX_DRM_PATH") &&
                                         stat(getenv("BGFX_DRM_PATH"), &st) == 0)) {
            if (bgfx_init_drm() == 0)
                return 0;
            /* fall through to framebuffer */
        }
    }

    /* test hook: BGFX_FB_PATH overrides the device path */
    const char *fbpath = FB_PATH;
    const char *alt = getenv("BGFX_FB_PATH");
    if (alt && alt[0])
        fbpath = alt;
    int fd = open(fbpath, O_RDWR);
    if (fd < 0) {
        bgfx_detect(&g_info);
        set_error(&g_info, "framebuffer %s unavailable: %s", fbpath, strerror(errno));
        return -1;
    }

    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        set_error(&g_info, "FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        set_error(&g_info, "FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_info.backend = BGFX_BACKEND_FB;
    g_info.width = var.xres;
    g_info.height = var.yres;
    snprintf(g_info.fb_path, sizeof(g_info.fb_path), "%s", fbpath);
    if (setup_format(&g_info, &var) < 0) {
        close(fd);
        return -1;
    }

    /* mmap exactly the visible area (yres, not yres_virtual) */
    size_t len = (size_t)g_info.pitch * (size_t)g_info.height;
    if (len == 0 || len > (size_t)fix.smem_len) {
        set_error(&g_info, "framebuffer size mismatch: pitch*height=%zu smem_len=%u",
                  len, fix.smem_len);
        close(fd);
        return -1;
    }
    uint8_t *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        set_error(&g_info, "mmap framebuffer failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    g_fd = fd;
    g_map = map;
    g_map_len = len;

    /* attach detection extras (drm path, gpu name) */
    bgfx_info_t det;
    if (bgfx_detect(&det)) {
        if (!g_info.drm_path[0])
            snprintf(g_info.drm_path, sizeof(g_info.drm_path), "%s", det.drm_path);
        if (!g_info.gpu[0])
            snprintf(g_info.gpu, sizeof(g_info.gpu), "%s", det.gpu);
        if (!g_info.driver[0])
            snprintf(g_info.driver, sizeof(g_info.driver), "%s", det.driver);
    }
    g_scr.px = map;
    g_scr.w = g_info.width;
    g_scr.h = g_info.height;
    g_scr.pitch = g_info.pitch;
    g_scr.bytes_pp = g_fmt.bytes_pp;
    return 0;
}

void bgfx_shutdown(void)
{
    if (g_map) {
        munmap(g_map, g_map_len);
        g_map = NULL;
        g_map_len = 0;
    }
    if (g_mem) {
        free(g_mem);
        g_mem = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_scr.px = NULL; g_scr.w = 0; g_scr.h = 0; g_scr.pitch = 0; g_scr.bytes_pp = 4;
    g_tgt.pixels = NULL;
    g_tgt_active = 0;
    g_info.backend = BGFX_BACKEND_NONE;
    memset(&g_info, 0, sizeof(g_info));
}

int bgfx_available(void)
{
    return g_info.backend != BGFX_BACKEND_NONE &&
           (g_scr.px != NULL || (g_tgt_active && g_tgt.pixels != NULL));
}

const bgfx_info_t *bgfx_info(void)
{
    return &g_info;
}

int bgfx_get_width(void)
{
    return g_tgt_active ? g_tgt.width : g_info.width;
}
int bgfx_get_height(void)
{
    return g_tgt_active ? g_tgt.height : g_info.height;
}
int bgfx_get_pitch(void)
{
    return g_tgt_active ? g_tgt.pitch : g_info.pitch;
}
int bgfx_get_bpp(void)
{
    return g_tgt_active ? g_tgt.bpp : g_info.bpp;
}

/* ---- Phase 13 renderer surfaces -------------------------------------- */

int bgfx_surface_create(bgfx_surface_t *s, int w, int h)
{
    if (!s || w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return -1;
    size_t pitch = (size_t)w * 4;
    size_t len = pitch * (size_t)h;
    s->pixels = calloc(1, len);
    if (!s->pixels)
        return -1;
    s->width = w;
    s->height = h;
    s->pitch = (int)pitch;
    s->bpp = 32;
    s->format = BGFX_FMT_XRGB8888;
    return 0;
}

void bgfx_surface_destroy(bgfx_surface_t *s)
{
    if (!s)
        return;
    free(s->pixels);
    s->pixels = NULL;
    s->width = s->height = s->pitch = s->bpp = s->format = 0;
}

int bgfx_target_surface(bgfx_surface_t *s)
{
    if (!s) {
        if (g_tgt_active) {
            g_tgt.pixels = NULL;
            g_tgt_active = 0;
            g_fmt = g_fmt_saved; /* restore screen packing */
        }
        return 0;
    }
    if (!s->pixels || s->bpp != 32)
        return -1;
    if (g_tgt_active) {
        g_tgt_active = 0;
        g_fmt = g_fmt_saved;
    }
    g_fmt_saved = g_fmt;
    g_tgt = *s;
    g_tgt_active = 1;
    /* target surfaces are always XRGB8888 regardless of screen format */
    g_fmt.r_off = 16; g_fmt.r_len = 8;
    g_fmt.g_off = 8;  g_fmt.g_len = 8;
    g_fmt.b_off = 0;  g_fmt.b_len = 8;
    g_fmt.bytes_pp = 4;
    return 0;
}

const bgfx_surface_t *bgfx_target(void)
{
    return g_tgt_active ? &g_tgt : NULL;
}

int bgfx_read_pixel(int x, int y, uint32_t *out)
{
    canvas_t c = canvas();
    if (!c.px || !out)
        return -1;
    if (x < 0 || y < 0 || x >= c.w || y >= c.h)
        return -1;
    const uint8_t *p = c.px + (size_t)y * (size_t)c.pitch +
                       (size_t)x * (size_t)c.bytes_pp;
    uint32_t v = 0;
    if (c.bytes_pp == 4)
        memcpy(&v, p, 4);
    else if (c.bytes_pp == 2) {
        uint16_t t;
        memcpy(&t, p, 2);
        v = t;
    } else
        v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);

    uint32_t r = (v >> g_fmt.r_off) & (g_fmt.r_len >= 32 ? 0xffffffffu
                                                         : ((1u << g_fmt.r_len) - 1));
    uint32_t g = (v >> g_fmt.g_off) & (g_fmt.g_len >= 32 ? 0xffffffffu
                                                         : ((1u << g_fmt.g_len) - 1));
    uint32_t b = (v >> g_fmt.b_off) & (g_fmt.b_len >= 32 ? 0xffffffffu
                                                         : ((1u << g_fmt.b_len) - 1));
    if (g_fmt.r_len < 8) r <<= (8 - g_fmt.r_len);
    if (g_fmt.g_len < 8) g <<= (8 - g_fmt.g_len);
    if (g_fmt.b_len < 8) b <<= (8 - g_fmt.b_len);
    *out = (r << 16) | (g << 8) | b;
    return 0;
}

uint32_t bgfx_pack(uint32_t color)
{
    return pack_pixel(color);
}

void bgfx_blit_from(const void *buf, int pitch,
                    int sx, int sy, int w, int h, int dx, int dy)
{
    if (!buf || w <= 0 || h <= 0)
        return;
    canvas_t c = canvas();
    if (!c.px || !bgfx_available())
        return;
    /* clip the source region to the destination */
    if (sx < 0) { w += sx; sx = 0; dx = 0; }
    if (sy < 0) { h += sy; sy = 0; dy = 0; }
    if (w <= 0 || h <= 0)
        return;
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx >= c.w || dy >= c.h || w <= 0 || h <= 0)
        return;
    if (w > c.w - dx)  w = c.w - dx;
    if (h > c.h - dy) h = c.h - dy;

    const uint8_t *src = (const uint8_t *)buf + (size_t)sy * (size_t)pitch +
                         (size_t)sx * (size_t)c.bytes_pp;
    for (int row = 0; row < h; row++) {
        uint8_t *dst = c.px + (size_t)(dy + row) * (size_t)c.pitch +
                       (size_t)dx * (size_t)c.bytes_pp;
        memcpy(dst, src, (size_t)w * (size_t)c.bytes_pp);
        src += pitch;
    }
}

/* ---------- rendering ---------- */

void bgfx_begin(void) { }
void bgfx_end(void)   { }

static void put_pixel_raw(canvas_t c, int x, int y, uint32_t px)
{
    uint8_t *p = c.px + (size_t)y * (size_t)c.pitch + (size_t)x * (size_t)c.bytes_pp;
    if (c.bytes_pp == 4) {
        memcpy(p, &px, 4);
    } else if (c.bytes_pp == 2) {
        uint16_t v = (uint16_t)px;
        memcpy(p, &v, 2);
    } else {
        p[0] = px & 0xff;
        p[1] = (px >> 8) & 0xff;
        p[2] = (px >> 16) & 0xff;
    }
}

void bgfx_clear(uint32_t color)
{
    canvas_t c = canvas();
    if (!c.px)
        return;
    uint32_t px = pack_pixel(color);
    for (int y = 0; y < c.h; y++) {
        for (int x = 0; x < c.w; x++)
            put_pixel_raw(c, x, y, px);
    }
}

int bgfx_put_pixel(int x, int y, uint32_t color)
{
    canvas_t c = canvas();
    if (!c.px)
        return -1;
    if (x < 0 || y < 0 || x >= c.w || y >= c.h)
        return -1; /* bounds-checked, never writes out of range */
    put_pixel_raw(c, x, y, pack_pixel(color));
    return 0;
}

int bgfx_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    canvas_t c = canvas();
    if (!c.px || w <= 0 || h <= 0)
        return -1;
    /* clip to the visible area */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= c.w || y >= c.h || w <= 0 || h <= 0)
        return -1;
    if (w > c.w - x) w = c.w - x;
    if (h > c.h - y) h = c.h - y;

    uint32_t px = pack_pixel(color);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            put_pixel_raw(c, x + xx, y + yy, px);
    return 0;
}

int bgfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    canvas_t cv = canvas();
    if (!cv.px)
        return -1;
    int idx = (unsigned char)c - BGFX_FONT_FIRST;
    if (idx < 0 || idx > BGFX_FONT_LAST - BGFX_FONT_FIRST)
        return -1;
    for (int row = 0; row < BGFX_FONT_H; row++) {
        uint8_t bits = bgfx_font_data[idx][row];
        for (int col = 0; col < BGFX_FONT_W; col++) {
            int on = (bits >> (7 - col)) & 1;
            bgfx_put_pixel(x + col, y + row, on ? fg : bg);
        }
    }
    return 0;
}

int bgfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    if (!s)
        return -1;
    canvas_t cv = canvas();
    if (!cv.px)
        return -1;
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            y += BGFX_FONT_H;
            continue;
        }
        bgfx_draw_char(cx, y, *s, fg, bg);
        cx += BGFX_FONT_W;
    }
    return 0;
}