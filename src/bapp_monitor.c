/*
 * bapp_monitor.c - Bulum System Monitor app (Phase 10).
 *
 * Reads /proc CPU/memory/uptime and a process snapshot via bsys, and shows
 * them in a scrollable text view.  Refreshes on demand (no polling thread).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"
#include "bhw.h"

#define MON_BUF 4096

enum { W_BODY = 1, W_REFRESH };

typedef struct {
    char buf[MON_BUF];
} mon_state_t;

static void refresh(appui_t *ui)
{
    mon_state_t *st = (mon_state_t *)ui->extra;
    if (!st) return;
    int cpu = 0;
    long mt = 0, ma = 0, uptime = 0;
    bsys_cpu_mem(&cpu, &mt, &ma, &uptime);
    char hn[64] = "?", kern[64] = "?";
    bsys_hostname(hn, sizeof(hn));
    bsys_kernel(kern, sizeof(kern));

    int off = 0;
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "CPU      : %d%%\n"
                    "Memory   : %ld / %ld MB (%ld%% used)\n"
                    "Uptime   : %ld h %02ld m\n"
                    "Host     : %s\n"
                    "Kernel   : %s\n"
                    "\nProcesses (pid  comm  rss KB):\n",
                    cpu,
                    mt > 0 ? mt / 1024 : 0, ma > 0 ? ma / 1024 : 0,
                    (mt > 0 && ma >= 0 && ma < mt)
                        ? (long)((mt - ma) * 100 / mt) : 0,
                    uptime / 3600, (uptime % 3600) / 60,
                    hn, kern);

    /* Phase 11: in-kernel hardware/driver framework */
    char klines[4][BHW_MAX_STR];
    int kn = bhw_bulum_hardware(klines, 4);
    if (kn > 0) {
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "\nDrivers:\n");
        for (int i = 0; i < kn; i++)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "  %s\n", klines[i]);
    }
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "\nProcesses (pid  comm  rss KB):\n");
    bsys_proc_t procs[64];
    int n = bsys_process_snapshot(procs, 64);
    int shown = 0;
    for (int i = 0; i < n && off < (int)sizeof(st->buf) - 80 && shown < 30; i++) {
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                        "%-7ld  %-12s  %6ld\n",
                        procs[i].pid, procs[i].comm, procs[i].rss_kb);
        shown++;
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

int bapp_monitor_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "System Monitor", 180, 90, 520, 400, 400, 280,
                         900, 600))
        return -1;
    mon_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    ui.extra = st;
    ui.on_event = on_event;

    int W = 520;
    appui_add(&ui, appui_textarea(W_BODY, 12, 12, W - 24, 400 - 66));
    appui_add(&ui, appui_button(W_REFRESH, W - 100, 400 - 40, 88, "Refresh"));
    refresh(&ui);
    appui_set_active(&ui);
    return ui.win;
}
