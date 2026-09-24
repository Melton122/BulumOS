/*
 * bapp_users.c - Bulum Users app (Phase 10).
 *
 * Reads /etc/passwd directly (safe libc) and lists user accounts with their
 * uid/gid/home/shell.  It deliberately does NOT read /etc/shadow or any
 * password hashes; only read-only, non-sensitive fields are shown.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"

#define USR_PATH  "/etc/passwd"
#define USR_MAX   128

enum { W_LIST = 1, W_INFO, W_REFRESH };

typedef struct {
    int    n;
    char   name[USR_MAX][32];
    long   uid[USR_MAX];
    long   gid[USR_MAX];
    char   home[USR_MAX][64];
    char   shell[USR_MAX][32];
    char   info[160];
} usr_state_t;

static void scan(usr_state_t *st)
{
    st->n = 0;
    FILE *f = fopen(USR_PATH, "r");
    if (!f) { snprintf(st->info, sizeof(st->info), "Cannot open %s", USR_PATH); return; }
    char line[320];
    while (fgets(line, sizeof(line), f) && st->n < USR_MAX) {
        /* name:passwd:uid:gid:gecos:home:shell */
        char *n = line, *pw = strchr(line, ':');
        if (!pw) continue;
        *pw = '\0';
        char *uidp = pw + 1;
        char *e1 = strchr(uidp, ':'); if (!e1) continue; *e1 = '\0';
        char *gidp = e1 + 1;
        char *e2 = strchr(gidp, ':'); if (!e2) continue; *e2 = '\0';
        char *gec = e2 + 1;
        char *e3 = strchr(gec, ':'); if (!e3) { e3 = gec + strlen(gec); }
        char *homep = e3 + 1;
        char *e4 = strchr(homep, ':');
        if (!e4) { e4 = homep + strlen(homep); }
        char *shellp = e4 + 1;
        {
            char *nl = strchr(shellp, '\n'); if (nl) *nl = '\0';
            char *cr = strchr(shellp, '\r'); if (cr) *cr = '\0';
        }
        snprintf(st->name[st->n], sizeof(st->name[0]), "%s", n);
        st->uid[st->n] = atol(uidp);
        st->gid[st->n] = atol(gidp);
        snprintf(st->home[st->n], sizeof(st->home[0]), "%s", homep);
        snprintf(st->shell[st->n], sizeof(st->shell[0]), "%s", shellp);
        st->n++;
    }
    fclose(f);
    if (st->n == 0)
        snprintf(st->info, sizeof(st->info), "No users found.");
}

static int rowfn(appui_t *ui, int wid, int row)
{
    usr_state_t *st = (usr_state_t *)ui->extra;
    appui_widget_t *w = appui_widget(ui, wid);
    if (!st || !w) return -1;
    int idx = w->first + row;
    if (idx < 0 || idx >= st->n) return -1;
    char tmp[BAPPUI_MAX_TEXT];
    snprintf(tmp, sizeof(tmp), "%-10s  uid=%ld  %s", st->name[idx], st->uid[idx],
             st->home[idx]);
    appui_set_rowtext(ui, wid, tmp);
    return 1;
}

static void on_event(appui_t *ui, int evt)
{
    usr_state_t *st = (usr_state_t *)ui->extra;
    if (!st) return;
    int wid = ui->last_widget;
    if (evt == APPUI_EVT_ACTIVATE && wid == W_REFRESH) {
        scan(st);
        appui_set_total(ui, W_LIST, st->n);
        appui_set_text(ui, W_INFO, st->info);
        return;
    }
    if ((evt == APPUI_EVT_SELECT || evt == APPUI_EVT_ACTIVATE) &&
        wid == W_LIST) {
        appui_widget_t *w = appui_widget(ui, W_LIST);
        if (w && st->n > 0) {
            int idx = w->first + w->selected;
            if (idx >= 0 && idx < st->n) {
                char m[160];
                snprintf(m, sizeof(m), "%s  uid=%ld gid=%ld shell=%s",
                         st->name[idx], st->uid[idx], st->gid[idx],
                         st->shell[idx]);
                appui_set_text(ui, W_INFO, m);
            }
        }
    }
}

int bapp_users_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Users", 200, 100, 520, 380, 400, 260, 900, 600))
        return -1;
    usr_state_t *st = calloc(1, sizeof(*st));
    if (!st) { appui_win_close(&ui); return -1; }
    scan(st);
    ui.extra = st;
    ui.on_event = on_event;

    int W = 520;
    appui_add(&ui, appui_list(W_LIST, 12, 12, W - 24, 300, APPUI_ROW_H));
    appui_add(&ui, appui_label(W_INFO, 12, 320, W - 24, st->info));
    appui_add(&ui, appui_button(W_REFRESH, W - 90, 380 - 34, 80, "Refresh"));
    appui_widget_t *lst = appui_widget(&ui, W_LIST);
    if (lst) { lst->rowfn = rowfn; lst->total = st->n; }
    appui_set_active(&ui);
    return ui.win;
}
