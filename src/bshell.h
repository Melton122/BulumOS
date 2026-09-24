/*
 * bshell.h - BulumOS desktop shell (Phase 9)
 *
 * The desktop shell drives the bwm compositor.  It owns the on-screen
 * "chrome": the system panel (launcher button, taskbar, status tray), the
 * launcher menu overlay, the desktop background, and transient notification
 * popups.  It also registers a couple of built-in in-process windows so the
 * desktop is usable out of the box, and presents the app registry (bapp) in
 * the launcher.
 *
 * The shell renders into the compositor's back buffer through bwm helpers in
 * the frame callbacks (background and foreground pass), so all pixel work is
 * format-independent and goes through the same double-buffer path.
 */

#ifndef BSHELL_H
#define BSHELL_H

#include "bwm.h"
#include "btheme.h"
#include "bapp.h"
#include "bnotify.h"

#define BSHELL_PANEL_H 34        /* system panel height (px) */
#define BSHELL_LAUNCHER_W 260    /* launcher menu width */
#define BSHELL_MAX_ITEMS (BAPP_MAX_APPS + 8)

typedef struct {
    const btheme_t *theme;
    const bwm_screen_t *scrn;

    /* launcher UI state */
    int launcher_open;
    int launcher_hover;          /* hovered item index (for highlight) */
    int launcher_scroll;         /* first visible item */

    /* launcher keyboard search */
    char launcher_query[32];     /* current search string            */
    int  launcher_vis[BAPP_MAX_APPS]; /* visible (filtered) app ids  */
    int  launcher_nvis;          /* number of visible apps           */

    /* notifications */
    bnotify_t notifs[BNOTIFY_CAP];
    int nnotifs;
    uint32_t notify_time_ms;

    /* clock + tray */
    int clock_hh, clock_mm;
    int battery_pct;

    /* demo window state */
    int demo_win;                /* "About BulumOS" window id (0 = closed) */
    int term_win;                /* "Terminal" demo window id */
} bshell_t;

/* Initialise the shell for the given compositor screen + theme. */
void bshell_init(bshell_t *s, const btheme_t *theme);

/* Register the desktop background / foreground render callbacks on bwm. */
void bshell_register(bshell_t *s);

/* Frame-callback entry points (called by bwm_frame). */
void bshell_draw_background(bshell_t *s, const void *bb, int pitch);
void bshell_draw_foreground(bshell_t *s, const void *bb, int pitch);

/* Handle a pointer event the compositor handed back to the shell
 * (BWM_HIT_PANEL / BWM_HIT_LAUNCHER).  Returns 1 if consumed. */
int bshell_pointer(bshell_t *s, const binput_event_t *ev);

/* Handle a keyboard event while the launcher is open (type-to-search,
 * Enter to launch, Backspace/Escape to clear/close).  Returns 1 if consumed. */
int bshell_key(bshell_t *s, const binput_event_t *ev);

/* Let the shell tick (clock refresh, notification expiry). */
void bshell_tick(bshell_t *s, int now_ms);

/* Helper content renderer for a simple text window (used by demos). */
typedef struct {
    char title[64];
    char lines[8][64];
    int nlines;
    int wrap;
} bshell_textapp_t;
void bshell_draw_textapp(void *user, const void *bb, int pitch,
                         bwm_rect_t rect, bwm_rect_t clip, int focused);

#endif /* BSHELL_H */
