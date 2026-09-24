/*
 * bappui.c - BulumOS native application framework (Phase 10)
 *
 * Implements the tiny widget toolkit declared in bappui.h.  It owns window
 * creation (through bwm), widget hit-testing and keyboard navigation, and
 * draws every widget into the compositor's back buffer through the clipped
 * bwm helpers.  `appui_draw` and `appui_event` are wired into bwm with the
 * window's user pointer set to the appui_t, so apps get a uniform widget UI.
 *
 * All drawing is clipped by bwm; all text is truncated to widget width; all
 * buffers are bounded.  No system()/popen(), no unbounded allocation.
 */

#include "bappui.h"
#include "btheme.h"
#include "bnotify.h"
#include "bgfx_font.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* clip text to a max number of characters (with a trailing "...")          */
static void trunc_str(char *dst, size_t dstsz, const char *src, int maxch)
{
    if (maxch < 0)
        maxch = 0;
    size_t cap = dstsz - 1;
    size_t n = strlen(src);
    if (n > cap)
        n = cap;
    memcpy(dst, src, n);
    dst[n] = '\0';
    if ((int)n > maxch && maxch > 0) {
        dst[maxch] = '\0';
        if (maxch > 3) {
            dst[maxch - 1] = '.';
            dst[maxch - 2] = '.';
            dst[maxch - 3] = '.';
        }
    }
}

/* ------------------------------------------------------------------------ */
/* widget draw routines (draw helpers receive wid->x/y as ABSOLUTE coords;   */
/* appui_draw sets them absolute before calling and restores afterwards)     */

static void draw_button(appui_t *ui, appui_widget_t *wid, const void *bb,
                        int pitch)
{
    const btheme_t *t = ui->theme;
    const bwm_pointer_t *ptr = bwm_pointer();
    int dis = (wid->flags & APPUI_F_DISABLED) != 0;
    int hover = !dis && ptr->x >= wid->x && ptr->x < wid->x + wid->w &&
                ptr->y >= wid->y && ptr->y < wid->y + wid->h;
    uint32_t base = dis ? (t->surface & 0x00ffffff) :
                   hover ? t->accent : t->surface;
    bwm_fill(bb, pitch, wid->x, wid->y, wid->w, wid->h, base);
    bwm_rect(bb, pitch, wid->x, wid->y, wid->w, wid->h,
             dis ? t->muted : (hover ? t->window : t->border));
    char tmp[BAPPUI_MAX_TEXT];
    trunc_str(tmp, sizeof(tmp), wid->text, (wid->w - 10) / BGFX_FONT_W);
    bwm_text(bb, pitch, wid->x + 5, wid->y + (wid->h - BGFX_FONT_H) / 2,
             tmp, dis ? t->muted : (hover ? t->window : t->text), base);
}

static void draw_list(appui_t *ui, appui_widget_t *wid, const void *bb,
                      int pitch)
{
    const btheme_t *t = ui->theme;
    int rh = wid->row_h > 0 ? wid->row_h : APPUI_ROW_H;
    if (wid->nrows <= 0)
        wid->nrows = wid->h / rh;
    int nv = wid->nrows;
    if (nv > wid->h / rh)
        nv = wid->h / rh;
    bwm_fill(bb, pitch, wid->x, wid->y, wid->w, wid->h, t->window);
    bwm_rect(bb, pitch, wid->x, wid->y, wid->w, wid->h, t->border);
    for (int r = 0; r < nv; r++) {
        int ry = wid->y + 1 + r * rh;
        char rowtext[BAPPUI_MAX_TEXT];
        rowtext[0] = '\0';
        if (wid->rowfn) {
            int filled = wid->rowfn(ui, wid->id, r);
            if (filled >= 0) {
                strncpy(rowtext, wid->text, sizeof(rowtext) - 1);
                rowtext[sizeof(rowtext) - 1] = '\0';
            }
        }
        uint32_t rowfg = t->text, rowbg = t->window;
        if (r == wid->selected) {
            rowfg = t->window;
            rowbg = t->accent;
        }
        bwm_fill(bb, pitch, wid->x + 1, ry, wid->w - 2, rh, rowbg);
        if (rowtext[0]) {
            char tmp[BAPPUI_MAX_TEXT];
            trunc_str(tmp, sizeof(tmp), rowtext, (wid->w - 12) / BGFX_FONT_W);
            bwm_text(bb, pitch, wid->x + 6, ry + (rh - BGFX_FONT_H) / 2, tmp,
                     rowfg, rowbg);
        }
    }
}

static void draw_input(appui_t *ui, appui_widget_t *wid, const void *bb,
                       int pitch)
{
    const btheme_t *t = ui->theme;
    int focused = (ui->focus == wid->id);
    int h = wid->h > 0 ? wid->h : APPUI_BTN_H;
    bwm_fill(bb, pitch, wid->x, wid->y, wid->w, h, t->surface);
    bwm_rect(bb, pitch, wid->x, wid->y, wid->w, h,
             focused ? t->accent : t->border);
    char tmp[BAPPUI_MAX_TEXT];
    trunc_str(tmp, sizeof(tmp), wid->text, (wid->w - 12) / BGFX_FONT_W);
    bwm_text(bb, pitch, wid->x + 5, wid->y + (h - BGFX_FONT_H) / 2, tmp,
             t->text, t->surface);
    if (focused) {
        int cx = wid->x + 6 + bwm_text_width(tmp);
        bwm_vline(bb, pitch, cx, wid->y + 4, h - 8, t->accent);
    }
}

static void draw_check(appui_t *ui, appui_widget_t *wid, const void *bb,
                       int pitch)
{
    const btheme_t *t = ui->theme;
    int box = BGFX_FONT_H;
    int x = wid->x, y = wid->y;
    bwm_rect(bb, pitch, x, y, box, box, t->border);
    if (wid->value) {
        bwm_fill(bb, pitch, x + 1, y + 1, box - 2, box - 2, t->accent);
        int c = x + box / 2;
        bwm_vline(bb, pitch, c, y + 3, box - 6, t->window);
        bwm_hline(bb, pitch, x + 2, c, box - 4, t->window);
    } else {
        bwm_fill(bb, pitch, x + 1, y + 1, box - 2, box - 2, t->window);
    }
    bwm_text(bb, pitch, x + box + 6, y, wid->text, t->text, t->window);
}

static void draw_textarea(appui_t *ui, appui_widget_t *wid, const void *bb,
                          int pitch)
{
    const btheme_t *t = ui->theme;
    int lh = wid->line_h > 0 ? wid->line_h : BGFX_FONT_H + 3;
    if (wid->nlines <= 0)
        wid->nlines = wid->h / lh;
    bwm_fill(bb, pitch, wid->x, wid->y, wid->w, wid->h, t->window);
    bwm_rect(bb, pitch, wid->x, wid->y, wid->w, wid->h, t->border);
    const char *p = wid->text;
    int line = 0;
    while (*p && line < wid->topline + wid->nlines) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (line >= wid->topline) {
            char tmp[BAPPUI_MAX_TEXT];
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            int ly = wid->y + 3 + (line - wid->topline) * lh;
            char ftmp[BAPPUI_MAX_TEXT];
            trunc_str(ftmp, sizeof(ftmp), tmp, (wid->w - 12) / BGFX_FONT_W);
            bwm_text(bb, pitch, wid->x + 5, ly, ftmp, t->text, t->window);
        }
        if (nl)
            p = nl + 1;
        else
            break;
        line++;
    }
}

/* ------------------------------------------------------------------------ */
/* public: draw everything                                                   */

void appui_draw(void *user, const void *bb, int pitch, bwm_rect_t body,
                bwm_rect_t clip, int focused)
{
    appui_t *ui = (appui_t *)user;
    if (!ui)
        return;
    (void)clip;
    (void)focused;
    ui->x = body.x;
    ui->y = body.y;
    ui->w = body.w;
    ui->h = body.h;
    int ox = body.x, oy = body.y;
    for (int i = 0; i < ui->nwidgets; i++) {
        appui_widget_t *wid = &ui->widgets[i];
        if (wid->flags & APPUI_F_HIDDEN)
            continue;
        int savex = wid->x, savey = wid->y;
        wid->x = wid->x + ox;
        wid->y = wid->y + oy;
        switch (wid->kind) {
        case APPUI_LABEL: {
            char tmp[BAPPUI_MAX_TEXT];
            trunc_str(tmp, sizeof(tmp), wid->text,
                      (wid->w > 0 ? wid->w : body.w) / BGFX_FONT_W);
            bwm_text(bb, pitch, wid->x, wid->y, tmp, ui->theme->text,
                     ui->theme->window);
            break;
        }
        case APPUI_BUTTON:
            draw_button(ui, wid, bb, pitch);
            break;
        case APPUI_LIST:
            draw_list(ui, wid, bb, pitch);
            break;
        case APPUI_INPUT:
            draw_input(ui, wid, bb, pitch);
            break;
        case APPUI_CHECK:
            draw_check(ui, wid, bb, pitch);
            break;
        case APPUI_TEXTAREA:
            draw_textarea(ui, wid, bb, pitch);
            break;
        case APPUI_SEPARATOR:
            bwm_hline(bb, pitch, wid->x, wid->y,
                      wid->w > 0 ? wid->w : body.w, ui->theme->border);
            break;
        default:
            break;
        }
        wid->x = savex;
        wid->y = savey;
    }
}

/* ------------------------------------------------------------------------ */
/* widget construction                                                       */

int appui_add(appui_t *ui, appui_widget_t w)
{
    if (!ui || ui->nwidgets >= BAPPUI_MAX_WIDGETS)
        return -1;
    ui->widgets[ui->nwidgets++] = w;
    return 0;
}

appui_widget_t *appui_widget(appui_t *ui, int wid)
{
    if (!ui)
        return NULL;
    for (int i = 0; i < ui->nwidgets; i++)
        if (ui->widgets[i].id == wid)
            return &ui->widgets[i];
    return NULL;
}

void appui_clear(appui_t *ui)
{
    if (!ui)
        return;
    ui->nwidgets = 0;
}

/* ------------------------------------------------------------------------ */
/* window lifecycle                                                          */

int appui_win_create(appui_t *ui, const char *title, int x, int y, int w, int h,
                     int min_w, int min_h, int max_w, int max_h)
{
    if (!ui)
        return -1;
    memset(ui, 0, sizeof(*ui));
    ui->theme = bwm_theme_cur();
    ui->win = bwm_window_create(title, x, y, w, h, min_w, min_h, max_w, max_h,
                                appui_draw, ui);
    if (ui->win <= 0)
        return -1;
    bwm_window_set_event(ui->win, appui_event);
    return 0;
}

void appui_win_set_title(appui_t *ui, const char *title)
{
    if (!ui || ui->win <= 0 || !title)
        return;
    bwindow_t *w = bwm_window_get(ui->win);
    if (w)
        snprintf(w->title, sizeof(w->title), "%s", title);
}

void appui_win_close(appui_t *ui)
{
    if (!ui || ui->win <= 0)
        return;
    bwm_window_destroy(ui->win);
    ui->win = 0;
    free(ui->extra);
    ui->extra = NULL;
}

int appui_win_valid(const appui_t *ui)
{
    return ui && ui->win > 0 && bwm_window_get(ui->win) != NULL;
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

void appui_notify(appui_t *ui, const char *title, const char *msg,
                  const char *icon, int prio)
{
    (void)ui;
    bnotify_send(title, msg, icon, prio, 4000);
}

void appui_set_active(appui_t *ui)
{
    if (ui && ui->win > 0)
        bwm_window_focus(ui->win);
}

/* ------------------------------------------------------------------------ */
/* event handling                                                            */

static int hit_widget(appui_t *ui, int bx, int by)
{
    for (int i = 0; i < ui->nwidgets; i++) {
        appui_widget_t *wid = &ui->widgets[i];
        if (wid->flags & APPUI_F_HIDDEN)
            continue;
        int ww = wid->w, wh = wid->h;
        if (wid->kind == APPUI_INPUT)
            wh = wid->h > 0 ? wid->h : APPUI_BTN_H;
        else if (wid->kind != APPUI_BUTTON && wid->kind != APPUI_LIST)
            continue;
        if (bx >= wid->x && bx < wid->x + ww && by >= wid->y && by < wid->y + wh)
            return i;
    }
    return -1;
}

static void fire(appui_t *ui, appui_widget_t *wid, int evt)
{
    if (!ui || !wid)
        return;
    ui->last_widget = wid->id;
    if (ui->on_event)
        ui->on_event(ui, evt);
}

int appui_event(void *user, const binput_event_t *ev, int x, int y)
{
    appui_t *ui = (appui_t *)user;
    if (!ui)
        return 0;
    int bx = x - ui->x, by = y - ui->y;
    int consumed = 0;

    if (ev->type == EV_REL)
        return 0;
    if (ev->type != EV_KEY)
        return 0;

    if (ev->code == BTN_LEFT) {
        if (ev->value != 1)
            return 0;
        int wi = hit_widget(ui, bx, by);
        if (wi < 0)
            return 0;
        appui_widget_t *wid = &ui->widgets[wi];
        if (wid->flags & APPUI_F_DISABLED)
            return 1;
        int evt = APPUI_EVT_CLICK;
        switch (wid->kind) {
        case APPUI_BUTTON:
            evt = APPUI_EVT_ACTIVATE;
            consumed = 1;
            break;
        case APPUI_CHECK:
            wid->value = !wid->value;
            evt = APPUI_EVT_CHANGE;
            consumed = 1;
            break;
        case APPUI_LIST: {
            int rh = wid->row_h > 0 ? wid->row_h : APPUI_ROW_H;
            int r = (by - wid->y) / rh;
            if (r < 0 || r >= wid->nrows)
                return 1;
            int row = wid->first + r;
            if (row < 0 || row >= wid->total)
                return 1;
            wid->selected = r;
            evt = (wid->selected == wid->last_sel) ? APPUI_EVT_ACTIVATE
                                                   : APPUI_EVT_SELECT;
            wid->last_sel = wid->selected;
            consumed = 1;
            break;
        }
        case APPUI_INPUT:
            ui->focus = wid->id;
            wid->value |= 0x1000;
            evt = APPUI_EVT_CLICK;
            consumed = 1;
            break;
        default:
            return 0;
        }
        fire(ui, wid, evt);
        return consumed;
    }

    if (ev->value != 1)
        return 0;
    if (ui->focus <= 0)
        return 0;
    appui_widget_t *wid = appui_widget(ui, ui->focus);
    if (!wid || (wid->flags & APPUI_F_HIDDEN))
        return 0;

    if (wid->kind == APPUI_INPUT) {
        int len = (int)strlen(wid->text);
        if (ev->code == KEY_BACKSPACE) {
            if (len > 0) {
                wid->text[len - 1] = '\0';
                fire(ui, wid, APPUI_EVT_CHANGE);
            }
            return 1;
        } else if (ev->code == KEY_ENTER) {
            fire(ui, wid, APPUI_EVT_ACTIVATE);
            return 1;
        } else if (ev->code == KEY_LEFT || ev->code == KEY_RIGHT ||
                   ev->code == KEY_UP || ev->code == KEY_DOWN) {
            return 1;
        } else if (ev->code >= KEY_1 && ev->code <= KEY_0) {
            char c = "1234567890"[ev->code - KEY_1];
            if (len < (int)sizeof(wid->text) - 1) {
                wid->text[len] = c;
                wid->text[len + 1] = '\0';
                fire(ui, wid, APPUI_EVT_CHANGE);
            }
            return 1;
        } else if (ev->code >= KEY_A && ev->code <= KEY_Z) {
            char c = 'a' + (ev->code - KEY_A);
            if (len < (int)sizeof(wid->text) - 1) {
                wid->text[len] = c;
                wid->text[len + 1] = '\0';
                fire(ui, wid, APPUI_EVT_CHANGE);
            }
            return 1;
        } else if (ev->code == KEY_SPACE) {
            if (len < (int)sizeof(wid->text) - 1) {
                wid->text[len] = ' ';
                wid->text[len + 1] = '\0';
                fire(ui, wid, APPUI_EVT_CHANGE);
            }
            return 1;
        } else if (ev->code == KEY_DOT || ev->code == KEY_SLASH ||
                   ev->code == KEY_MINUS) {
            char c = ev->code == KEY_DOT ? '.' :
                     ev->code == KEY_SLASH ? '/' : '-';
            if (len < (int)sizeof(wid->text) - 1) {
                wid->text[len] = c;
                wid->text[len + 1] = '\0';
                fire(ui, wid, APPUI_EVT_CHANGE);
            }
            return 1;
        }
        return 0;
    }

    if (wid->kind == APPUI_LIST) {
        if (ev->code == KEY_UP) {
            if (wid->selected > 0) {
                wid->selected--;
                if (wid->selected < wid->first)
                    wid->first = wid->selected;
                fire(ui, wid, APPUI_EVT_SELECT);
            }
            return 1;
        } else if (ev->code == KEY_DOWN) {
            if (wid->selected < wid->total - 1) {
                wid->selected++;
                if (wid->selected >= wid->first + wid->nrows)
                    wid->first = wid->selected - wid->nrows + 1;
                fire(ui, wid, APPUI_EVT_SELECT);
            }
            return 1;
        } else if (ev->code == KEY_ENTER) {
            fire(ui, wid, APPUI_EVT_ACTIVATE);
            return 1;
        }
        return 0;
    }

    if (wid->kind == APPUI_TEXTAREA) {
        if (ev->code == KEY_PAGEUP) {
            wid->topline -= wid->nlines;
            if (wid->topline < 0) wid->topline = 0;
            return 1;
        } else if (ev->code == KEY_PAGEDOWN) {
            wid->topline += wid->nlines;
            return 1;
        } else if (ev->code == KEY_HOME) {
            wid->topline = 0;
            return 1;
        }
        return 0;
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* widget accessors                                                          */

const char *appui_get_text(appui_t *ui, int wid)
{
    appui_widget_t *w = appui_widget(ui, wid);
    return w ? w->text : "";
}

void appui_set_text(appui_t *ui, int wid, const char *text)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w || !text)
        return;
    snprintf(w->text, sizeof(w->text), "%s", text);
}

void appui_set_rowtext(appui_t *ui, int wid, const char *text)
{
    appui_set_text(ui, wid, text);
}

void appui_set_total(appui_t *ui, int wid, int total)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w)
        return;
    if (total < 0)
        total = 0;
    w->total = total;
    if (w->selected > total - 1)
        w->selected = total > 0 ? total - 1 : 0;
    if (w->first > total)
        w->first = total;
}

void appui_set_selected(appui_t *ui, int wid, int which)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w)
        return;
    if (which < 0)
        which = 0;
    if (which > w->total - 1)
        which = w->total > 0 ? w->total - 1 : 0;
    w->selected = which;
    if (w->selected < w->first)
        w->first = w->selected;
    if (w->selected >= w->first + w->nrows)
        w->first = w->selected - w->nrows + 1;
}

void appui_set_visible(appui_t *ui, int wid, int on)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w)
        return;
    if (on)
        w->flags &= ~APPUI_F_HIDDEN;
    else
        w->flags |= APPUI_F_HIDDEN;
}

void appui_scroll_top(appui_t *ui, int wid)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w)
        return;
    if (w->kind == APPUI_TEXTAREA)
        w->topline = 0;
    else
        w->first = 0;
}

void appui_scroll_end(appui_t *ui, int wid)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w)
        return;
    if (w->kind == APPUI_TEXTAREA) {
        int lh = w->line_h > 0 ? w->line_h : BGFX_FONT_H + 3;
        int nlines = w->h > 0 ? w->h / lh : 20;
        int total = 0;
        for (const char *p = w->text; *p; p++)
            if (*p == '\n')
                total++;
        w->nlines = nlines;
        w->topline = total - nlines + 1;
        if (w->topline < 0)
            w->topline = 0;
    } else {
        if (w->total > w->nrows)
            w->first = w->total - w->nrows;
        w->selected = w->total > 0 ? w->total - 1 : 0;
    }
}

void appui_reparse_line_count(appui_t *ui, int wid, const char *text)
{
    appui_widget_t *w = appui_widget(ui, wid);
    if (!w)
        return;
    snprintf(w->text, sizeof(w->text), "%s", text ? text : "");
}

/* ------------------------------------------------------------------------ */
/* widget factory helpers                                                    */

appui_widget_t appui_label(int id, int x, int y, int w, const char *text)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_LABEL;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = BGFX_FONT_H;
    if (text)
        snprintf(wd.text, sizeof(wd.text), "%s", text);
    return wd;
}

appui_widget_t appui_button(int id, int x, int y, int w, const char *text)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_BUTTON;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = APPUI_BTN_H;
    if (text)
        snprintf(wd.text, sizeof(wd.text), "%s", text);
    return wd;
}

appui_widget_t appui_list(int id, int x, int y, int w, int h, int row_h)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_LIST;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = h;
    wd.row_h = row_h > 0 ? row_h : APPUI_ROW_H;
    wd.nrows = h > 0 ? h / wd.row_h : 0;
    wd.selected = 0;
    wd.first = 0;
    wd.total = 0;
    wd.last_sel = -1;
    return wd;
}

appui_widget_t appui_input(int id, int x, int y, int w, const char *text)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_INPUT;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = APPUI_BTN_H;
    if (text)
        snprintf(wd.text, sizeof(wd.text), "%s", text);
    return wd;
}

appui_widget_t appui_check(int id, int x, int y, int w, const char *text, int on)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_CHECK;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = BGFX_FONT_H;
    wd.value = on ? 1 : 0;
    if (text)
        snprintf(wd.text, sizeof(wd.text), "%s", text);
    return wd;
}

appui_widget_t appui_textarea(int id, int x, int y, int w, int h)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_TEXTAREA;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = h;
    wd.line_h = BGFX_FONT_H + 3;
    wd.nlines = h > 0 ? h / wd.line_h : 20;
    wd.topline = 0;
    wd.follow = 0;
    return wd;
}

appui_widget_t appui_separator(int id, int x, int y, int w)
{
    appui_widget_t wd;
    memset(&wd, 0, sizeof(wd));
    wd.kind = APPUI_SEPARATOR;
    wd.id = id;
    wd.x = x; wd.y = y; wd.w = w; wd.h = 1;
    return wd;
}
