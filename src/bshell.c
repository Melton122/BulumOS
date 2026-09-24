/*
 * bshell.c - BulumOS desktop shell (Phase 9)
 *
 * Drives the bwm compositor: system panel, launcher menu, desktop
 * background, notification popups, clock and status tray, plus a couple of
 * built-in in-process demo windows so the desktop is usable immediately.
 *
 * Rendering is done exclusively through the bwm back-buffer helpers so the
 * shell stays pixel-format agnostic and shares the compositor's double-buffer
 * present path.
 */

#define _GNU_SOURCE

#include "bshell.h"
#include "bshell_internal.h"
#include "bgfx_font.h"
#include "bapps.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_H BSHELL_PANEL_H

/* status tray width allocated on the right of the panel */
#define TRAY_W 170

void bshell_init(bshell_t *s, const btheme_t *theme)
{
    memset(s, 0, sizeof(*s));
    s->theme = theme;
    s->scrn = bwm_screen();
    s->battery_pct = 87;
    s->clock_hh = 12;
    s->clock_mm = 0;
    bnotify_init(&s->notify_time_ms, BNOTIFY_CAP);
    s->nnotifs = bnotify_list(s->notifs, BNOTIFY_CAP);
}

void bshell_register(bshell_t *s)
{
    bwm_set_background_fn((bwm_overlay_fn)bshell_draw_background, s);
    bwm_set_foreground_fn((bwm_overlay_fn)bshell_draw_foreground, s);
}

/* ---- desktop background ------------------------------------------------ */

void bshell_draw_background(bshell_t *s, const void *bb, int pitch)
{
    const btheme_t *t = s->theme;
    if (t->wtop != t->wbot)
        bwm_gradient(bb, pitch, t->wtop, t->wbot);
    else
        bwm_fill(bb, pitch, 0, 0, s->scrn->w, s->scrn->h, t->wtop);

    /* subtle bottom accent line above the panel */
    if (PANEL_H > 0)
        bwm_hline(bb, pitch, 0, PANEL_H, s->scrn->w, t->border);
}

/* ---- launcher menu ----------------------------------------------------- */

static int launcher_root(bshell_t *s)
{
    return PANEL_H; /* the launcher drops down below the panel */
}

/* Rebuild the filtered (visible) app list from the current search query.
 * An empty query shows every app in registration order. */
static void launcher_refilter(bshell_t *s)
{
    int napps = bapp_count();
    s->launcher_nvis = 0;
    if (s->launcher_query[0] == '\0') {
        for (int i = 0; i < napps && i < BAPP_MAX_APPS; i++)
            s->launcher_vis[s->launcher_nvis++] = i;
        return;
    }
    for (int i = 0; i < napps && s->launcher_nvis < BAPP_MAX_APPS; i++) {
        const bapp_t *a = bapp_get(i);
        if (!a)
            continue;
        if (strcasestr(a->name, s->launcher_query) ||
            strcasestr(a->cat, s->launcher_query))
            s->launcher_vis[s->launcher_nvis++] = i;
    }
    if (s->launcher_hover >= s->launcher_nvis)
        s->launcher_hover = s->launcher_nvis > 0 ? s->launcher_nvis - 1 : -1;
}

static const bapp_t *launcher_item(bshell_t *s, int visidx)
{
    if (visidx < 0 || visidx >= s->launcher_nvis)
        return NULL;
    return bapp_get(s->launcher_vis[visidx]);
}

void bshell_launcher_rect(bshell_t *s, int *x, int *y, int *w, int *h)
{
    int napps = s->launcher_nvis;
    *x = 4;
    *y = launcher_root(s);
    *w = BSHELL_LAUNCHER_W;
    if (*w > s->scrn->w - 8)
        *w = s->scrn->w - 8;
    *h = napps * (BGFX_FONT_H + 8) + 12;
    int maxh = s->scrn->h - *y - 8;
    if (*h > maxh)
        *h = maxh > 0 ? maxh : 0;
}

void bshell_draw_launcher(bshell_t *s, const void *bb, int pitch)
{
    const btheme_t *t = s->theme;
    int napps = s->launcher_nvis;
    if (napps <= 0)
        return;

    int x, y, w, h;
    bshell_launcher_rect(s, &x, &y, &w, &h);

    /* hover highlight tracks the pointer within the menu */
    const bwm_pointer_t *ptr = bwm_pointer();
    int hov = -1;
    if (ptr->x >= x && ptr->x < x + w && ptr->y >= y + 14 &&
        ptr->y < y + h - 4)
        hov = (ptr->y - (y + 14)) / (BGFX_FONT_H + 6);
    s->launcher_hover = (hov >= 0 && hov < napps) ? hov : -1;

    bwm_fill(bb, pitch, x, y, w, h, t->surface);
    bwm_rect(bb, pitch, x, y, w, h, t->border);
    bwm_fill(bb, pitch, x, y, w, 4, t->accent);

    int iy = y + 14;
    for (int i = 0; i < napps; i++) {
        const bapp_t *a = launcher_item(s, i);
        if (!a)
            continue;
        if (iy + 8 >= y + h)
            break;
        int hover = (i == s->launcher_hover);
        if (hover)
            bwm_fill(bb, pitch, x + 3, iy - 2, w - 6, BGFX_FONT_H + 4,
                     t->accent);
        bwm_icon(bb, pitch, x + 8, iy, a->icon, 18,
                 hover ? t->window : t->muted,
                 hover ? t->accent : t->surface);
        bwm_text(bb, pitch, x + 32, iy, a->name,
                 hover ? t->window : t->text, hover ? t->accent : t->surface);
        iy += BGFX_FONT_H + 6;
    }

    /* show the active search query at the top */
    if (s->launcher_query[0]) {
        char q[48];
        snprintf(q, sizeof(q), "search: %s", s->launcher_query);
        bwm_text(bb, pitch, x + 6, y + 6, q, t->accent, t->surface);
    }
}

/* ---- panel ------------------------------------------------------------- */

/* Geometry helpers shared with pointer handling */
void bshell_panel_zones(bshell_t *s, int *launcher_w,
                        int *tray_x, int *clock_x)
{
    *launcher_w = 58;
    *tray_x = s->scrn->w - TRAY_W;
    *clock_x = s->scrn->w - 88;
}

static void draw_panel(bshell_t *s, const void *bb, int pitch)
{
    const btheme_t *t = s->theme;
    int W = s->scrn->w;

    bwm_fill(bb, pitch, 0, 0, W, PANEL_H, t->panel);
    bwm_hline(bb, pitch, 0, PANEL_H, W, t->border);

    int lw, tx, cx;
    bshell_panel_zones(s, &lw, &tx, &cx);

    /* launcher button */
    int hover_l = s->launcher_open;
    bwm_fill(bb, pitch, 0, 0, lw, PANEL_H, hover_l ? t->accent : t->panel);
    bwm_icon(bb, pitch, (lw - 24) / 2, (PANEL_H - 24) / 2, "launcher", 24,
             hover_l ? t->window : t->text, hover_l ? t->accent : t->panel);
    bwm_text(bb, pitch, 6, (PANEL_H - BGFX_FONT_H) / 2,
             hover_l ? "Open" : "Apps",
             hover_l ? t->window : t->muted, hover_l ? t->accent : t->panel);

    /* taskbar: running windows (centre) */
    int x = lw + 6;
    int cnt = bwm_window_count();
    if (cnt > 30)
        cnt = 30;
    int slot_w = 120;
    int max_w = tx - x - 6;
    if (slot_w > max_w && max_w > 20)
        slot_w = max_w;
    if (slot_w >= 40) {
        for (int i = 0; i < cnt; i++) {
            bwindow_t *w = bwm_window_at(i);
            if (!w)
                continue;
            int active = (w->id == bwm_focused_window());
            bwm_fill(bb, pitch, x, 3, slot_w, PANEL_H - 6,
                     active ? t->accent : t->surface);
            bwm_text(bb, pitch, x + 4, (PANEL_H - BGFX_FONT_H) / 2,
                     w->title, active ? t->window : t->text,
                     active ? t->accent : t->surface);
            x += slot_w + 4;
            if (x + slot_w > tx)
                break;
        }
    }

    /* status tray: clock + notification indicator */
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d", s->clock_hh, s->clock_mm);
    bwm_text(bb, pitch, cx, (PANEL_H - BGFX_FONT_H) / 2, buf,
             t->text, t->panel);

    /* notification bell + count */
    int bell = tx + 6;
    bwm_icon(bb, pitch, bell, (PANEL_H - 18) / 2, "notifications", 18,
             s->nnotifs ? t->warning : t->muted, t->panel);
    if (s->nnotifs > 0) {
        char nb[8];
        snprintf(nb, sizeof(nb), "%d", s->nnotifs);
        bwm_text(bb, pitch, bell + 20, (PANEL_H - BGFX_FONT_H) / 2, nb,
                 t->warning, t->panel);
    }

    /* battery icon */
    bwm_icon(bb, pitch, tx + 42, (PANEL_H - 18) / 2, "battery", 18,
             s->battery_pct < 20 ? t->error : t->muted, t->panel);
    if (s->battery_pct < 20) {
        char bb2[8];
        snprintf(bb2, sizeof(bb2), "%d%%", s->battery_pct);
        bwm_text(bb, pitch, tx + 62, (PANEL_H - BGFX_FONT_H) / 2, bb2,
                 t->error, t->panel);
    }
    bwm_icon(bb, pitch, tx + 100, (PANEL_H - 18) / 2, "wifi", 18,
             s->clock_hh % 2 ? t->muted : t->text, t->panel);
}

/* ---- notifications ----------------------------------------------------- */

static void draw_notifications(bshell_t *s, const void *bb, int pitch)
{
    const btheme_t *t = s->theme;
    int W = s->scrn->w;
    int x = W - 320;
    int y = PANEL_H + 8;
    int w = 312;
    s->nnotifs = bnotify_list(s->notifs, BNOTIFY_CAP);
    for (int i = 0; i < s->nnotifs; i++) {
        bnotify_t *n = &s->notifs[i];
        if (!n->title[0])
            continue;
        uint32_t bar = n->priority >= BNOTIFY_PRIO_HIGH ? t->error :
                       (n->priority == BNOTIFY_PRIO_NORMAL ? t->accent : t->warning);
        bwm_fill(bb, pitch, x, y, w, 46, t->surface);
        bwm_fill(bb, pitch, x, y, w, 3, bar);
        bwm_rect(bb, pitch, x, y, w, 46, t->border);
        bwm_icon(bb, pitch, x + 6, y + 4, n->icon[0] ? n->icon : "notifications",
                 18, bar, t->surface);
        bwm_text(bb, pitch, x + 30, y + 3, n->title, t->text, t->surface);
        /* message, bounded to the popup width (no overlap with edge) */
        int msgmax = w - 2 * 8;
        char buf[BNOTIFY_MAX_MSG];
        size_t len = strlen(n->msg);
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, n->msg, len);
        buf[len] = '\0';
        int maxw = (msgmax - 8) / BGFX_FONT_W;   /* chars that fit */
        if ((int)len > maxw)
            len = maxw > 0 ? maxw : 0;
        char tmp[BNOTIFY_MAX_MSG];
        memcpy(tmp, buf, len);
        tmp[len] = '\0';
        bwm_text(bb, pitch, x + 8, y + 22, tmp, t->muted, t->surface);
        y += 50;
        if (y >= s->scrn->h - 10)
            break;
    }
}

/* ---- frame entry points ------------------------------------------------ */

void bshell_draw_foreground(bshell_t *s, const void *bb, int pitch)
{
    draw_panel(s, bb, pitch);
    draw_notifications(s, bb, pitch);
    /* keep the compositor's launcher pointer-trap in sync each frame */
    if (s->launcher_open && s->launcher_nvis > 0) {
        int x, y, w, h;
        bshell_launcher_rect(s, &x, &y, &w, &h);
        bwm_set_launcher(1, x, y, w, h);
        bshell_draw_launcher(s, bb, pitch);
    } else {
        bwm_set_launcher(0, 0, 0, 0, 0);
    }
}

/* ---- pointer handling -------------------------------------------------- */

/* map a click at (px,py) to a shell action; returns nonzero if consumed */
int bshell_pointer(bshell_t *s, const binput_event_t *ev)
{
    const bwm_pointer_t *ptr = bwm_pointer();
    int px = ptr->x, py = ptr->y;
    if (ev->type != EV_KEY || ev->code != BTN_LEFT)
        return 0;
    int down = (ev->value == 1);
    if (!down)
        return 0;

    int lw, tx, cx;
    bshell_panel_zones(s, &lw, &tx, &cx);

    /* launcher toggle */
    if (py < PANEL_H && px < lw) {
        s->launcher_open = !s->launcher_open;
        if (s->launcher_open) {
            s->launcher_query[0] = '\0';
            launcher_refilter(s);
        }
        s->launcher_hover = -1;
        return 1;
    }

    /* click on empty panel area closes the launcher */
    if (py < PANEL_H) {
        s->launcher_open = 0;
        return 0;
    }

    /* launcher item selection */
    if (s->launcher_open && px >= 4 && px < 4 + BSHELL_LAUNCHER_W &&
        py >= launcher_root(s)) {
        int idx = (py - launcher_root(s) - 14) / (BGFX_FONT_H + 6);
        const bapp_t *a = launcher_item(s, idx);
        if (a) {
            s->launcher_open = 0;
            s->launcher_hover = -1;
            s->launcher_query[0] = '\0';
            bshell_launch(s, a);
            return 1;
        }
        return 1;
    }

    /* click anywhere else closes launcher */
    if (s->launcher_open) {
        s->launcher_open = 0;
        s->launcher_hover = -1;
    }
    return 0;
}

/* ---- keyboard: launcher type-to-search -------------------------------- */
int bshell_key(bshell_t *s, const binput_event_t *ev)
{
    if (ev->type != EV_KEY || ev->value != 1)
        return 0;
    if (!s->launcher_open)
        return 0;

    int code = ev->code;
    if (code == KEY_ESC) {
        if (s->launcher_query[0]) {
            s->launcher_query[0] = '\0';
            launcher_refilter(s);
        } else {
            s->launcher_open = 0;
        }
        return 1;
    }
    if (code == KEY_ENTER) {
        const bapp_t *a = launcher_item(s, s->launcher_hover >= 0
                                             ? s->launcher_hover : 0);
        if (a) {
            s->launcher_open = 0;
            s->launcher_query[0] = '\0';
            s->launcher_hover = -1;
            bshell_launch(s, a);
        }
        return 1;
    }
    if (code == KEY_BACKSPACE) {
        size_t len = strlen(s->launcher_query);
        if (len > 0) {
            s->launcher_query[len - 1] = '\0';
            launcher_refilter(s);
            if (s->launcher_hover >= s->launcher_nvis)
                s->launcher_hover = s->launcher_nvis - 1;
        }
        return 1;
    }
    if (code == KEY_UP) {
        if (s->launcher_hover > 0)
            s->launcher_hover--;
        return 1;
    }
    if (code == KEY_DOWN) {
        if (s->launcher_hover < s->launcher_nvis - 1)
            s->launcher_hover++;
        return 1;
    }
    /* append printable characters (a-z, digits, space) to the query */
    if (code >= KEY_1 && code <= KEY_0) {
        char c = "1234567890"[code - KEY_1];
        if (strlen(s->launcher_query) < sizeof(s->launcher_query) - 1) {
            size_t l = strlen(s->launcher_query);
            s->launcher_query[l] = c;
            s->launcher_query[l + 1] = '\0';
            launcher_refilter(s);
            if (s->launcher_nvis > 0 && s->launcher_hover < 0)
                s->launcher_hover = 0;
        }
        return 1;
    }
    if (code >= KEY_A && code <= KEY_Z) {
        char c = 'a' + (code - KEY_A);
        if (strlen(s->launcher_query) < sizeof(s->launcher_query) - 1) {
            size_t l = strlen(s->launcher_query);
            s->launcher_query[l] = c;
            s->launcher_query[l + 1] = '\0';
            launcher_refilter(s);
            if (s->launcher_nvis > 0 && s->launcher_hover < 0)
                s->launcher_hover = 0;
        }
        return 1;
    }
    if (code == KEY_SPACE) {
        if (strlen(s->launcher_query) < sizeof(s->launcher_query) - 1) {
            size_t l = strlen(s->launcher_query);
            s->launcher_query[l] = ' ';
            s->launcher_query[l + 1] = '\0';
            launcher_refilter(s);
        }
        return 1;
    }
    return 0;
}

/* ---- ticking ----------------------------------------------------------- */

void bshell_tick(bshell_t *s, int now_ms)
{
    s->notify_time_ms = (uint32_t)now_ms;
    bnotify_tick(now_ms);
    s->nnotifs = bnotify_list(s->notifs, BNOTIFY_CAP);
    if (now_ms % 60000 < 50) {
        s->clock_mm++;
        if (s->clock_mm >= 60) {
            s->clock_mm = 0;
            s->clock_hh = (s->clock_hh % 24) + 1;
        }
    }
}

/* ---- built-in demo apps ------------------------------------------------- */

void bshell_launch(bshell_t *s, const bapp_t *app)
{
    if (!app)
        return;
    /* built-in apps run in-process on the appui framework */
    if (app->builtin) {
        int win = bapps_open(app->exec);
        if (win > 0) {
            /* the appui window is already focused by the app */
            return;
        }
        bnotify_send(app->name, "This app could not be opened.", app->icon,
                     BNOTIFY_PRIO_HIGH, 4000);
        return;
    }
    /* external apps are launched with the safe fork/execve path */
    long pid = bapp_launch(app, 0, NULL);
    if (pid > 0) {
        /* show a brief notification so the user sees it started */
        bnotify_send(app->name, "Application launched.", app->icon,
                     BNOTIFY_PRIO_NORMAL, 4000);
    } else if (pid < 0) {
        bnotify_send(app->name, "Failed to launch.", app->icon,
                     BNOTIFY_PRIO_HIGH, 4000);
    }
}

void bshell_draw_textapp(void *user, const void *bb, int pitch,
                         bwm_rect_t rect, bwm_rect_t clip, int focused)
{
    (void)clip;
    (void)focused;
    bshell_textapp_t *a = (bshell_textapp_t *)user;
    if (!a)
        return;
    const btheme_t *t = bwm_theme_cur();
    int x = rect.x + 8, y = rect.y + 8;
    bwm_text(bb, pitch, rect.x + 4, rect.y + 4, a->title, t->accent,
             t->window);
    y = rect.y + 8 + BGFX_FONT_H + 6;
    for (int i = 0; i < a->nlines && y + BGFX_FONT_H < rect.y + rect.h; i++) {
        if (a->lines[i][0])
            bwm_text(bb, pitch, x, y, a->lines[i], t->text, t->window);
        y += BGFX_FONT_H + 3;
    }
}
