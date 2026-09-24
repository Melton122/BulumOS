/*
 * btheme.h - BulumOS desktop theme engine (Phase 9.6)
 *
 * Centralised colours so the desktop never hard-codes UI colours.  A theme is
 * a small set of named colours loaded from /etc/bulum/theme.conf, with Bulum
 * Dark as the built-in default and Bulum Light provided.  Unknown keys are
 * ignored; a malformed file falls back to the default set (never crashes).
 */

#ifndef BTHEME_H
#define BTHEME_H

#include <stdint.h>

#define BTHEME_MAX_NAME 32

#define BTHEME_CONF_PATH   "/etc/bulum/theme.conf"
#define BTHEME_DESKTOP_CONF "/etc/bulum/desktop.conf"

typedef struct {
    char   name[BTHEME_MAX_NAME];

    /* core palette (RGB as 0xRRGGBB) */
    uint32_t background;   /* desktop wallpaper base          */
    uint32_t surface;      /* window/panel surface            */
    uint32_t panel;        /* system panel                    */
    uint32_t window;       /* window body                     */
    uint32_t titlebar;     /* unfocused title                 */
    uint32_t titlebar_active; /* focused title                */
    uint32_t text;         /* primary text                    */
    uint32_t muted;        /* secondary text                  */
    uint32_t accent;       /* highlights / selection          */
    uint32_t border;       /* window borders                  */
    uint32_t success;      /* ok/success                      */
    uint32_t warning;      /* warnings                        */
    uint32_t error;        /* errors / destructive            */

    /* wallpaper: a simple vertical gradient of two colors */
    uint32_t wtop, wbot;
} btheme_t;

void btheme_default_dark(btheme_t *t);
void btheme_default_light(btheme_t *t);

/* Load a theme by name ("" / NULL => default from desktop.conf `theme=`).
 * On any load / parse error the named default set is used and 0 is still
 * returned (the caller can decide how to treat a missing file). */
int btheme_load(btheme_t *t, const char *path);

/* parse a file into the target palette; returns 0 if the file was found and
 * had no fatal errors, -1 if the file was missing/empty.  Never partial. */
int btheme_parse(btheme_t *t, const char *path);

/* helpers ---------------------------------------------------------------- */
uint32_t btheme_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif /* BTHEME_H */