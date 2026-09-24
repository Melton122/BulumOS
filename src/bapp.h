/*
 * bapp.h - BulumOS application registry (Phase 9.11)
 *
 * Lightweight `.bapp` metadata format:
 *   name=Terminal
 *   icon=terminal
 *   exec=/usr/bin/bterm
 *   category=System
 *
 * Applications are launched only through fork()/execve() after path and
 * argument validation.  `system()`/`popen()` are never used for launching.
 */

#ifndef BAPP_H
#define BAPP_H

#include <stdint.h>

#define BAPP_MAX_NAME     64
#define BAPP_MAX_ICON     24
#define BAPP_MAX_EXEC     256
#define BAPP_MAX_CAT      32
#define BAPP_MAX_VERSION  32
#define BAPP_MAX_DESC     160
#define BAPP_MAX_APPS     64     /* bounded registry */

#define BAPP_APPS_DIR     "/usr/share/applications"

typedef struct {
    int    id;                 /* 1-based index */
    char   name[BAPP_MAX_NAME];
    char   icon[BAPP_MAX_ICON];
    char   exec[BAPP_MAX_EXEC];
    char   cat[BAPP_MAX_CAT];
    char   version[BAPP_MAX_VERSION];
    char   desc[BAPP_MAX_DESC];
    int    builtin;            /* 1 = in-process app handled by the shell */
} bapp_t;

/* Clear the registry and rescan the applications dir.  Returns the number
 * of applications registered (0..BAPP_MAX_APPS).  Never fails outright. */
int  bapp_rescan(const char *dir);

/* Registry access -------------------------------------------------------- */
int         bapp_count(void);
const bapp_t *bapp_get(int index);       /* 0..count-1 */
const bapp_t *bapp_find(const char *name);  /* by display name, or NULL */

/* Pack the known built-in apps (used when no .bapp files exist yet / as a
 * fallback so a fresh desktop always has a working launcher). */
int  bapp_register_defaults(void);

/* Safe launch ------------------------------------------------------------ */
/* Validate a command string against the registered registry entry:
 *  - the path must be absolute, canonical, and within whitelisted roots
 *  - no ';', '&', '|', newline, '$' (no shell interpolation possible)
 * Launching builds an argv and calls fork()+execve().  Returns pid, 0 if the
 * app is built-in (in-process), or -1 on validation/launch failure. */
long bapp_launch(const bapp_t *app, int argc, const char **argv);

/* Low-level safe launcher for an already-validated absolute path. */
long bapp_launch_path(const char *exe, const char **argv, int argc);

/* path validation for launch (used internally; exposed for tests) */
int  bapp_path_ok(const char *exec);

/* Work area for the display inside windows (helpers shared by apps). */
int  bapp_is_builtin(const bapp_t *app);

#endif /* BAPP_H */