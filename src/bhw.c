/*
 * bhw.c - BulumOS hardware API (Phase 11)
 *
 * Aggregates hardware data from sysfs/procfs/devfs.  Pure libc, portable
 * across ARM64/x86-64.  Live hardware reporting for bhardware/bdevices/
 * bdriver/bnetwork/bpower and the desktop apps.
 */
#define _GNU_SOURCE
#include "bhw.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define SYS "/sys"
#define SYS_BLOCK SYS "/block"
#define SYS_BUS_PCI SYS "/bus/pci/devices"
#define SYS_CLASS_SND SYS "/class/sound"
#define SYS_CLASS_NET SYS "/class/net"

int bhw_read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = 0;
    int c;
    while (n + 1 < size && (c = fgetc(f)) != EOF) {
        if (c == '\n')
            break;
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    fclose(f);
    return 1;
}

void bhw_cpu_info(struct bhw_cpu *out)
{
    memset(out, 0, sizeof(*out));
    char buf[BHW_MAX_STR];

    /* arch from /proc/sys/kernel/arch (host build) or uname */
    if (bhw_read_file("/proc/sys/kernel/arch", out->arch, sizeof(out->arch)) == 0) {
        struct utsname un;
        if (uname(&un) == 0)
            snprintf(out->arch, sizeof(out->arch), "%s", un.machine);
    }

    if (bhw_read_file("/proc/cpuinfo", buf, sizeof(buf)) == 0) {
        /* fall back to uname model unknown */
        snprintf(out->model, sizeof(out->model), "(unknown CPU)");
    } else {
        /* cpuinfo is multiline; re-read via dedicated helper below */
    }

    /* model name from cpuinfo (ARM64 "Hardware"/"Model", x86 "model name") */
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        const char *p = strstr(line, "model name");
        if (!p)
            p = strstr(line, "Hardware");
        if (!p)
            p = strstr(line, "Processor");
        if (p) {
            char *eq = strchr(line, ':');
            if (eq) {
                snprintf(out->model, sizeof(out->model), "%s",
                         eq[1] == ' ' ? eq + 2 : eq + 1);
                size_t l = strlen(out->model);
                while (l && (out->model[l-1] == '\n' ||
                             out->model[l-1] == ' '))
                    out->model[--l] = 0;
            }
            break;
        }
    }
    if (f)
        fclose(f);

    /* count processors: count "processor" lines in /proc/cpuinfo */
    f = fopen("/proc/cpuinfo", "r");
    int n = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "processor", 9) == 0)
            n++;
    }
    if (f)
        fclose(f);
    out->cores = n > 0 ? n : sysconf(_SC_NPROCESSORS_ONLN);
    out->online = sysconf(_SC_NPROCESSORS_ONLN);
    out->bogo = 0;
}

void bhw_mem_info(struct bhw_mem *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fopen("/proc/meminfo", "r");
    char line[160];
    while (f && fgets(line, sizeof(line), f)) {
        unsigned long v;
        if (sscanf(line, "MemTotal: %lu kB", &v) == 1)
            out->total_kb = v;
        else if (sscanf(line, "MemFree: %lu kB", &v) == 1)
            out->free_kb = v;
        else if (sscanf(line, "MemAvailable: %lu kB", &v) == 1) {
            out->avail_kb = v;
            out->mem_available = 1;
        }
    }
    if (f)
        fclose(f);
}

int bhw_pci_list(struct bhw_pci *out, int maxn)
{
    DIR *d = opendir(SYS_BUS_PCI);
    if (!d)
        return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < maxn) {
        if (e->d_name[0] == '.')
            continue;
        if (strcmp(e->d_name, "0000:00:00.0") == 0 && count == 0) {
            /* still list if there are others; just skip pseudo-root later */
        }
        char p[512];
        char buf[BHW_MAX_STR];

        snprintf(p, sizeof(p), "%s/%s/class", SYS_BUS_PCI, e->d_name);
        if (bhw_read_file(p, out[count].cls, sizeof(buf)))
            ;
        snprintf(p, sizeof(p), "%s/%s/vendor", SYS_BUS_PCI, e->d_name);
        bhw_read_file(p, out[count].vendor, sizeof(out[count].vendor));
        snprintf(p, sizeof(p), "%s/%s/device", SYS_BUS_PCI, e->d_name);
        bhw_read_file(p, out[count].device, sizeof(out[count].device));
        snprintf(out[count].slot, sizeof(out[count].slot), "%s", e->d_name);
        count++;
    }
    closedir(d);
    return count;
}

void bhw_gpu_detect(struct bhw_gpu_list *gl)
{
    gl->items = NULL;
    gl->count = 0;

    /* DRM cards under /sys/class/drm */
    DIR *d = opendir("/sys/class/drm");
    if (!d)
        return;

    struct dirent *e;
    int cap = 4;
    gl->items = calloc(cap, sizeof(struct bhw_gpu));
    if (!gl->items) {
        closedir(d);
        return;
    }

    while ((e = readdir(d))) {
        if (strstr(e->d_name, "card") != e->d_name)
            continue;
        if (strchr(e->d_name, '-'))
            continue; /* skip card0-DP-1 connectors */
        if (gl->count >= cap) {
            cap *= 2;
            struct bhw_gpu *tmp = realloc(gl->items, cap * sizeof(struct bhw_gpu));
            if (!tmp)
                break;
            gl->items = tmp;
            memset(&gl->items[gl->count], 0, (cap - gl->count) * sizeof(struct bhw_gpu));
        }
        struct bhw_gpu *s = &gl->items[gl->count];
        s->present = 1;
        char p[512];
        snprintf(p, sizeof(p), "/sys/class/drm/%s/device/uevent", e->d_name);
        char buf[BHW_MAX_STR];
        if (bhw_read_file(p, buf, sizeof(buf))) {
            char *m = strstr(buf, "DRIVER=");
            if (m)
                snprintf(s->driver, sizeof(s->driver), "%s", m + 7);
            else
                snprintf(s->driver, sizeof(s->driver), "%s", buf);
        }
        /* fb0 via /sys/class/graphics */
        DIR *gr = opendir("/sys/class/graphics");
        if (gr) {
            struct dirent *ge;
            while ((ge = readdir(gr))) {
                if (strncmp(ge->d_name, "fb", 2) == 0) {
                    snprintf(s->fb_path, sizeof(s->fb_path), "/dev/%s",
                             ge->d_name);
                    char n[128];
                    snprintf(p, sizeof(p), "/sys/class/graphics/%s/name",
                             ge->d_name);
                    if (bhw_read_file(p, n, sizeof(n)))
                        snprintf(s->name, sizeof(s->name), "%s", n);
                    char rz[64];
                    snprintf(p, sizeof(p), "/sys/class/graphics/%s/virtual_size",
                             ge->d_name);
                    if (bhw_read_file(p, rz, sizeof(rz))) {
                        int w = 0, h = 0;
                        if (sscanf(rz, "%d,%d", &w, &h) == 2) {
                            s->width = w;
                            s->height = h;
                        }
                    }
                    break;
                }
            }
            closedir(gr);
        }
        gl->count++;
    }
    closedir(d);
}

void bhw_gpu_free(struct bhw_gpu_list *gl)
{
    free(gl->items);
    gl->items = NULL;
    gl->count = 0;
}

int bhw_gpu_first(struct bhw_gpu *out)
{
    struct bhw_gpu_list gl;
    bhw_gpu_detect(&gl);
    if (gl.count == 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = gl.items[0];
    {
        int present = gl.items[0].present;
        free(gl.items);
        return present;
    }
}

int bhw_net_list(struct bhw_netif *out, int maxn)
{
    DIR *d = opendir(SYS_CLASS_NET);
    if (!d)
        return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < maxn) {
        if (e->d_name[0] == '.')
            continue;
        if (strcmp(e->d_name, "lo") == 0)
            continue;
        struct bhw_netif *s = &out[count];
        snprintf(s->name, sizeof(s->name), "%s", e->d_name);
        char p[512], buf[BHW_MAX_STR];
        snprintf(p, sizeof(p), "%s/%s/operstate", SYS_CLASS_NET, e->d_name);
        if (bhw_read_file(p, buf, sizeof(buf)))
            s->up = strcmp(buf, "up") == 0;
        snprintf(p, sizeof(p), "%s/%s/address", SYS_CLASS_NET, e->d_name);
        if (bhw_read_file(p, s->mac, sizeof(s->mac)))
            ;
        snprintf(p, sizeof(p), "%s/%s/statistics/rx_bytes",
                 SYS_CLASS_NET, e->d_name);
        if (bhw_read_file(p, buf, sizeof(buf)))
            s->rx_bytes = strtoul(buf, NULL, 10);
        snprintf(p, sizeof(p), "%s/%s/statistics/tx_bytes",
                 SYS_CLASS_NET, e->d_name);
        if (bhw_read_file(p, buf, sizeof(buf)))
            s->tx_bytes = strtoul(buf, NULL, 10);
        count++;
    }
    closedir(d);
    return count;
}

int bhw_block_count(void)
{
    DIR *d = opendir(SYS_BLOCK);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        n++;
    }
    closedir(d);
    return n;
}

int bhw_block_names(char names[][BHW_MAX_STR], int maxn)
{
    DIR *d = opendir(SYS_BLOCK);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < maxn) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(names[n], BHW_MAX_STR, "%s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

int bhw_usb_count(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        /* only root hub + real devices; skip port nodes like usb1 */
        if (strchr(e->d_name, ':'))
            n++;
    }
    closedir(d);
    return n;
}

int bhw_audio_cards(char cards[][BHW_MAX_STR], int maxn)
{
    DIR *d = opendir(SYS_CLASS_SND);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < maxn) {
        if (strncmp(e->d_name, "card", 4) == 0) {
            snprintf(cards[n], BHW_MAX_STR, "%s", e->d_name);
            n++;
        }
    }
    closedir(d);
    return n;
}

int bhw_power_battery(int *level, int *ac)
{
    *level = -1;
    *ac = -1;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[512], buf[BHW_MAX_STR];
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type",
                 e->d_name);
        if (bhw_read_file(p, buf, sizeof(buf)) && strcmp(buf, "Battery") == 0) {
            found = 1;
            snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity",
                     e->d_name);
            if (bhw_read_file(p, buf, sizeof(buf)))
                *level = atoi(buf);
        } else if (bhw_read_file(p, buf, sizeof(buf)) &&
                   strcmp(buf, "Mains") == 0) {
            snprintf(p, sizeof(p), "/sys/class/power_supply/%s/online",
                     e->d_name);
            if (bhw_read_file(p, buf, sizeof(buf)))
                *ac = atoi(buf);
        }
    }
    closedir(d);
    return found;
}

int bhw_bulum_drivers(char lines[][BHW_MAX_STR], int maxn)
{
    FILE *f = fopen("/proc/bulum/drivers", "r");
    if (!f)
        return 0;
    int n = 0;
    while (n < maxn && fgets(lines[n], BHW_MAX_STR, f)) {
        lines[n][strcspn(lines[n], "\n")] = 0;
        n++;
    }
    fclose(f);
    return n;
}

int bhw_bulum_hardware(char lines[][BHW_MAX_STR], int maxn)
{
    FILE *f = fopen("/proc/bulum/hardware", "r");
    if (!f)
        return 0;
    int n = 0;
    while (n < maxn && fgets(lines[n], BHW_MAX_STR, f)) {
        lines[n][strcspn(lines[n], "\n")] = 0;
        n++;
    }
    fclose(f);
    return n;
}
