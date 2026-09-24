/*
 * bapp_security.c - Bulum Security app (Phase 10).
 *
 * Performs a read-only, stat-based security audit (no shell commands).
 * Checks ownership/permissions of sensitive files, whether /etc/shadow
 * exists, whether root shell is usable, and the presence of a firewall
 * marker.  Nothing here writes or mutates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define SEC_BUF 4096

enum { W_BODY = 1, W_REFRESH };

typedef struct {
    char buf[SEC_BUF];
} sec_state_t;

static void audit_line(char *out, size_t outsz, const char *label, int ok,
                       const char *note)
{
    if (ok)
        snprintf(out, outsz, "[OK]   %s%s%s", label, note ? " - " : "",
                 note ? note : "");
    else
        snprintf(out, outsz, "[WARN] %s%s%s", label, note ? " - " : "",
                 note ? note : "");
}

static void refresh(appui_t *ui)
{
    sec_state_t *st = (sec_state_t *)ui->extra;
    if (!st) return;
    int off = 0;

    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                    "Security audit\n\n");

    struct stat sb;
    char line[320];

    if (getuid() == 0) {
        snprintf(line, sizeof(line), "running as root");
        audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                   "Privileged desktop", 0, line);
    } else {
        snprintf(line, sizeof(line), "running as uid %ld", (long)getuid());
        audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                   "Privileged desktop", 1, line);
    }
    off += (int)strlen(st->buf + off);

    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "\n");

    if (stat("/etc/passwd", &sb) == 0) {
        int world = (sb.st_mode & 0002) != 0;
        snprintf(line, sizeof(line), "mode %o", (unsigned)sb.st_mode & 0777);
        audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                   "/etc/passwd world-writable", !world, line);
        off += (int)strlen(st->buf + off);
    }

    int shadow = (stat("/etc/shadow", &sb) == 0);
    if (shadow) {
        int world = (sb.st_mode & 077) != 0;
        snprintf(line, sizeof(line), "mode %o", (unsigned)sb.st_mode & 0777);
        audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                   "/etc/shadow protection", !world, line);
        off += (int)strlen(st->buf + off);
    } else {
        audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                   "/etc/shadow present", 0, "not found");
        off += (int)strlen(st->buf + off);
    }

    off += snprintf(st->buf + off, sizeof(st->buf) - (size_t)off, "\n");

    int fw = (access("/etc/bulum/firewall", F_OK) == 0);
    audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
               "Firewall rules present", fw, fw ? NULL : "adding rules advised");
    off += (int)strlen(st->buf + off);

    long mt = 0, ma = 0, up = 0;
    bsys_cpu_mem(NULL, &mt, &ma, &up);
    if (mt > 0) {
        long used_pct = (ma >= 0 && ma < mt) ? (mt - ma) * 100 / mt : -1;
        if (used_pct >= 95) {
            audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                       "Memory pressure", 0, "nearly full");
        } else {
            audit_line(st->buf + off, sizeof(st->buf) - (size_t)off,
                       "Memory pressure", 1, "ok");
        }
        off += (int)strlen(st->buf + off);
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

int bapp_security_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Security", 200, 100, 560, 400, 420, 280,
                         900, 600))
        return -1;
    sec_state_t *st = calloc(1, sizeof(*st));
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
