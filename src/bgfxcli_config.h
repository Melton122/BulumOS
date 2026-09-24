/*
 * bgfxcli_config.h - parser for /etc/bulum/graphics.conf (Phase 8)
 *
 * Format: key=value lines, '#' comments.  Supported settings:
 *   backend=auto|framebuffer|drm
 *   resolution=auto          (only "auto" is honoured this phase)
 *   input=auto               (only "auto" is honoured this phase)
 *
 * Unknown keys and unsupported values produce warnings but never
 * fail the parser: the caller keeps its defaults.
 */

#ifndef BGFXCLI_CONFIG_H
#define BGFXCLI_CONFIG_H

#include <stdio.h>
#include <string.h>

static int bgfxcli_read_config(const char *path, char *backend, size_t backend_sz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
        while (*v == ' ' || *v == '\t')
            v++;
        char *key = p;
        size_t kl = strlen(key);
        while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t'))
            key[--kl] = '\0';

        if (strcmp(key, "backend") == 0) {
            if (strcmp(v, "auto") == 0 || strcmp(v, "framebuffer") == 0 ||
                strcmp(v, "drm") == 0) {
                snprintf(backend, backend_sz, "%s", v);
            } else {
                fprintf(stderr, "bgfx: warning: unsupported backend '%s' (auto|framebuffer|drm)\n", v);
            }
        } else if (strcmp(key, "resolution") == 0) {
            if (strcmp(v, "auto") != 0)
                fprintf(stderr, "bgfx: warning: resolution '%s' unsupported in Phase 8 (auto only)\n", v);
        } else if (strcmp(key, "input") == 0) {
            if (strcmp(v, "auto") != 0)
                fprintf(stderr, "bgfx: warning: input '%s' unsupported in Phase 8 (auto only)\n", v);
        } else {
            fprintf(stderr, "bgfx: warning: unknown graphics.conf key '%s'\n", key);
        }
    }
    fclose(f);
    return 0;
}

#endif /* BGFXCLI_CONFIG_H */