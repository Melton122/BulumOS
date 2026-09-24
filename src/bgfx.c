/*
 * bgfx.c - BulumOS graphics abstraction (Phase 8)
 *
 * Backends:
 *   framebuffer: /dev/fb0 via open/ioctl/mmap.  Primary backend.
 *   drm:         not implemented in Phase 8; detection + honest
 *                error only.  Documented as the next backend.
 *
 * Safety (Phase 8 stage 18):
 *   - all pixel offsets are bounds-checked against width/height
 *   - pitch is validated against width*bpp/8 at init
 *   - pixel formats other than 16/24/32 bpp with sane channel
 *     offsets are rejected with a useful error
 *   - no dynamic allocation; fixed-size structs only
 */

#define _GNU_SOURCE

#include "bgfx.h"
#include "bgfx_font.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
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

/* channel packing for the current format */
static struct {
    int r_off, r_len;
    int g_off, g_len;
    int b_off, b_len;
    int bytes_pp; /* 2, 3 or 4 */
} g_fmt;

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

int bgfx_init(const char *backend)
{
    const char *want = backend ? backend : "auto";

    memset(&g_info, 0, sizeof(g_info));
    g_info.backend = BGFX_BACKEND_NONE;

    if (strcasecmp(want, "drm") == 0) {
        /* Stage 5: DRM is detection-only in Phase 8 */
        bgfx_detect(&g_info);
        set_error(&g_info,
                  "drm backend not implemented in Phase 8 (framebuffer is primary; "
                  "DRM/KMS is the next backend)");
        return -1;
    }
    if (strcasecmp(want, "framebuffer") != 0 && strcasecmp(want, "auto") != 0) {
        set_error(&g_info, "unknown backend '%s' (supported: auto, framebuffer, drm)", want);
        return -1;
    }

    int fd = open(FB_PATH, O_RDWR);
    if (fd < 0) {
        /* test hook: BGFX_FB_PATH overrides the device path */
        const char *alt = getenv("BGFX_FB_PATH");
        if (alt && alt[0])
            fd = open(alt, O_RDWR);
        if (fd < 0) {
            bgfx_detect(&g_info);
            set_error(&g_info, "framebuffer %s unavailable: %s", FB_PATH, strerror(errno));
            return -1;
        }
        snprintf(g_info.fb_path, sizeof(g_info.fb_path), "%s", alt);
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
    snprintf(g_info.fb_path, sizeof(g_info.fb_path), "%s", FB_PATH);
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
    return 0;
}

void bgfx_shutdown(void)
{
    if (g_map) {
        munmap(g_map, g_map_len);
        g_map = NULL;
        g_map_len = 0;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_info.backend = BGFX_BACKEND_NONE;
    memset(&g_info, 0, sizeof(g_info));
}

int bgfx_available(void)
{
    return g_info.backend != BGFX_BACKEND_NONE && g_map != NULL;
}

const bgfx_info_t *bgfx_info(void)
{
    return &g_info;
}

int bgfx_get_width(void)  { return g_info.width; }
int bgfx_get_height(void) { return g_info.height; }
int bgfx_get_pitch(void)  { return g_info.pitch; }
int bgfx_get_bpp(void)    { return g_info.bpp; }

uint32_t bgfx_pack(uint32_t color)
{
    return pack_pixel(color);
}

void bgfx_blit_from(const void *buf, int pitch,
                    int sx, int sy, int w, int h, int dx, int dy)
{
    if (!bgfx_available() || !buf || w <= 0 || h <= 0)
        return;
    /* clip the source region to the framebuffer destination */
    if (sx < 0) { w += sx; sx = 0; dx = 0; }
    if (sy < 0) { h += sy; sy = 0; dy = 0; }
    if (w <= 0 || h <= 0)
        return;
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx >= g_info.width || dy >= g_info.height || w <= 0 || h <= 0)
        return;
    if (w > g_info.width - dx)  w = g_info.width - dx;
    if (h > g_info.height - dy) h = g_info.height - dy;

    const uint8_t *src = (const uint8_t *)buf + (size_t)sy * (size_t)pitch +
                         (size_t)sx * (size_t)g_fmt.bytes_pp;
    for (int row = 0; row < h; row++) {
        uint8_t *dst = g_map + (size_t)(dy + row) * (size_t)g_info.pitch +
                       (size_t)dx * (size_t)g_fmt.bytes_pp;
        memcpy(dst, src, (size_t)w * (size_t)g_fmt.bytes_pp);
        src += pitch;
    }
}

/* ---------- rendering ---------- */

void bgfx_begin(void) { }
void bgfx_end(void)   { }

void bgfx_clear(uint32_t color)
{
    if (!bgfx_available())
        return;
    uint32_t px = pack_pixel(color);
    uint8_t *p = g_map;
    for (int y = 0; y < g_info.height; y++) {
        uint8_t *row = p;
        for (int x = 0; x < g_info.width; x++) {
            if (g_fmt.bytes_pp == 4) {
                memcpy(row, &px, 4);
            } else if (g_fmt.bytes_pp == 2) {
                uint16_t v = (uint16_t)px;
                memcpy(row, &v, 2);
            } else {
                row[0] = px & 0xff;
                row[1] = (px >> 8) & 0xff;
                row[2] = (px >> 16) & 0xff;
            }
            row += g_fmt.bytes_pp;
        }
        p += g_info.pitch;
    }
}

int bgfx_put_pixel(int x, int y, uint32_t color)
{
    if (!bgfx_available())
        return -1;
    if (x < 0 || y < 0 || x >= g_info.width || y >= g_info.height)
        return -1; /* bounds-checked, never writes out of range */
    uint32_t px = pack_pixel(color);
    uint8_t *p = g_map + (size_t)y * (size_t)g_info.pitch + (size_t)x * (size_t)g_fmt.bytes_pp;
    if (g_fmt.bytes_pp == 4) {
        memcpy(p, &px, 4);
    } else if (g_fmt.bytes_pp == 2) {
        uint16_t v = (uint16_t)px;
        memcpy(p, &v, 2);
    } else {
        p[0] = px & 0xff;
        p[1] = (px >> 8) & 0xff;
        p[2] = (px >> 16) & 0xff;
    }
    return 0;
}

int bgfx_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!bgfx_available() || w <= 0 || h <= 0)
        return -1;
    /* clip to the visible area */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= g_info.width || y >= g_info.height || w <= 0 || h <= 0)
        return -1;
    if (w > g_info.width - x) w = g_info.width - x;
    if (h > g_info.height - y) h = g_info.height - y;

    uint32_t px = pack_pixel(color);
    for (int yy = 0; yy < h; yy++) {
        uint8_t *p = g_map + (size_t)(y + yy) * (size_t)g_info.pitch +
                     (size_t)x * (size_t)g_fmt.bytes_pp;
        for (int xx = 0; xx < w; xx++) {
            if (g_fmt.bytes_pp == 4) {
                memcpy(p, &px, 4);
            } else if (g_fmt.bytes_pp == 2) {
                uint16_t v = (uint16_t)px;
                memcpy(p, &v, 2);
            } else {
                p[0] = px & 0xff;
                p[1] = (px >> 8) & 0xff;
                p[2] = (px >> 16) & 0xff;
            }
            p += g_fmt.bytes_pp;
        }
    }
    return 0;
}

int bgfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    if (!bgfx_available())
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
    if (!bgfx_available() || !s)
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