/*
 * btheme.c - BulumOS desktop theme engine (Phase 9.6)
 *
 * Defaults are guaranteed to be a valid, coherent set.  Parsing only ever
 * overwrites colours for keys it recognises; anything else (including a fully
 * malformed file) leaves the current set intact, so a corrupt config can
 * never break the desktop or produce uninitialised values.
 */

#include "btheme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

uint32_t btheme_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void btheme_default_dark(btheme_t *t)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "Bulum Dark");
    t->background      = btheme_rgb(0x16, 0x16, 0x1e);
    t->surface         = btheme_rgb(0x20, 0x20, 0x2a);
    t->panel           = btheme_rgb(0x1c, 0x1c, 0x24);
    t->window          = btheme_rgb(0x26, 0x26, 0x30);
    t->titlebar        = btheme_rgb(0x2c, 0x2c, 0x38);
    t->titlebar_active = btheme_rgb(0x3a, 0x3a, 0x4a);
    t->text            = btheme_rgb(0xe0, 0xe0, 0xe8);
    t->muted           = btheme_rgb(0x90, 0x90, 0x9c);
    t->accent          = btheme_rgb(0x35, 0x9b, 0xe0);
    t->border          = btheme_rgb(0x3a, 0x3a, 0x46);
    t->success         = btheme_rgb(0x3f, 0xb0, 0x6f);
    t->warning         = btheme_rgb(0xe0, 0xa0, 0x3f);
    t->error           = btheme_rgb(0xe0, 0x4f, 0x4f);
    t->wtop = btheme_rgb(0x18, 0x1f, 0x2a);
    t->wbot = btheme_rgb(0x12, 0x12, 0x18);
}

void btheme_default_light(btheme_t *t)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "Bulum Light");
    t->background      = btheme_rgb(0xe8, 0xe8, 0xee);
    t->surface         = btheme_rgb(0xfa, 0xfa, 0xfc);
    t->panel           = btheme_rgb(0xd8, 0xd8, 0xe0);
    t->window          = btheme_rgb(0xf4, 0xf4, 0xf8);
    t->titlebar        = btheme_rgb(0xc8, 0xc8, 0xd2);
    t->titlebar_active = btheme_rgb(0x35, 0x9b, 0xe0);
    t->text            = btheme_rgb(0x1a, 0x1a, 0x22);
    t->muted           = btheme_rgb(0x58, 0x58, 0x64);
    t->accent          = btheme_rgb(0x1e, 0x77, 0xb8);
    t->border          = btheme_rgb(0xb0, 0xb0, 0xbc);
    t->success         = btheme_rgb(0x2e, 0x8b, 0x57);
    t->warning         = btheme_rgb(0xb8, 0x82, 0x1b);
    t->error           = btheme_rgb(0xc0, 0x3a, 0x3a);
    t->wtop = btheme_rgb(0xcf, 0xd8, 0xe6);
    t->wbot = btheme_rgb(0xe0, 0xe0, 0xe8);
}

/* parse one hex colour like "#rrggbb" or "0xrrggbb" exactly (6 hex digits) */
static int parse_color(const char *s, uint32_t *out)
{
    const char *p = s;
    if (*p == '#')
        p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    else
        return -1;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        int c = p[i];
        if (c >= '0' && c <= '9')      v = (v << 4) | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (uint32_t)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static int match_field(const char *field, const char *key, size_t klen)
{
    return strncmp(field, key, klen) == 0 && field[klen] == '\0';
}

/* btheme_parse: apply every recognised `key=color` line to t. */
int btheme_parse(btheme_t *t, const char *path)
{
    if (!t || !path)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *key = line;
        while (*key && isspace((unsigned char)*key)) key++;
        if (!*key || *key == '#')
            continue;
        char *eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *val = eq + 1;
        while (isspace((unsigned char)*val)) val++;
        size_t klen = strlen(key);
        while (klen && isspace((unsigned char)key[klen - 1])) key[--klen] = '\0';

        uint32_t c;
        if (parse_color(val, &c))
            continue;                       /* not a colour: ignore line */

#define APPLY(k, f) do { if (match_field(key, k, sizeof(k)-1)) { f = c; applied = 1; } } while (0)
        APPLY("background",      t->background);
        APPLY("surface",         t->surface);
        APPLY("panel",           t->panel);
        APPLY("window",          t->window);
        APPLY("titlebar",        t->titlebar);
        APPLY("titlebar-active", t->titlebar_active);
        APPLY("text",            t->text);
        APPLY("muted",           t->muted);
        APPLY("accent",          t->accent);
        APPLY("border",          t->border);
        APPLY("success",         t->success);
        APPLY("warning",         t->warning);
        APPLY("error",           t->error);
#undef APPLY
    }
    fclose(f);
    return applied ? 0 : -1;
}

int btheme_load(btheme_t *t, const char *path)
{
    /* choose which theme to start from */
    const char *src = path ? path : BTHEME_CONF_PATH;
    btheme_default_dark(t);

    /* read desktop.conf `theme=dark|light` to choose the base set */
    FILE *d = fopen(BTHEME_DESKTOP_CONF, "r");
    if (d) {
        char line[128];
        while (fgets(line, sizeof(line), d)) {
            if (strncmp(line, "theme=", 6) == 0) {
                char v[32];
                int vi = 0;
                for (const char *p = line + 6; *p && *p != '\n' && vi < 31; p++, vi++)
                    v[vi] = *p;
                v[vi] = 0;
                if (strcmp(v, "light") == 0)
                    btheme_default_light(t);
            }
        }
        fclose(d);
    }

    /* apply an explicit theme file over the base */
    if (btheme_parse(t, src) == 0)
        return 0;
    return 0; /* default set already valid */
}
