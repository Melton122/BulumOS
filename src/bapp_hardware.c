/*
 * bapp_hardware.c - Bulum Hardware app (Phase 11).
 *
 * Renders a live hardware summary inside a desktop window using the bhw
 * library: CPU, memory, GPU, storage, network, USB, audio, power and the
 * in-kernel bulum driver framework (/proc/bulum).  Refresh on demand.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bapps.h"
#include "bappui.h"
#include "bhw.h"

#define HW_BUF 4096

enum { W_BODY = 1, W_REFRESH };

typedef struct {
    char buf[HW_BUF];
} hw_state_t;

static void mem_line(char *out, size_t sz, const char *label,
                     unsigned long kb)
{
    if (kb >= 1024 * 1024)
        snprintf(out, sz, "%s %ld.%02ld GB", label, kb / 1024 / 1024,
                 (kb % (1024 * 1024)) * 100 / (1024 * 1024));
    else if (kb >= 1024)
        snprintf(out, sz, "%s %ld MB", label, kb / 1024);
    else
        snprintf(out, sz, "%s %lu KB", label, kb);
}

static void refresh(appui_t *ui)
{
    hw_state_t *st = (hw_state_t *)ui->extra;
    if (!st) return;

    struct bhw_cpu cpu;
    bhw_cpu_info(&cpu);

    struct bhw_mem mem;
    bhw_mem_info(&mem);
    char mt[64], mf[64], ma[64];
    mem_line(mt, sizeof(mt), "Memory total", mem.total_kb);
    mem_line(mf, sizeof(mf), "Memory free ", mem.free_kb);
    mem_line(ma, sizeof(ma), "Memory avail", mem.avail_kb);

    struct bhw_gpu gpu;
    int has_gpu = bhw_gpu_first(&gpu);

    int blocks = bhw_block_count();

    struct bhw_netif nif[4];
    int nn = bhw_net_list(nif, 4);

    char klines[8][BHW_MAX_STR];
    int kn = bhw_bulum_hardware(klines, 8);

    char dlines[8][BHW_MAX_STR];
    int dn = bhw_bulum_drivers(dlines, 8);

    int usbc = bhw_usb_count();

    int off = 0;
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "CPU\n  %s\n  %d cores (%d online)\n\n"
                    "Memory\n  %s\n  %s\n  %s\n\n",
                    cpu.model[0] ? cpu.model : "(unknown)", cpu.cores,
                    cpu.online, mt, mf, ma);

    if (has_gpu) {
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "GPU\n  %s", gpu.name[0] ? gpu.name : "(unnamed)");
        if (gpu.driver[0])
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "  [%s]", gpu.driver);
        if (gpu.width > 0)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "\n  %dx%d", gpu.width, gpu.height);
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "\n\n");
    } else {
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "GPU\n  none detected\n\n");
    }

    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "Storage\n  %d block device(s)\n\n"
                    "Network\n",
                    blocks);
    if (nn > 0) {
        for (int i = 0; i < nn; i++)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "  %s %s %s\n", nif[i].name,
                            nif[i].up ? "up" : "down",
                            nif[i].mac[0] ? nif[i].mac : "(no mac)");
    } else {
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "  (none)\n");
    }
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "\nUSB\n  %d device(s)\n\n", usbc);

    if (kn > 0) {
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "Kernel framework (/proc/bulum)\n");
        for (int i = 0; i < kn; i++)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "  %s\n", klines[i]);
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "\nRegistered drivers\n");
        for (int i = 0; i < dn; i++)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "  %s\n", dlines[i]);
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "\n");
    }

    appui_set_text(ui, W_BODY, st->buf);
    appui_reparse_line_count(ui, W_BODY, st->buf);
}

static void on_event(appui_t *ui, int evt)
{
    (void)evt;
    if (ui->last_widget == W_REFRESH)
        refresh(ui);
}

int bapp_hardware_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Hardware", 150, 92, 560, 420, 420, 300, 900,
                         640))
        return -1;
    hw_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    ui.extra = st;
    ui.on_event = on_event;

    int W = 560;
    appui_add(&ui, appui_textarea(W_BODY, 12, 12, W - 24, 420 - 66));
    appui_add(&ui, appui_button(W_REFRESH, W - 100, 420 - 40, 88, "Refresh"));
    refresh(&ui);
    appui_set_active(&ui);
    return ui.win;
}