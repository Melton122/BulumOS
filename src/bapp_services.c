/*
 * bapp_services.c - Bulum Services app (Phase 10).
 *
 * Queries the bctl service manager via bsys_exec (fork/execve, whitelisted
 * absolute path).  Output is ANSI-stripped and shown in a text area.  No
 * system()/popen(); no shell interpretation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define SV_BUF 8192

enum { W_BODY = 1, W_REFRESH, W_STOP, W_START };

typedef struct {
    char buf[SV_BUF];
} sv_state_t;

static void run_into(appui_t *ui, const char *cmdlabel, const char **argv,
                     int argc)
{
    sv_state_t *st = (sv_state_t *)ui->extra;
    if (!st) return;
    bsys_result_t res;
    int off = 0;
    off += snprintf(st->buf, sizeof(st->buf), "%s:\n", cmdlabel);
    if (bsys_exec("/usr/bin/bctl", argv, argc, &res) == 0) {
        char clean[BSYS_CAP];
        bsys_strip_ansi(res.out, clean, sizeof(clean));
        bsys_trim(clean);
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "%s",
                        clean);
        if (res.rc != 0)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "\n(exit %d)", res.rc);
        appui_set_text(ui, W_BODY, st->buf);
        appui_reparse_line_count(ui, W_BODY, st->buf);
    } else {
        appui_set_text(ui, W_BODY, "bctl could not be run (missing or unsafe).");
        appui_reparse_line_count(ui, W_BODY, st->buf);
    }
}

static void on_event(appui_t *ui, int evt)
{
    if (evt != APPUI_EVT_ACTIVATE)
        return;
    switch (ui->last_widget) {
    case W_REFRESH: {
        const char *a[] = { "list" };
        run_into(ui, "Service status", a, 1);
        break;
    }
    case W_START: {
        const char *a[] = { "start", "all" };
        run_into(ui, "Start all", a, 2);
        break;
    }
    case W_STOP: {
        const char *a[] = { "stop", "all" };
        run_into(ui, "Stop all", a, 2);
        break;
    }
    }
}

int bapp_services_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Services", 200, 100, 560, 430, 420, 300,
                         900, 600))
        return -1;
    sv_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    ui.extra = st;
    ui.on_event = on_event;
    int W = 560;
    appui_add(&ui, appui_textarea(W_BODY, 12, 12, W - 24, 430 - 92));
    appui_add(&ui, appui_button(W_REFRESH, 12, 430 - 66, 80, "List"));
    appui_add(&ui, appui_button(W_START, 104, 430 - 66, 80, "Start All"));
    appui_add(&ui, appui_button(W_STOP, 196, 430 - 66, 80, "Stop All"));
    {
        const char *a[] = { "list" };
        run_into(&ui, "Service status", a, 1);
    }
    appui_set_active(&ui);
    return ui.win;
}
