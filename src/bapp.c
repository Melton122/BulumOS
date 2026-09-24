/*
 * bapp.c - BulumOS application registry (Phase 9.11)
 *
 * Parses `.bapp` metadata (length-bounded), then validates and launches
 * executables with fork/execve() on an absolute whitelisted path.  No system()
 * / popen() and no shell metacharacter handling anywhere in this file.
 */

#define _GNU_SOURCE

#include "bapp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bapp_t g_apps[BAPP_MAX_APPS];
static int    g_napps = 0;

int bapp_count(void)
{
    return g_napps;
}

const bapp_t *bapp_get(int index)
{
    if (index < 0 || index >= g_napps)
        return NULL;
    return &g_apps[index];
}

const bapp_t *bapp_find(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < g_napps; i++)
        if (strcmp(g_apps[i].name, name) == 0)
            return &g_apps[i];
    return NULL;
}

int bapp_is_builtin(const bapp_t *app)
{
    return app && app->builtin;
}

static int register_one(const char *name, const char *icon,
                        const char *exec, const char *cat, int builtin,
                        const char *version, const char *desc)
{
    if (g_napps >= BAPP_MAX_APPS)
        return -1;
    bapp_t *a = &g_apps[g_napps++];
    a->id = g_napps;
    a->builtin = builtin;
    snprintf(a->name, sizeof(a->name), "%s", name ? name : "?");
    snprintf(a->icon, sizeof(a->icon), "%s", icon ? icon : "help");
    snprintf(a->exec, sizeof(a->exec), "%s", exec ? exec : "");
    snprintf(a->cat,  sizeof(a->cat),  "%s", cat ? cat : "System");
    snprintf(a->version, sizeof(a->version), "%s", version ? version : "1.0.0");
    snprintf(a->desc, sizeof(a->desc), "%s", desc ? desc : "");
    return 0;
}

int bapp_register_defaults(void)
{
    register_one("Bulum Files",  "files",    "internal:files",    "Files",     1, "1.0.0", "Browse and manage files");
    register_one("Bulum Settings","settings", "internal:settings", "System",    1, "1.0.0", "System and appearance settings");
    register_one("Bulum Packages","packages", "internal:packages", "System",    1, "1.0.0", "Install and remove packages");
    register_one("System Monitor","monitor", "internal:monitor",  "System",    1, "1.0.0", "CPU, memory and processes");
    register_one("Network",      "network",  "internal:network",  "System",    1, "1.0.0", "Network interfaces and DNS");
    register_one("Security",     "security", "internal:security", "Security",  1, "1.0.0", "Security audit and status");
    register_one("Services",     "services", "internal:services", "System",    1, "1.0.0", "Manage bctl services");
    register_one("Logs",         "log",      "internal:logs",     "System",    1, "1.0.0", "View system logs");
    register_one("Users",        "users",    "internal:users",    "System",    1, "1.0.0", "View user accounts");
    register_one("Terminal",     "terminal", "internal:terminal", "Utilities", 1, "1.0.0", "BulumOS terminal (bsh)");
    register_one("About BulumOS","about",    "internal:about",    "System",    1, "1.0.0", "About BulumOS");
    return g_napps;
}

int bapp_rescan(const char *dir)
{
    const char *scan = dir && dir[0] ? dir : BAPP_APPS_DIR;
    g_napps = 0;
    DIR *d = opendir(scan);
    if (!d)
        return bapp_register_defaults();

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 5 || strcmp(nm + l - 5, ".bapp") != 0)
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", scan, nm);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char name[BAPP_MAX_NAME] = "", icon[BAPP_MAX_ICON] = "",
             exec[BAPP_MAX_EXEC] = "", cat[BAPP_MAX_CAT] = "",
             ver[BAPP_MAX_VERSION] = "", desc[BAPP_MAX_DESC] = "";
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (!line[0] || line[0] == '#')
                continue;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = '\0';
            char *val = eq + 1;
            if (strcmp(line, "name") == 0 && name[0] == '\0')
                snprintf(name, sizeof(name), "%s", val);
            else if (strcmp(line, "icon") == 0 && icon[0] == '\0')
                snprintf(icon, sizeof(icon), "%s", val);
            else if (strcmp(line, "exec") == 0 && exec[0] == '\0')
                snprintf(exec, sizeof(exec), "%s", val);
            else if (strcmp(line, "category") == 0 && cat[0] == '\0')
                snprintf(cat, sizeof(cat), "%s", val);
            else if (strcmp(line, "version") == 0 && ver[0] == '\0')
                snprintf(ver, sizeof(ver), "%s", val);
            else if (strcmp(line, "description") == 0 && desc[0] == '\0')
                snprintf(desc, sizeof(desc), "%s", val);
        }
        fclose(f);
        if (!name[0])
            snprintf(name, sizeof(name), "%s", nm);
        int builtin = (strncmp(exec, "internal:", 9) == 0);
        if (!exec[0])
            continue;      /* malformed: no executable path -> skip */
        register_one(name, icon, exec, cat, builtin, ver, desc);
    }
    closedir(d);
    if (g_napps == 0)
        return bapp_register_defaults();
    return g_napps;
}

/* whitelisted roots for absolute executables */
static const char *const ROOTS[] = { "/usr/bin/", "/bin/", "/sbin/" };

int bapp_path_ok(const char *exec)
{
    if (!exec || !exec[0] || exec[0] != '/')
        return 0;
    /* forbid shell metacharacters so nothing can be injected/interpreted */
    if (strchr(exec, ';') || strchr(exec, '|') || strchr(exec, '&') ||
        strchr(exec, '\n') || strchr(exec, '$') || strchr(exec, '`') ||
        strchr(exec, '\''))
        return 0;
    /* no path traversal */
    if (strstr(exec, "/../") || strncmp(exec, "../", 3) == 0)
        return 0;
    /* must be inside a whitelisted root */
    for (size_t i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++)
        if (strncmp(exec, ROOTS[i], strlen(ROOTS[i])) == 0)
            return 1;
    return 0;
}

long bapp_launch_path(const char *exe, const char **argv, int argc)
{
    if (!bapp_path_ok(exe))
        return -1;
    if (argc < 0 || argc > 60)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        const char *av[64];
        int ai = 0;
        av[ai++] = exe;
        for (int i = 0; i < argc; i++)
            av[ai++] = argv[i];
        av[ai] = NULL;
        execve(exe, (char **)av, NULL);
        _exit(127);
    }
    return (long)pid;
}

long bapp_launch(const bapp_t *app, int argc, const char **argv)
{
    if (!app)
        return -1;
    if (app->builtin || strncmp(app->exec, "internal:", 9) == 0)
        return 0;                      /* handled in-process by the shell */
    if (argc < 0 || argc > 60 || !bapp_path_ok(app->exec))
        return -1;
    return bapp_launch_path(app->exec, argv, argc);
}
