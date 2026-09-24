/*
 * bapp_packages.c - Bulum Packages app (Phase 10/12).
 *
 * Queries the bulum/bpk package manager via bsys_exec (fork/execve,
 * whitelisted absolute path).  Output is ANSI-stripped and shown in a text
 * area.  No system()/popen(); no shell interpretation.
 *
 * Phase 12: full command set -- List, Search, Install, Remove (with a
 * confirm dialog), Update, Upgrade, Clean -- plus a launcher registry rescan
 * after install/remove so newly installed apps appear immediately.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"
#include "bapp.h"

#define PK_BUF 8192

enum {
    W_BODY = 1, W_SEARCH, W_TERM, W_LIST, W_INSTALL, W_REMOVE, W_UPDATE,
    W_UPGRADE, W_CLEAN
};

typedef struct {
    char buf[PK_BUF];
    char pending[128];   /* package name awaiting remove confirmation */
} pk_state_t;

static appui_t *g_ui;

static void auto_msg(appui_t *ui, const char *m)
{
    appui_set_text(ui, W_BODY, m);
    appui_reparse_line_count(ui, W_BODY, m);
}

static void run_into(appui_t *ui, const char *cmdlabel, const char **argv,
                     int argc)
{
    pk_state_t *st = (pk_state_t *)ui->extra;
    if (!st) return;
    bsys_result_t res;
    int off = 0;
    off += snprintf(st->buf, sizeof(st->buf), "%s:\n", cmdlabel);
    if (bsys_exec("/sbin/bulum", argv, argc, &res) == 0) {
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
        appui_scroll_end(ui, W_BODY);
        if (res.rc == 0 &&
            (strcmp(cmdlabel, "Install") == 0 ||
             strcmp(cmdlabel, "Remove") == 0))
            bapp_rescan(BAPP_APPS_DIR);
    } else {
        auto_msg(ui, "bulum could not be run (missing or unsafe).");
    }
}

static void remove_cb(void *user, int ok)
{
    (void)user;
    if (!ok || !g_ui)
        return;
    pk_state_t *st = (pk_state_t *)g_ui->extra;
    if (!st || !st->pending[0])
        return;
    const char *a[] = { "remove", st->pending };
    run_into(g_ui, "Remove", a, 2);
    appui_set_active(g_ui);
}

static void on_event(appui_t *ui, int evt)
{
    if (evt != APPUI_EVT_ACTIVATE)
        return;
    pk_state_t *st = (pk_state_t *)ui->extra;
    switch (ui->last_widget) {
    case W_LIST: {
        const char *a[] = { "list" };
        run_into(ui, "Installed packages", a, 1);
        break;
    }
    case W_SEARCH:
    case W_INSTALL: {
        const char *term = appui_get_text(ui, W_TERM);
        if (!term || !term[0]) {
            auto_msg(ui,
                     ui->last_widget == W_SEARCH
                         ? "Enter a search term first."
                         : "Enter a package name first.");
            break;
        }
        const char *a[] = { ui->last_widget == W_SEARCH ? "search" : "install",
                            term };
        run_into(ui, ui->last_widget == W_SEARCH ? "Search results" : "Install",
                 a, 2);
        break;
    }
    case W_REMOVE: {
        const char *term = appui_get_text(ui, W_TERM);
        if (!term || !term[0]) {
            auto_msg(ui, "Enter a package name first.");
            break;
        }
        if (st)
            snprintf(st->pending, sizeof(st->pending), "%s", term);
        char msg[160];
        snprintf(msg, sizeof(msg), "Remove installed package '%s'?", term);
        bapp_dialog("Confirm Remove", msg, "Remove", "Cancel", remove_cb,
                    ui);
        break;
    }
    case W_UPDATE: {
        const char *a[] = { "update" };
        run_into(ui, "Update", a, 1);
        break;
    }
    case W_UPGRADE: {
        const char *a[] = { "upgrade" };
        run_into(ui, "Upgrade", a, 1);
        break;
    }
    case W_CLEAN: {
        const char *a[] = { "clean" };
        run_into(ui, "Cache clean", a, 1);
        break;
    }
    }
}

int bapp_packages_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Packages", 200, 100, 580, 430, 440, 300,
                         900, 600))
        return -1;
    pk_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    ui.extra = st;
    ui.on_event = on_event;
    g_ui = &ui;
    int W = 580;
    appui_add(&ui, appui_input(W_TERM, 12, 12, W - 24, "search term / package name"));
    appui_add(&ui, appui_textarea(W_BODY, 12, 44, W - 24, 430 - 140));
    appui_add(&ui, appui_button(W_LIST, 12, 372, 90, "List"));
    appui_add(&ui, appui_button(W_SEARCH, 112, 372, 90, "Search"));
    appui_add(&ui, appui_button(W_UPDATE, 212, 372, 90, "Update"));
    appui_add(&ui, appui_button(W_UPGRADE, 312, 372, 90, "Upgrade"));
    appui_add(&ui, appui_button(W_INSTALL, 12, 400, 90, "Install"));
    appui_add(&ui, appui_button(W_REMOVE, 112, 400, 90, "Remove"));
    appui_add(&ui, appui_button(W_CLEAN, 212, 400, 90, "Clean"));
    {
        const char *a[] = { "list" };
        run_into(&ui, "Installed packages", a, 1);
    }
    appui_set_active(&ui);
    return ui.win;
}