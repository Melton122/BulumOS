/*
 * bapps.c - Phase 10 in-process application dispatcher + shared dialog.
 *
 * Maps an `internal:<name>` token (bapp_t.exec) to the matching app module's
 * `bapp_<name>_open()`.  Also provides bapp_dialog(): a small reusable
 * appui-based message/confirm window used by the file manager and other apps
 * for confirmations and errors.
 */

#include "bapps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapp.h"
#include "bappui.h"

/* ------------------------------------------------------------------------ */
/* dispatcher                                                               */

#define ASSOC_PATH "/etc/bulum/associations.conf"

/* allow host tests to redirect the association file (produces the default) */
static const char *assoc_path(void)
{
    const char *e = getenv("BULUM_ASSOC");
    return (e && e[0]) ? e : ASSOC_PATH;
}

/* resolve an association config's value into a registered app exec token */
static int assoc_to_exec(const char *value, char *exec, size_t execsz)
{
    if (!value || !value[0])
        return -1;
    /* value may be "internal:name" or plain "name" of a registered app */
    for (int i = 0; i < bapp_count(); i++) {
        const bapp_t *a = bapp_get(i);
        if (!a)
            continue;
        if (strcmp(a->name, value) == 0 || strcmp(a->exec, value) == 0) {
            snprintf(exec, execsz, "%s", a->exec);
            return 0;
        }
    }
    return -1;
}

int bapps_open_for_path(const char *path)
{
    if (!path || !path[0])
        return -1;
    /* extract the extension, lowercased */
    const char *dot = strrchr(path, '.');
    if (!dot)
        return -1;
    char ext[24];
    size_t el = 0;
    for (const char *p = dot + 1; *p && el < sizeof(ext) - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        ext[el++] = c;
    }
    ext[el] = '\0';
    if (el == 0)
        return -1;

    FILE *f = fopen(assoc_path(), "r");
    if (!f)
        return -1;
    char exec[BAPP_MAX_EXEC] = "";
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (!line[0] || line[0] == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strcmp(line, ext) != 0 &&
            (line[0] != '.' || strcmp(line + 1, ext) != 0))
            continue;
        if (assoc_to_exec(eq + 1, exec, sizeof(exec)) == 0) {
            fclose(f);
            return bapps_open(exec);
        }
    }
    fclose(f);
    return -1;
}

int bapps_open(const char *exec)
{
    if (!exec || strncmp(exec, "internal:", 9) != 0)
        return -1;
    const char *name = exec + 9;
    if (strcmp(name, "files") == 0)      return bapp_files_open();
    if (strcmp(name, "settings") == 0)   return bapp_settings_open();
    if (strcmp(name, "packages") == 0)   return bapp_packages_open();
    if (strcmp(name, "monitor") == 0)    return bapp_monitor_open();
    if (strcmp(name, "hardware") == 0)   return bapp_hardware_open();
    if (strcmp(name, "network") == 0)    return bapp_network_open();
    if (strcmp(name, "security") == 0)   return bapp_security_open();
    if (strcmp(name, "services") == 0)   return bapp_services_open();
    if (strcmp(name, "logs") == 0)       return bapp_logs_open();
    if (strcmp(name, "users") == 0)      return bapp_users_open();
    if (strcmp(name, "terminal") == 0)   return bapp_terminal_open();
    if (strcmp(name, "about") == 0)      return bapp_about_open();
    return -1;                          /* unknown builtin */
}

/* ------------------------------------------------------------------------ */
/* shared dialog                                                             */

typedef struct {
    bdialog_cb cb;
    void *user;
} bdialog_state_t;

static appui_t g_dlg;
static int g_dlg_open;

static void dlg_event(appui_t *ui, int evt)
{
    (void)evt;
    bdialog_state_t *st = (bdialog_state_t *)ui->extra;
    if (evt != APPUI_EVT_ACTIVATE)
        return;
    int wid = ui->last_widget;
    int ok = (wid == 1001);              /* 1001 = OK, 1002 = Cancel */
    bdialog_cb cb = st ? st->cb : NULL;
    void *user = st ? st->user : NULL;
    appui_win_close(ui);
    g_dlg_open = 0;
    if (cb)
        cb(user, ok);
}

/* modal-style confirm dialog; calls cb(user, ok) when a button is pressed */
int bapp_dialog(const char *title, const char *message, const char *ok_label,
                const char *cancel_label, bdialog_cb cb, void *user)
{
    if (g_dlg_open)
        return -1;               /* only one dialog at a time */
    bdialog_state_t *st = calloc(1, sizeof(*st));
    if (!st)
        return -1;
    st->cb = cb;
    st->user = user;

    if (appui_win_create(&g_dlg, title, 260, 220, 420, 150, 320, 120, 800, 600)) {
        free(st);
        return -1;
    }
    g_dlg.extra = st;
    g_dlg.on_event = dlg_event;
    g_dlg_open = 1;

    int dlg_w = 420;
    appui_add(&g_dlg, appui_label(1000, 10, 8, dlg_w - 20, message));
    if (ok_label && ok_label[0])
        appui_add(&g_dlg, appui_button(1001, dlg_w - 190, 110, 90, ok_label));
    if (cancel_label && cancel_label[0])
        appui_add(&g_dlg, appui_button(1002, dlg_w - 90, 110, 80, cancel_label));
    appui_set_active(&g_dlg);
    return 0;
}
