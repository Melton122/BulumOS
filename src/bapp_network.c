/*
 * bapp_network.c - Bulum Network app (Phase 10).
 *
 * Reads interface counters from /proc/net/dev and DNS servers from
 * /etc/resolv.conf directly (safe libc) - no shell, no colour parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define NET_BUF 4096

enum { W_BODY = 1, W_REFRESH };

typedef struct {
    char buf[NET_BUF];
} net_state_t;

static void dev_snapshot(char *out, size_t outsz)
{
    size_t off = 0;
    off += snprintf(out + off, outsz - off,
                    "Interfaces (from /proc/net/dev):\n\n");
    off += snprintf(out + off, outsz - off,
                    "%-8s  %12s  %12s\n", "iface", "rx-bytes", "tx-bytes");
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) {
        snprintf(out, outsz, "%s", "No /proc/net/dev.");
        return;
    }
    char line[256];
    if (fgets(line, sizeof(line), f)) { }   /* header */
    if (fgets(line, sizeof(line), f)) { }   /* header 2 */
    while (fgets(line, sizeof(line), f) && off < outsz - 120) {
        char iface[64] = "";
        unsigned long rx = 0, tx = 0;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        size_t in = (size_t)(colon - line);
        if (in >= sizeof(iface)) in = sizeof(iface) - 1;
        memcpy(iface, line, in);
        iface[in] = '\0';
        char *ifp = iface;
        while (*ifp == ' ') ifp++;
        if (sscanf(colon + 1, "%lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   &rx, &tx) >= 1) {
            off += snprintf(out + off, outsz - off, "%-8s  %12lu  %12lu\n",
                            ifp, rx, tx);
        }
    }
    fclose(f);
}

static void dns_list(char *out, size_t outsz)
{
    size_t off = 0;
    off += snprintf(out + off, outsz - off, "\nDNS nameservers:\n");
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) {
        snprintf(out + off, outsz - off, "  (no /etc/resolv.conf)\n");
        return;
    }
    char line[128];
    int any = 0;
    while (fgets(line, sizeof(line), f) && off < outsz - 100) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (strncmp(line, "nameserver", 10) == 0) {
            off += snprintf(out + off, outsz - off, "  %s\n", line + 10 + 1);
            any = 1;
        }
    }
    if (!any)
        off += snprintf(out + off, outsz - off, "  (none)\n");
    fclose(f);
}

static void refresh(appui_t *ui)
{
    net_state_t *st = (net_state_t *)ui->extra;
    if (!st) return;
    char hn[64] = "?";
    bsys_hostname(hn, sizeof(hn));
    int off = 0;
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "Hostname: %s\n\n", hn);
    char d[1024];
    dev_snapshot(d, sizeof(d));
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "%s", d);
    char r[256];
    dns_list(r, sizeof(r));
    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "%s", r);
    appui_set_text(ui, W_BODY, st->buf);
    appui_reparse_line_count(ui, W_BODY, st->buf);
}

static void on_event(appui_t *ui, int evt)
{
    (void)evt;
    if (ui->last_widget == W_REFRESH)
        refresh(ui);
}

int bapp_network_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Network", 200, 100, 560, 400, 420, 280, 900, 600))
        return -1;
    net_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    ui.extra = st;
    ui.on_event = on_event;
    int W = 560;
    appui_add(&ui, appui_textarea(W_BODY, 12, 12, W - 24, 400 - 66));
    appui_add(&ui, appui_button(W_REFRESH, W - 96, 400 - 40, 84, "Refresh"));
    refresh(&ui);
    appui_set_active(&ui);
    return ui.win;
}
