/*
 * bapp_logs.c - Bulum Logs app (Phase 10).
 *
 * Reads the tail of each log file under /var/log/bulum directly (safe libc);
 * no shell, no parsing of CLI colour output.  A bounded number of bytes of
 * each file is shown so the text area can never balloon.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"

#define LOG_DIR  "/var/log/bulum"
#define LOG_BUF  8192
#define LOG_TAIL 800
#define LOG_MAX  16

enum { W_BODY = 1, W_REFRESH };

typedef struct {
    char buf[LOG_BUF];
    char info[160];
} log_state_t;

static void tail_file(const char *path, char *dst, size_t dstsz)
{
    dst[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(dst, dstsz, "(unreadable)"); return; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); snprintf(dst, dstsz, "(error)"); return; }
    long end = ftell(f);
    long start = end - (long)LOG_TAIL;
    if (start < 0) start = 0;
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); snprintf(dst, dstsz, "(error)"); return; }
    size_t got = 0;
    char ch;
    while (got + 1 < dstsz && fread(&ch, 1, 1, f) == 1)
        dst[got++] = ch;
    dst[got] = '\0';
    fclose(f);
}

static void refresh(appui_t *ui)
{
    log_state_t *st = (log_state_t *)ui->extra;
    if (!st) return;
    int off = 0;
    DIR *d = opendir(LOG_DIR);
    if (!d) {
        snprintf(st->buf, sizeof(st->buf), "%s is not readable.", LOG_DIR);
        appui_set_text(ui, W_BODY, st->buf);
        appui_reparse_line_count(ui, W_BODY, st->buf);
        return;
    }
    struct dirent *e;
    int shown = 0;
    while ((e = readdir(d)) && shown < LOG_MAX) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", LOG_DIR, nm);
        char tail[LOG_TAIL + 1];
        tail_file(full, tail, sizeof(tail));
        int n = snprintf(st->buf + off, sizeof(st->buf) - (size_t)off,
                         "== %s ==\n%s\n\n", nm, tail);
        if (n <= 0) break;
        off += n;
        shown++;
    }
    closedir(d);
    (void)off;
    appui_set_text(ui, W_BODY, st->buf);
    appui_reparse_line_count(ui, W_BODY, st->buf);
}

static void on_event(appui_t *ui, int evt)
{
    (void)evt;
    if (ui->last_widget == W_REFRESH)
        refresh(ui);
}

int bapp_logs_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Logs", 200, 100, 560, 400, 420, 280, 900, 600))
        return -1;
    log_state_t *st = calloc(1, sizeof(*st));
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
