/*
 * bsys.c - shared safe system-state helpers for Phase 10 desktop apps.
 *
 * See bsys.h.  The subprocess runner uses fork/execve+pipe only - no
 * system()/popen() - captures bounded stdout, and reaps the child so no
 * zombies/orphans can accumulate.  All /proc reads are length-bounded line
 * parses; preferences.conf is validated on load (never crashes) before use.
 */

#define _GNU_SOURCE

#include "bsys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* subprocess                                                               */

static const char *const ROOTS[] = { BSYS_ROOTS };

static int exec_path_ok(const char *exe)
{
    if (!exe || !exe[0] || exe[0] != '/')
        return 0;
    for (size_t i = 0; i < sizeof(ROOTS) / sizeof(ROOTS[0]); i++)
        if (strncmp(exe, ROOTS[i], strlen(ROOTS[i])) == 0 &&
            strchr(exe + strlen(ROOTS[i]), '/') == NULL)   /* no subdirs */
            return 1;
    return 0;
}

int bsys_exec(const char *exe, const char **argv, int argc, bsys_result_t *res)
{
    memset(res, 0, sizeof(*res));
    if (!exec_path_ok(exe) || argc < 0 || argc > BSYS_MAX_ARG - 2)
        return -1;

    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        const char *av[BSYS_MAX_ARG];
        int ai = 0;
        av[ai++] = exe;
        for (int i = 0; i < argc; i++)
            av[ai++] = argv[i];
        av[ai] = NULL;
        execve(exe, (char **)av, NULL);
        _exit(127);
    }

    /* parent: read bounded output, then reap */
    close(pfd[1]);
    size_t got = 0;
    while (got < BSYS_CAP - 1) {
        ssize_t n = read(pfd[0], res->out + got, BSYS_CAP - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(pfd[0]);
    res->out[got > (size_t)BSYS_CAP - 1 ? BSYS_CAP - 1 : got] = '\0';
    res->outlen = (int)got;
    /* if we hit the cap, child may still be writing; waitpid reaps it and it
     * gets EPIPE -> terminates, so no deadlock, no zombie. */
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    res->rc = (WIFEXITED(st)) ? WEXITSTATUS(st)
             : (WIFSIGNALED(st)) ? 128 + WTERMSIG(st) : -1;
    return 0;
}

void bsys_strip_ansi(const char *src, char *dst, size_t dstsz)
{
    size_t d = 0;
    for (size_t i = 0; src[i] && d + 1 < dstsz; i++) {
        if (src[i] == 0x1b && src[i + 1] == '[') {
            i += 2;
            while (src[i] && d + 1 < dstsz) {
                if ((src[i] >= '0' && src[i] <= '9') || src[i] == ';')
                    i++;
                else
                    break;
            }
            if (src[i] && (src[i] == 'm' || src[i] == 'K' || src[i] == 'J'))
                continue;
            if (src[i])
                continue;
        } else {
            dst[d++] = src[i];
        }
    }
    dst[d] = '\0';
}

void bsys_trim(char *s)
{
    if (!s)
        return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start]))
        start++;
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

static void read_kb(const char *path, const char *key, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            char *p = line + strlen(key);
            while (*p && !isdigit((unsigned char)*p))
                p++;
            *out = atol(p);
            break;
        }
    }
    fclose(f);
}

int bsys_cpu_mem(int *cpu_pct, long *mem_total_kb, long *mem_avail_kb,
                 long *uptime_s)
{
    if (cpu_pct) {
        static unsigned long last_total = 0, last_idle = 0;
        unsigned long total = 0, idle = 0;
        FILE *f = fopen("/proc/stat", "r");
        if (f) {
            char line[256];
            if (fgets(line, sizeof(line), f)) {
                unsigned long a = 0, b = 0, c = 0, d = 0, e = 0, f1 = 0, g = 0;
                if (sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu",
                           &a, &b, &c, &d, &e, &f1, &g) >= 4) {
                    idle = d;
                    total = a + b + c + d + e + f1 + g;
                }
            }
            fclose(f);
        }
        int uso = 0;
        if (last_total && total >= last_total) {
            unsigned long dt = total - last_total;
            unsigned long di = idle - last_idle;
            int p = 100;
            if (dt > 0) {
                p = (int)((dt > di ? dt - di : 0) * 100UL / dt);
                if (p > 100) p = 100;
                if (p < 0) p = 0;
            }
            uso = p;
        }
        last_total = total;
        last_idle = idle;
        *cpu_pct = uso;
    }
    long total = -1, avail = -1;
    read_kb("/proc/meminfo", "MemTotal:", &total);
    read_kb("/proc/meminfo", "MemAvailable:", &avail);
    if (mem_total_kb) *mem_total_kb = total;
    if (mem_avail_kb) *mem_avail_kb = avail;

    if (uptime_s) {
        FILE *f = fopen("/proc/uptime", "r");
        double up = 0;
        if (f) {
            if (fscanf(f, "%lf", &up) != 1)
                up = 0;
            fclose(f);
        }
        *uptime_s = (long)up;
    }
    return 0;
}

int bsys_kernel(char *kern, size_t kernsz)
{
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (!f)
        return -1;
    if (fgets(kern, (int)kernsz, f)) {
        bsys_trim(kern);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

int bsys_hostname(char *hn, size_t hnz)
{
    if (gethostname(hn, hnz - 1) != 0)
        return -1;
    hn[hnz - 1] = '\0';
    return 0;
}

const char *bsys_arch(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    return "aarch64";
#elif defined(__x86_64__) || defined(__amd64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

int bsys_process_snapshot(bsys_proc_t *procs, int max)
{
    if (max > BSYS_MAX_PROC)
        max = BSYS_MAX_PROC;
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const char *nm = e->d_name;
        if (!isdigit((unsigned char)nm[0]))
            continue;
        long pid = atol(nm);
        if (pid <= 0)
            continue;
        char p[64];
        snprintf(p, sizeof(p), "/proc/%ld/stat", pid);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        char line[512];
        if (fgets(line, sizeof(line), f)) {
            char *cp = strrchr(line, ')');
            if (cp) {
                char *lp = strchr(line, '(');
                size_t clen = (size_t)(cp - lp - 1);
                if (clen >= sizeof(procs[n].comm))
                    clen = sizeof(procs[n].comm) - 1;
                memcpy(procs[n].comm, lp + 1, clen);
                procs[n].comm[clen] = '\0';
                /* tokens after ") ": field 3 (state) onwards; index i = field i+3 */
                unsigned long vals[24] = {0};
                int vi = 0;
                char *tmp = strdup(cp + 2);
                if (tmp) {
                    char *tok = strtok(tmp, " ");
                    while (tok && vi < 24) {
                        vals[vi++] = strtoul(tok, NULL, 10);
                        tok = strtok(NULL, " ");
                    }
                    free(tmp);
                }
                /* field14=utime(idx11), field15=stime(idx12), field24=rss(idx21) */
                unsigned long utime = vals[11], stime = vals[12];
                long rss_pages = (long)vals[21];
                procs[n].pid = pid;
                procs[n].utime_ms = (long)(utime + stime);
                procs[n].rss_kb = rss_pages * (long)(sysconf(_SC_PAGESIZE) / 1024);
                n++;
            }
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

long bsys_proc_pct(const bsys_proc_t *a, const bsys_proc_t *b, long hertz)
{
    /* cpu% = delta ticks / (delta seconds in hertz) */
    if (!a || !b || hertz <= 0 || b->pid != a->pid ||
        b->utime_ms < a->utime_ms)
        return 0;
    long dt = b->utime_ms - a->utime_ms;
    if (dt <= 0)
        return 0;
    long p = dt * 100L / hertz;
    return p > 100 ? 100 : p;
}

/* ------------------------------------------------------------------------ */
/* preferences                                                               */

void bpref_defaults(bpref_t *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->theme, sizeof(p->theme), "dark");
    p->panel_visible = 1;
    p->desktop_icons = 1;
    snprintf(p->clock_format, sizeof(p->clock_format), "24h");
    p->launcher_auto_close = 1;
}

int bpref_load(bpref_t *p, const char *path)
{
    bpref_defaults(p);
    if (!path || !path[0])
        path = "/etc/bulum/preferences.conf";
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;               /* missing file -> defaults, not an error */
    char line[128];
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
        bsys_trim(val);
        if (strcmp(line, "theme") == 0) {
            if (!strcmp(val, "light") || !strcmp(val, "dark"))
                snprintf(p->theme, sizeof(p->theme), "%s", val);
        } else if (strcmp(line, "panel") == 0) {
            p->panel_visible = (!strcmp(val, "1") || !strcmp(val, "yes") ||
                                !strcmp(val, "on"));
        } else if (strcmp(line, "desktop_icons") == 0) {
            p->desktop_icons = (!strcmp(val, "1") || !strcmp(val, "yes"));
        } else if (strcmp(line, "clock_format") == 0) {
            if (!strcmp(val, "12h") || !strcmp(val, "24h"))
                snprintf(p->clock_format, sizeof(p->clock_format), "%s", val);
        } else if (strcmp(line, "launcher_auto_close") == 0) {
            p->launcher_auto_close = (!strcmp(val, "1") || !strcmp(val, "yes"));
        }
    }
    fclose(f);
    return 0;
}

int bpref_save(const bpref_t *p, const char *path)
{
    if (!path || !path[0])
        path = "/etc/bulum/preferences.conf";
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    const char *theme = (!strcmp(p->theme, "light")) ? "light" : "dark";
    const char *cf = (!strcmp(p->clock_format, "12h")) ? "12h" : "24h";
    fprintf(f, "theme=%s\n", theme);
    fprintf(f, "panel=%s\n", p->panel_visible ? "1" : "0");
    fprintf(f, "desktop_icons=%s\n", p->desktop_icons ? "1" : "0");
    fprintf(f, "clock_format=%s\n", cf);
    fprintf(f, "launcher_auto_close=%s\n", p->launcher_auto_close ? "1" : "0");
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* clipboard                                                                 */

static char g_clip[BSYS_CLIP_MAX];
static int  g_clip_len = 0;

void bsys_clip_set(const char *text, int len)
{
    if (len <= 0 && text)
        len = (int)strlen(text);
    if (len > BSYS_CLIP_MAX - 1)
        len = BSYS_CLIP_MAX - 1;
    if (len < 0)
        len = 0;
    g_clip_len = len;
    if (len > 0 && text)
        memcpy(g_clip, text, (size_t)len);
    g_clip[g_clip_len] = '\0';
}

const char *bsys_clip_get(void)
{
    return g_clip;
}

int bsys_clip_has(void)
{
    return g_clip_len > 0;
}
