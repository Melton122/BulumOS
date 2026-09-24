/*
 * bshell_internal.h - internal helpers shared within bshell.c
 *
 * Split out so bshell.c stays readable: these are not part of the public
 * shell API but are used by more than one function in the translation unit.
 */

#ifndef BSHELL_INTERNAL_H
#define BSHELL_INTERNAL_H

#include "bshell.h"

/* Launch an app from the launcher (built-in opens a demo window, external
 * apps are spawned safely via bapp_launch). */
void bshell_launch(bshell_t *s, const bapp_t *app);

/* Panel hit-zone geometry: launcher width, tray x, clock x. */
void bshell_panel_zones(bshell_t *s, int *launcher_w,
                        int *tray_x, int *clock_x);

/* Draw the launcher popup menu (called by bshell_draw_foreground). */
void bshell_draw_launcher(bshell_t *s, const void *bb, int pitch);

/* Bounded geometry of the launcher menu (used for both drawing and the
 * compositor's pointer-trap region). */
void bshell_launcher_rect(bshell_t *s, int *x, int *y, int *w, int *h);

#endif /* BSHELL_INTERNAL_H */
