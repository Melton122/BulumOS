/*
 * bappui.h - BulumOS native application framework (Phase 10)
 *
 * A tiny, C-native toolkit for the in-process desktop applications.  It wraps
 * the bwm compositor's window + draw/event model into a simple widget system
 * (labels, buttons, lists, single-line input, checkable rows, text areas) so
 * the Phase 10 apps (Files, Settings, Packages, System Monitor, Network,
 * Security, Services, Logs, Users, About) share one small, dependency-free
 * UI core.
 *
 * The framework is deliberately small: no layout engine, no styling engine,
 * no cross-process IPC.  Apps place widgets explicitly in their window body
 * and the framework handles hit-testing, keyboard focus, scrolling and
 * drawing against the bwm back buffer.
 *
 * All buffers are bounded; text fields are length-capped; every draw goes
 * through the clipped bwm helpers so nothing can render out of range.
 */

#ifndef BAPPUI_H
#define BAPPUI_H

#include "bwm.h"
#include "binput.h"

#define BAPPUI_MAX_WIDGETS 64      /* widgets per window (bounded)     */
#define BAPPUI_MAX_TEXT   256      /* widget label/input buffer        */
#define BAPPUI_MAX_MSG     128     /* internal scratch                 */
#define APPUI_FONT_H (16)          /* 8x16 font row height             */
#define APPUI_BTN_H  (22)          /* standard button height           */
#define APPUI_ROW_H  (20)          /* standard list row height         */

/* ---- widget kinds ------------------------------------------------------ */
enum {
    APPUI_LABEL = 1,   /* static text line                              */
    APPUI_BUTTON,      /* push button (fires APPUI_EVT_ACTIVATE)       */
    APPUI_LIST,        /* scrollable selection list + optional header   */
    APPUI_INPUT,       /* single-line text field (cursor + ctrl)         */
    APPUI_CHECK,       /* checkable row / toggle                        */
    APPUI_TEXTAREA,    /* multi-line scrollable text view               */
    APPUI_SEPARATOR,   /* horizontal rule                               */
};

/* ---- widget flags ------------------------------------------------------ */
enum {
    APPUI_F_HIDDEN  = 1 << 0,    /* not drawn / not hit-tested        */
    APPUI_F_DISABLED = 1 << 1,   /* drawn muted, not activatable      */
};

/* ---- app events raised on a widget ------------------------------------- */
enum {
    APPUI_EVT_ACTIVATE = 1,   /* button pressed / check toggled / row opened */
    APPUI_EVT_SELECT,         /* list selection changed                       */
    APPUI_EVT_CHANGE,         /* input text changed / check toggled           */
    APPUI_EVT_NEEDS_ROW,      /* app must fill row text for a new visible row */
    APPUI_EVT_CLICK,          /* generic click (e.g. to re-focus input)       */
};

typedef struct appui_s appui_t;

/* Row-text provider: called by the list before drawing each visible row so
 * the app can populate the row's text (or update it live).  The app returns
 * the session-relative row index it filled, or -1 to leave the row empty. */
typedef int (*appui_row_fn)(appui_t *ui, int widget, int row);

/* widget field for lists combined with app data */
typedef struct {
    int  kind;
    int  x, y, w, h;          /* geometry rel. to window body          */
    int  id;                  /* app-assigned widget id                */
    unsigned flags;
    char text[BAPPUI_MAX_TEXT]; /* label / button / input / textarea   */

    /* list / textarea state */
    int row_h;                /* row (or line) height                  */
    int nrows;                /* number of usable rows                 */
    int first;                /* first visible row / scroll offset     */
    int selected;             /* selected row                          */
    int last_sel;             /* previous selection (for open vs select) */
    int total;                /* total items (for scrollbar ratio)     */
    int line_h;               /* textarea line height                  */
    int nlines;               /* textarea visible line count           */
    int topline;              /* textarea scroll offset (in lines)     */
    int follow;               /* textarea: keep scrolled to end        */

    int value;                /* check 0/1, input cursor               */
    appui_row_fn rowfn;       /* list row-text provider                */

    /* app-owned per-widget extra */
    void *extra;
} appui_widget_t;

/* widget coordinate space: (0,0) is the top-left of the window BODY (below
 * the title bar).  Apps build windows with appui_win_create() then add
 * widgets with appui_add(); the framework owns drawing + events. */

/* ---- app callbacks (thin wrappers over bwm draw/event) ------------------ */
struct appui_s {
    int   win;                 /* bwm window id (0 = none)             */
    int   x, y, w, h;          /* last-known window geometry           */
    const btheme_t *theme;     /* active theme                         */
    appui_widget_t widgets[BAPPUI_MAX_WIDGETS];
    int   nwidgets;
    int   focus;               /* focused widget id (keyboard target)  */
    int   dirty;               /* request a redraw next frame          */
    int   last_widget;         /* widget id that produced the last event */

    /* set by the app to react to a widget event (APPUI_EVT_*).  The app
     * inspects ui->last_widget and the widget's state (selected, value). */
    void (*on_event)(appui_t *ui, int evt);

    /* app-owned data (each app casts/uses this freely) */
    void *extra;
};

/* ---- lifecycle ---------------------------------------------------------- */
/* Create an app window using the framework.  `draw`/`event` are optional
 * extra callbacks chained after the framework's own (may be NULL).  Returns
 * 0 on success, -1 on failure. */
int  appui_win_create(appui_t *ui, const char *title, int x, int y, int w, int h,
                      int min_w, int min_h, int max_w, int max_h);
void appui_win_set_title(appui_t *ui, const char *title);
void appui_win_close(appui_t *ui);
int  appui_win_valid(const appui_t *ui);

/* ---- widget construction ------------------------------------------------- */
int  appui_add(appui_t *ui, appui_widget_t w);
appui_widget_t *appui_widget(appui_t *ui, int wid);         /* find by id */
void appui_clear(appui_t *ui);

/* ---- drawing + event plumbing (used as the bwm draw/event callbacks) ---- */
void appui_draw(void *user, const void *bb, int pitch, bwm_rect_t body,
                bwm_rect_t clip, int focused);
int  appui_event(void *user, const binput_event_t *ev, int x, int y);

/* ---- convenience helpers -------------------------------------------------- */
void appui_notify(appui_t *ui, const char *title, const char *msg,
                  const char *icon, int prio);
void appui_set_active(appui_t *ui);        /* focus/raise the window */

/* ---- widget accessors (used by apps + list row functions) ----------------- */
const char *appui_get_text(appui_t *ui, int wid);
void appui_set_text(appui_t *ui, int wid, const char *text);
void appui_set_rowtext(appui_t *ui, int wid, const char *text); /* for row fns */
void appui_set_total(appui_t *ui, int wid, int total);
void appui_set_selected(appui_t *ui, int wid, int which);  /* 0-based row */
void appui_set_visible(appui_t *ui, int wid, int on);      /* hide/show */
void appui_scroll_top(appui_t *ui, int wid);
void appui_scroll_end(appui_t *ui, int wid);
void appui_reparse_line_count(appui_t *ui, int wid, const char *text);

/* ---- shared widget factories --------------------------------------------- */
appui_widget_t appui_label(int id, int x, int y, int w, const char *text);
appui_widget_t appui_button(int id, int x, int y, int w, const char *text);
appui_widget_t appui_list(int id, int x, int y, int w, int h, int row_h);
appui_widget_t appui_input(int id, int x, int y, int w, const char *text);
appui_widget_t appui_check(int id, int x, int y, int w, const char *text, int on);
appui_widget_t appui_textarea(int id, int x, int y, int w, int h);
appui_widget_t appui_separator(int id, int x, int y, int w);

#endif /* BAPPUI_H */
