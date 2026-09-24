/*
 * bsys.h - shared safe system-state helpers for Phase 10 desktop apps
 *
 * A dependency-free layer the Phase 10 applications share to:
 *   - run an existing BulumOS CLI tool (bctl, bulum, blog, bnet, bsecure,
 *     buser, bstatus) through fork()/execve() and capture its bounded stdout
 *     (NEVER system()/popen());
 *   - strip ANSI colour escapes from that output so a GUI can parse it;
 *   - read cheap /proc facts (CPU %, memory, uptime, processes) for the
 *     system monitor;
 *   - read/write a validated desktop preferences.conf.
 *
 * Every buffer here is bounded; every path validated; nothing grows.
 */

#ifndef BSYS_H
#define BSYS_H

#include <stddef.h>

#define BSYS_CAP      8192      /* captured stdout cap (bytes)          */
#define BSYS_MAX_ARG  64        /* max argv elements for a child         */
#define BSYS_MAX_PROC 256       /* max process table entries             */
#define BSYS_CLIP_MAX  4096      /* clipboard text cap (bytes)            */

/* whitelisted roots for subprocess executables */
#define BSYS_ROOTS   "/usr/bin/", "/bin/", "/sbin/"

typedef struct {
    int   rc;                   /* child exit status (0 = ok)            */
    char  out[BSYS_CAP];        /* captured stdout (nul-terminated)      */
    int   outlen;               /* bytes in out (excluding nul)          */
} bsys_result_t;

/* Run an absolute executable (validated against whitelisted roots) with an
 * argv, capture its stdout (bounded), and wait for it.  Returns 0 on
 * success (child ran and was reaped), -1 on validation/fork failure.
 * Never returns "success" with unread output: the buffer is bounded. */
int  bsys_exec(const char *exe, const char **argv, int argc, bsys_result_t *res);

/* Copy a whole command into out of up to len chars */
void bsys_strip_ansi(const char *src, char *dst, size_t dstsz);
void bsys_trim(char *s);

/* ---- /proc facts --------------------------------------------------------- */
/* Returns 0 on success.  cpu_pct = 0..100 over the last measurement. */
int  bsys_cpu_mem(int *cpu_pct, long *mem_total_kb, long *mem_avail_kb,
                  long *uptime_s);
int  bsys_kernel(char *kern, size_t kernsz);
int  bsys_hostname(char *hn, size_t hnz);
const char *bsys_arch(void);

/* process snapshot: fills the table + returns count (<= BSYS_MAX_PROC) */
typedef struct {
    long pid;
    char comm[32];
    long utime_ms;             /* cpu ticks -> ms (approx) at snapshot    */
    long rss_kb;
} bsys_proc_t;
int  bsys_process_snapshot(bsys_proc_t *procs, int max);
/* delta-based cpu% for one pid between two snapshots */
long bsys_proc_pct(const bsys_proc_t *a, const bsys_proc_t *b, long hertz);

/* ---- system clipboard ---------------------------------------------------- */
/* A single shared text clipboard for the in-process desktop (bounded). */
void bsys_clip_set(const char *text, int len);
const char *bsys_clip_get(void);     /* never NULL ("" when empty) */
int bsys_clip_has(void);

/* ---- preferences.conf ----------------------------------------------------- */
typedef struct {
    char  theme[16];           /* dark | light                            */
    int   panel_visible;       /* 1 = show system panel                   */
    int   desktop_icons;       /* 1 = show desktop icons                  */
    char  clock_format[16];    /* 12h | 24h                               */
    int   launcher_auto_close; /* 1 = close launcher after launching      */
} bpref_t;

void bpref_defaults(bpref_t *p);
/* Load /etc/bulum/preferences.conf (validate; never crash).  Returns 0. */
int  bpref_load(bpref_t *p, const char *path);
/* Write validated preferences back.  Returns 0 on success. */
int  bpref_save(const bpref_t *p, const char *path);

#endif /* BSYS_H */
