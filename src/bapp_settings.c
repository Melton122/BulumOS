/*
 * bapp_settings.c - Bulum Settings app (Phase 10).
 *
 * Reads and writes /etc/bulum/preferences.conf via the bsys preferences
 * helpers.  Provides appearance + desktop toggles.  Values are validated on
 * load and sanitized on save.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define PREF_PATH "/etc/bulum/preferences.conf"

enum { W_LABEL = 1, W_THEME_D, W_THEME_L, W_CLK12, W_CLK24,
       W_PANEL, W_ICONS, W_LAUTO, W_SAVE, W_RELOAD };

static void refresh_checks(appui_t *ui)
{
    bpref_t *p = (bpref_t *)ui->extra;
    if (!p) return;
    appui_widget_t *w;
    w = appui_widget(ui, W_THEME_D); if (w) w->value = (strcmp(p->theme, "dark") == 0);
    w = appui_widget(ui, W_THEME_L); if (w) w->value = (strcmp(p->theme, "light") == 0);
    w = appui_widget(ui, W_CLK12);   if (w) w->value = (strcmp(p->clock_format, "12h") == 0);
    w = appui_widget(ui, W_CLK24);   if (w) w->value = (strcmp(p->clock_format, "24h") == 0);
    w = appui_widget(ui, W_PANEL);   if (w) w->value = p->panel_visible;
    w = appui_widget(ui, W_ICONS);   if (w) w->value = p->desktop_icons;
    w = appui_widget(ui, W_LAUTO);   if (w) w->value = p->launcher_auto_close;
}

static void on_event(appui_t *ui, int evt)
{
    bpref_t *p = (bpref_t *)ui->extra;
    if (!p)
        return;
    int wid = ui->last_widget;
    appui_widget_t *w = appui_widget(ui, wid);

    if (evt == APPUI_EVT_CHANGE) {
        switch (wid) {
        case W_THEME_D: if (w && w->value) snprintf(p->theme, sizeof(p->theme), "dark"); break;
        case W_THEME_L: if (w && w->value) snprintf(p->theme, sizeof(p->theme), "light"); break;
        case W_CLK12:   if (w && w->value) snprintf(p->clock_format, sizeof(p->clock_format), "12h"); break;
        case W_CLK24:   if (w && w->value) snprintf(p->clock_format, sizeof(p->clock_format), "24h"); break;
        case W_PANEL:   if (w) p->panel_visible = w->value; break;
        case W_ICONS:   if (w) p->desktop_icons = w->value; break;
        case W_LAUTO:   if (w) p->launcher_auto_close = w->value; break;
        }
        return;
    }
    if (evt != APPUI_EVT_ACTIVATE)
        return;

    if (wid == W_SAVE) {
        if (bpref_save(p, PREF_PATH) == 0) {
            appui_set_text(ui, W_LABEL, "Saved to /etc/bulum/preferences.conf");
            appui_notify(ui, "Settings", "Preferences saved.", "settings",
                         0);
        } else {
            appui_set_text(ui, W_LABEL, "Save failed (write permission?)");
        }
    } else if (wid == W_RELOAD) {
        bpref_load(p, PREF_PATH);
        refresh_checks(ui);
        appui_set_text(ui, W_LABEL, "Reloaded preferences.");
    }
}

int bapp_settings_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Bulum Settings", 240, 120, 460, 360, 360, 260,
                         900, 600))
        return -1;
    bpref_t *p = calloc(1, sizeof(*p));
    if (!p) {
        appui_win_close(&ui);
        return -1;
    }
    bpref_load(p, PREF_PATH);
    ui.extra = p;
    ui.on_event = on_event;

    int W = 460;
    appui_add(&ui, appui_label(W_LABEL, 14, 10, W - 28, "Preferences"));
    appui_add(&ui, appui_separator(0, 14, 34, W - 28));

    appui_add(&ui, appui_check(W_THEME_D, 20, 44, W - 40, "Dark theme", 1));
    appui_add(&ui, appui_check(W_THEME_L, 20, 70, W - 40, "Light theme", 0));
    appui_add(&ui, appui_check(W_CLK12, 20, 96, W - 40, "12-hour clock", 0));
    appui_add(&ui, appui_check(W_CLK24, 20, 122, W - 40, "24-hour clock", 1));
    appui_add(&ui, appui_separator(0, 148, 14, W - 28));
    appui_add(&ui, appui_check(W_PANEL, 20, 158, W - 40, "Show system panel", 1));
    appui_add(&ui, appui_check(W_ICONS, 20, 184, W - 40, "Show desktop icons", 1));
    appui_add(&ui, appui_check(W_LAUTO, 20, 210, W - 40, "Close launcher after launch", 1));

    appui_add(&ui, appui_button(W_SAVE, 260, 316, 90, "Save"));
    appui_add(&ui, appui_button(W_RELOAD, 360, 316, 90, "Reload"));

    refresh_checks(&ui);
    appui_set_active(&ui);
    return ui.win;
}
