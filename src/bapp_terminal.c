/*
 * bapp_terminal.c - Bulum Terminal (Phase 10, bounded command console).
 *
 * A real interactive PTY/shell widget is deferred to Phase 11 (per the task);
 * this Phase 10 version runs a single well-formed, whitelisted absolute
 * command via bsys_exec and shows its captured output.  Only absolute
 * paths under /bin, /usr/bin or /sbin are accepted; input is length-bounded;
 * no system()/popen(), no shell interpretation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define TM_BUF 8192

enum { W_BODY = 1, W_CMD, W_RUN, W_PR1, W_PR2 };

typedef struct {
    char buf[TM_BUF];
} tm_state_t;

static void show(appui_t *ui, const char *m)
{
    appui_set_text(ui, W_BODY, m);
    appui_reparse_line_count(ui, W_BODY, m);
}

/* run an absolute whitelisted command with space-separated args */
static void run_cmd(appui_t *ui, const char *cmdline)
{
    tm_state_t *st = (tm_state_t *)ui->extra;
    if (!st) return;
    if (!cmdline || !cmdline[0]) {
        show(ui, "No command entered.");
        return;
    }
    /* tokenize, bounded */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", cmdline);
    const char *av[BSYS_MAX_ARG];
    int argc = 0;
    char *tok = strtok(tmp, " ");
    while (tok && argc < BSYS_MAX_ARG - 1) {
        av[argc++] = tok;
        tok = strtok(NULL, " ");
    }
    if (argc == 0) {
        show(ui, "No command.");
        return;
    }
    const char *exe = av[0];
    if (exe[0] != '/') {
        show(ui, "Absolute path required (e.g. /usr/bin/bctl).");
        return;
    }
    bsys_result_t res;
    int off = 0;
    off += snprintf(st->buf, sizeof(st->buf), "$ %s\n", cmdline);
    if (bsys_exec(exe, av + 1, argc - 1, &res) == 0) {
        char clean[BSYS_CAP];
        bsys_strip_ansi(res.out, clean, sizeof(clean));
        bsys_trim(clean);
        off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "%s",
                        clean);
        if (res.rc != 0)
            off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                            "\n(exit %d)", res.rc);
        show(ui, st->buf);
    } else {
        show(ui, "Command unsafe or not found (must be under /bin, /usr/bin, /sbin).");
    }
}

static void on_event(appui_t *ui, int evt)
{
    if (evt != APPUI_EVT_ACTIVATE)
        return;
    switch (ui->last_widget) {
    case W_RUN:
    case W_CMD: {
        const char *c = appui_get_text(ui, W_CMD);
        run_cmd(ui, c);
        break;
    }
    case W_PR1:
        run_cmd(ui, "/usr/bin/bctl list");
        break;
    case W_PR2:
        run_cmd(ui, "/usr/bin/bstatus");
        break;
    }
}

int bapp_terminal_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Terminal", 160, 80, 600, 420, 440, 280,
                         1100, 700))
        return -1;
    tm_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    ui.extra = st;
    ui.on_event = on_event;
    int W = 600;
    appui_add(&ui, appui_input(W_CMD, 12, 12, W - 24, "/usr/bin/bctl list"));
    appui_add(&ui, appui_textarea(W_BODY, 12, 44, W - 24, 420 - 130));
    appui_add(&ui, appui_button(W_RUN, 12, 420 - 72, 80, "Run"));
    appui_add(&ui, appui_button(W_PR1, 100, 420 - 72, 90, "bctl list"));
    appui_add(&ui, appui_button(W_PR2, 198, 420 - 72, 90, "bstatus"));
    run_cmd(&ui, "/usr/bin/bctl list");
    appui_set_active(&ui);
    return ui.win;
}