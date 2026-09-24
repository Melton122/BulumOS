/*
 * bsh.c - Bulum Shell: a native, self-contained shell for Bulum OS.
 *
 * Features:
 *   - interactive line editing, history, tab completion, PS1/PS2 prompts
 *   - quoting, escapes, variable expansion, globbing, tilde expansion
 *   - redirection: < > >> 2> 2>> 2>&1 >& &> 2>&-
 *   - pipelines, and/or/background operators, job control (fg/bg/jobs)
 *   - builtins: cd pwd echo printf export unset env exit true false
 *               history alias unalias jobs fg bg help
 *   - config files: /etc/bshrc, ~/.bshrc
 *   - non-interactive mode: bsh -c 'cmd', bsh script args...
 *
 * Static build, no external dependencies.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define BSH_NAME "Bulum Shell"
#define BSH_VERSION "0.1.0"

#define MAX_WORDS      4096
#define MAX_STAGES      64
#define MAX_ALIASES    128
#define MAX_JOBS        64
#define MAX_HISTORY    500
#define MAX_HIST_LEN   512
#define MAX_EXPAND   65536
#define MAX_ALIAS_DEPTH 10
#define DEFAULT_PATH "/sbin:/usr/sbin:/bin:/usr/bin"
#define HISTORY_FILE  ".bsh_history"

/* ------------------------------------------------------------------ */
/* dynamic buffer                                                      */
/* ------------------------------------------------------------------ */
typedef struct DBuf {
    char *b;
    size_t len, cap;
} DBuf;

static int db_reserve(DBuf *d, size_t need) {
    if (d->cap - d->len >= need) return 0;
    size_t ncap = d->cap ? d->cap * 2 : 64;
    while (ncap - d->len < need) ncap *= 2;
    if (ncap > MAX_EXPAND) {
        fprintf(stderr, "bsh: expansion too large\n");
        return -1;
    }
    char *nb = realloc(d->b, ncap);
    if (!nb) return -1;
    d->b = nb;
    d->cap = ncap;
    return 0;
}

static void db_init(DBuf *d) { d->b = NULL; d->len = 0; d->cap = 0; }
static void db_free(DBuf *d) { free(d->b); d->b = NULL; d->len = d->cap = 0; }
static int db_putc(DBuf *d, char c) { if (db_reserve(d, 1)) return -1; d->b[d->len++] = c; return 0; }
static int db_puts(DBuf *d, const char *s) {
    size_t n = strlen(s);
    if (db_reserve(d, n)) return -1;
    memcpy(d->b + d->len, s, n);
    d->len += n;
    return 0;
}
static int db_putn(DBuf *d, const char *s, size_t n) {
    if (db_reserve(d, n)) return -1;
    memcpy(d->b + d->len, s, n);
    d->len += n;
    return 0;
}
static int db_printf(DBuf *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof tmp) {
        va_start(ap, fmt);
        char *big = malloc((size_t)n + 1);
        if (!big) return -1;
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        int r = db_puts(d, big);
        free(big);
        return r;
    }
    return db_puts(d, tmp);
}

/* ------------------------------------------------------------------ */
/* shell state                                                         */
/* ------------------------------------------------------------------ */
typedef struct Var {
    char *name, *value;
    int exported;
    struct Var *next;
} Var;

typedef struct Alias {
    char *name, *value;
    struct Alias *next;
} Alias;

static Var   *vars;
static Alias *aliases;
static int    last_status = 0;
static int    interactive = 0;
static int    login_shell = 0;
static int    exit_requested = 0;
static int    exit_code = 0;
static int    jobctl = 0;              /* job control active */
static pid_t  shell_pgrp = 0;
static int    ttyfd = -1;
static char  *ps1_default = "\\u@\\h:\\w\\$ ";
static volatile sig_atomic_t sigint_flag = 0;
static volatile sig_atomic_t jobs_changed = 0;
static pid_t  shell_pid;
static const char *pos_params[10] = { NULL };
static int    pos_argc = 0;

/* ------------------------------------------------------------------ */
/* variables                                                           */
/* ------------------------------------------------------------------ */
static Var *find_var(const char *name) {
    for (Var *v = vars; v; v = v->next)
        if (!strcmp(v->name, name)) return v;
    return NULL;
}

static const char *getvar(const char *name) {
    Var *v = find_var(name);
    return v ? v->value : NULL;
}

static void setvar(const char *name, const char *value, int exported) {
    Var *v = find_var(name);
    if (!v) {
        v = calloc(1, sizeof *v);
        if (!v) return;
        v->name = strdup(name);
        v->next = vars;
        vars = v;
    } else {
        free(v->value);
    }
    v->value = strdup(value ? value : "");
    v->exported = exported;
}

static void unsetvar(const char *name) {
    Var **pp = &vars;
    while (*pp) {
        Var *v = *pp;
        if (!strcmp(v->name, name)) {
            *pp = v->next;
            free(v->name);
            free(v->value);
            free(v);
            return;
        }
        pp = &v->next;
    }
}

static int is_valid_name(const char *s) {
    if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (const char *p = s + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* aliases                                                             */
/* ------------------------------------------------------------------ */
static Alias *find_alias(const char *name) {
    for (Alias *a = aliases; a; a = a->next)
        if (!strcmp(a->name, name)) return a;
    return NULL;
}

static int add_alias(const char *name, const char *value) {
    if (!is_valid_name(name) || !value) return -1;
    Alias *a = find_alias(name);
    if (a) {
        free(a->value);
        a->value = strdup(value);
        return 0;
    }
    int cnt = 0;
    for (a = aliases; a; a = a->next) cnt++;
    if (cnt >= MAX_ALIASES) return -1;
    a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->name = strdup(name);
    a->value = strdup(value);
    a->next = aliases;
    aliases = a;
    return 0;
}

static void remove_alias(const char *name) {
    Alias **pp = &aliases;
    while (*pp) {
        Alias *a = *pp;
        if (!strcmp(a->name, name)) {
            *pp = a->next;
            free(a->name);
            free(a->value);
            free(a);
            return;
        }
        pp = &a->next;
    }
}

/* ------------------------------------------------------------------ */
/* history                                                             */
/* ------------------------------------------------------------------ */
static char *hist[MAX_HISTORY];
static int hist_n = 0, hist_pos = 0;
static int hist_saved[MAX_HISTORY];

static void hist_add(const char *line) {
    if (!line || !*line) return;
    if (strlen(line) > MAX_HIST_LEN) return;
    if (hist_n > 0 && !strcmp(hist[hist_n - 1], line)) return;
    if (hist_n >= MAX_HISTORY) {
        free(hist[0]);
        memmove(hist, hist + 1, (size_t)(MAX_HISTORY - 1) * sizeof(char *));
        memmove(hist_saved, hist_saved + 1, (size_t)(MAX_HISTORY - 1) * sizeof(int));
        hist_n--;
        hist_pos = hist_n;
    }
    hist[hist_n] = strdup(line);
    hist_saved[hist_n] = 0;
    hist_n++;
    hist_pos = hist_n;
}

static const char *hist_file_path(void) {
    static char path[PATH_MAX];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(path, sizeof path, "%s/%s", home, HISTORY_FILE);
    return path;
}

static void hist_load(void) {
    const char *p = hist_file_path();
    if (!p) return;
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[MAX_HIST_LEN + 2];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (hist_n >= MAX_HISTORY) {
            free(hist[0]);
            memmove(hist, hist + 1, (size_t)(MAX_HISTORY - 1) * sizeof(char *));
            memmove(hist_saved, hist_saved + 1, (size_t)(MAX_HISTORY - 1) * sizeof(int));
            hist_n--;
        }
        hist[hist_n] = strdup(line);
        hist_saved[hist_n] = 1;
        hist_n++;
    }
    fclose(f);
    hist_pos = hist_n;
}

static void hist_save(void) {
    const char *p = hist_file_path();
    if (!p) return;
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < hist_n; i++)
        fprintf(f, "%s\n", hist[i]);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* jobs                                                                */
/* ------------------------------------------------------------------ */
typedef enum { PS_RUN, PS_STOP, PS_EXIT, PS_SIG } PState;
typedef struct Proc {
    pid_t pid;
    PState st;
    int status;
} Proc;

typedef enum { JS_RUN, JS_STOP, JS_DONE } JobState;

struct Cmd;   /* fwd */
static void bsh_free_cmd_tree(struct Cmd *c);

typedef struct Job {
    int id;
    pid_t pgid;              /* 0 when no job control */
    Proc *procs;
    int nprocs;
    JobState state;
    char *cmd;
    struct Cmd *tree;        /* background statement's parse tree */
    int notified;
    struct Job *next;
} Job;

static Job *job_list = NULL;
static int  job_count = 0;
static int  job_next_id = 1;

static Proc *fg_procs = NULL;
static int   fg_nprocs = 0;

static struct Cmd *pending_bg_tree = NULL;

static void update_job_state(Job *j) {
    int done = 1, stopped = 0;
    for (int i = 0; i < j->nprocs; i++) {
        if (j->procs[i].st == PS_RUN) done = 0;
        if (j->procs[i].st == PS_STOP) stopped = 1;
    }
    if (stopped && j->state == JS_RUN) { j->state = JS_STOP; jobs_changed = 1; }
    if (done && j->state != JS_DONE) { j->state = JS_DONE; jobs_changed = 1; }
}

static void job_free(Job *j) {
    if (!j) return;
    if (j->tree) bsh_free_cmd_tree(j->tree);
    free(j->procs);
    free(j->cmd);
    free(j);
}

static void reap_children(void) {
    pid_t p;
    int st;
    while ((p = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
        int found = 0;
        if (fg_procs) {
            for (int i = 0; i < fg_nprocs; i++) {
                if (fg_procs[i].pid == p) {
                    if (WIFSTOPPED(st)) { fg_procs[i].st = PS_STOP; fg_procs[i].status = WSTOPSIG(st); }
                    else if (WIFSIGNALED(st)) { fg_procs[i].st = PS_SIG; fg_procs[i].status = WTERMSIG(st); }
                    else { fg_procs[i].st = PS_EXIT; fg_procs[i].status = WEXITSTATUS(st); }
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            for (Job *j = job_list; j; j = j->next) {
                for (int i = 0; i < j->nprocs; i++) {
                    if (j->procs[i].pid == p) {
                        if (WIFSTOPPED(st)) { j->procs[i].st = PS_STOP; j->procs[i].status = WSTOPSIG(st); }
                        else if (WIFSIGNALED(st)) { j->procs[i].st = PS_SIG; j->procs[i].status = WTERMSIG(st); }
                        else { j->procs[i].st = PS_EXIT; j->procs[i].status = WEXITSTATUS(st); }
                        break;
                    }
                }
                update_job_state(j);
            }
        }
    }
}

static void on_sigchld(int sig) { (void)sig; reap_children(); }

/* returns 0 = all done, 1 = some stopped */
static int wait_procs(Proc *procs, int n) {
    for (;;) {
        int all_done = 1, any_stopped = 0;
        for (int i = 0; i < n; i++) {
            if (procs[i].st != PS_RUN) {
                if (procs[i].st == PS_STOP) any_stopped = 1;
                continue;
            }
            int st;
            pid_t r = waitpid(procs[i].pid, &st, WNOHANG | WUNTRACED);
            if (r == procs[i].pid) {
                if (WIFSTOPPED(st)) { procs[i].st = PS_STOP; procs[i].status = WSTOPSIG(st); }
                else if (WIFSIGNALED(st)) { procs[i].st = PS_SIG; procs[i].status = WTERMSIG(st); }
                else { procs[i].st = PS_EXIT; procs[i].status = WEXITSTATUS(st); }
                continue;
            }
            if (r < 0 && errno == ECHILD && procs[i].st == PS_RUN) {
                procs[i].st = PS_EXIT;
                procs[i].status = 0;
                continue;
            }
            all_done = 0;
        }
        if (all_done) return any_stopped;
        if (any_stopped) return 1;
        usleep(1000);
    }
}

static int proc_status(Proc *procs, int n) {
    int status = 0;
    for (int i = 0; i < n; i++) {
        if (procs[i].st == PS_EXIT) status = procs[i].status;
        else if (procs[i].st == PS_SIG) status = 128 + procs[i].status;
        else if (procs[i].st == PS_STOP) status = 0;
    }
    return status;
}

static int wait_one(pid_t pid) {
    Proc p[1];
    p[0].pid = pid;
    p[0].st = PS_RUN;
    fg_procs = p;
    fg_nprocs = 1;
    wait_procs(p, 1);
    fg_procs = NULL;
    fg_nprocs = 0;
    return proc_status(p, 1);
}

static Job *job_add(pid_t pgid, Proc *procs, int n, const char *cmdline,
                    JobState state, struct Cmd *tree) {
    while (job_count >= MAX_JOBS) {
        Job *j = job_list, *prev = NULL;
        while (j && j->state != JS_DONE) { prev = j; j = j->next; }
        if (!j) j = job_list;
        if (prev) prev->next = j->next; else job_list = j->next;
        job_free(j);
        job_count--;
    }
    Job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->id = job_next_id++;
    if (job_next_id > 1000000) job_next_id = 1;
    j->pgid = pgid;
    j->nprocs = n;
    j->procs = malloc((size_t)n * sizeof(Proc));
    if (j->procs) memcpy(j->procs, procs, (size_t)n * sizeof(Proc));
    j->state = state;
    j->cmd = strdup(cmdline ? cmdline : "");
    j->tree = tree;
    j->next = job_list;
    job_list = j;
    job_count++;
    return j;
}

static Job *find_job_spec(const char *spec) {
    if (!spec) return job_list ? job_list : NULL;
    if (!strcmp(spec, "%+") || !strcmp(spec, "%%")) return job_list ? job_list : NULL;
    if (!strcmp(spec, "%-")) return job_list && job_list->next ? job_list->next : NULL;
    if (spec[0] == '%') spec++;
    if (isdigit((unsigned char)*spec)) {
        int id = atoi(spec);
        for (Job *j = job_list; j; j = j->next)
            if (j->id == id) return j;
        return NULL;
    }
    for (Job *j = job_list; j; j = j->next)
        if (j->cmd && !strncmp(j->cmd, spec, strlen(spec))) return j;
    return NULL;
}

static const char *job_state_str(JobState s) {
    switch (s) {
    case JS_RUN:  return "Running";
    case JS_STOP: return "Stopped";
    default:      return "Done";
    }
}

static void job_print(Job *j, int is_cur, int is_prev) {
    char mark = ' ';
    if (is_cur) mark = '+';
    else if (is_prev) mark = '-';
    printf("[%d]%c %-8s %s\n", j->id, mark, job_state_str(j->state),
           j->cmd ? j->cmd : "");
}

static void job_print_all(void) {
    if (!job_list) return;
    Job *cur = job_list;
    Job *prev = cur ? cur->next : NULL;
    for (Job *k = job_list; k; k = k->next)
        job_print(k, k == cur, k == prev);
}

/* print state changes; called before each prompt */
static void job_notify(void) {
    if (!jobs_changed) return;
    jobs_changed = 0;
    for (Job *j = job_list; j; j = j->next) {
        if ((j->state == JS_STOP || j->state == JS_DONE) && !j->notified) {
            j->notified = 1;
            fprintf(stderr, "[%d]+  %-8s\t%s\n", j->id,
                    job_state_str(j->state), j->cmd ? j->cmd : "");
        }
    }
}

/* free done jobs that are not current/previous */
static void job_trim(void) {
    if (job_count <= 2) return;
    Job **pp = &job_list;
    while (*pp) {
        Job *k = *pp;
        int is_head = (k == job_list) || (k == job_list->next);
        if (k->state == JS_DONE && !is_head) {
            *pp = k->next;
            job_free(k);
            job_count--;
            continue;
        }
        pp = &k->next;
    }
}

/* ------------------------------------------------------------------ */
/* terminal modes                                                      */
/* ------------------------------------------------------------------ */
static struct termios t_orig, t_raw;
static int have_orig = 0;

static void term_raw(void) {
    if (ttyfd < 0) return;
    if (!have_orig) {
        if (tcgetattr(ttyfd, &t_orig) == 0) have_orig = 1;
        else return;
    }
    t_raw = t_orig;
    t_raw.c_lflag &= ~(ICANON | ECHO);
    t_raw.c_lflag |= ISIG;
    t_raw.c_cc[VMIN] = 1;
    t_raw.c_cc[VTIME] = 0;
    tcsetattr(ttyfd, TCSANOW, &t_raw);
}

static void term_restore(void) {
    if (ttyfd < 0 || !have_orig) return;
    tcsetattr(ttyfd, TCSANOW, &t_orig);
}

/* ------------------------------------------------------------------ */
/* tokenizer                                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    OP_NONE = 0, OP_TERM = 1, OP_AND = 2, OP_OR = 3, OP_BG = 4,
    OP_PIPE = 5, OP_RIN = 6, OP_ROUT = 7, OP_RAPP = 8, OP_RDUP = 9,
    OP_RCLOSE = 10
} Oper;

/* a "part" is a piece of a word: literal text, or a variable name that
 * is expanded at execution time (so $? and assignments see the right
 * shell state) */
typedef struct WPart {
    int quoted;        /* part appeared inside quotes / escaped */
    int isvar;         /* if set, s is a variable name to expand later */
    char *s;
    size_t len, cap;   /* for literal accumulator parts */
    struct WPart *next;
} WPart;

typedef struct Tok {
    int type;          /* T_WORD, T_OP */
    Oper op;
    int fd;            /* for redirections */
    int glob;          /* word needs globbing */
    WPart *parts;      /* for T_WORD: list of parts */
    struct Tok *next;
} Tok;

#define T_WORD 0
#define T_OP   1

static Tok *tok_new(int type) {
    Tok *t = calloc(1, sizeof *t);
    if (t) t->type = type;
    return t;
}

static void part_free(WPart *p) {
    while (p) {
        WPart *nx = p->next;
        free(p->s);
        free(p);
        p = nx;
    }
}

static void tok_free(Tok *t) {
    while (t) {
        Tok *nx = t->next;
        part_free(t->parts);
        free(t);
        t = nx;
    }
}

/* returns 0 ok, 1 = unterminated quote (need more), -1 = error */
static int tokenize(const char *line, Tok **out, const char **errmsg) {
    *out = NULL;
    *errmsg = NULL;
    Tok *head = NULL, **tail = &head;
    WPart *parts = NULL, **ppart = &parts;
    WPart *lit = NULL;         /* current literal accumulator part */
    int q = 0;                 /* 0 none, 1 single quote, 2 double quote */
    int q_empty = 0;           /* a quote opened and nothing was added */
    int word_started = 0;
    int glob = 0;
    int word_count = 0;

#define LIT_NEW() do { \
        lit = calloc(1, sizeof *lit); \
        if (!lit) goto fail; \
        lit->cap = 16; \
        lit->s = malloc(lit->cap); \
        if (!lit->s) { free(lit); lit = NULL; goto fail; } \
        lit->s[0] = 0; \
    } while (0)

#define LIT_PUTC(ch) do { \
        if (!lit) LIT_NEW(); \
        if (lit->len + 2 > lit->cap) { \
            size_t nc = lit->cap * 2; \
            char *ns = realloc(lit->s, nc); \
            if (!ns) goto fail; \
            lit->s = ns; lit->cap = nc; \
        } \
        lit->s[lit->len++] = (ch); lit->s[lit->len] = 0; \
    } while (0)

#define LIT_PUTCQ(ch) do { LIT_PUTC(ch); if (lit) lit->quoted = 1; } while (0)

#define LIT_PUTS(str) do { \
        for (const char *sp = (str); *sp; sp++) LIT_PUTC(*sp); \
    } while (0)

#define FLUSH_LIT() do { \
        if (lit) { \
            *ppart = lit; \
            ppart = &lit->next; \
            lit = NULL; \
            word_started = 1; \
        } \
    } while (0)

#define ADD_VAR(name, quotedv) do { \
        FLUSH_LIT(); \
        { \
            WPart *np = calloc(1, sizeof *np); \
            if (!np) goto fail; \
            np->isvar = 1; \
            np->quoted = (quotedv); \
            np->s = strdup(name); \
            if (!np->s) { free(np); goto fail; } \
            *ppart = np; \
            ppart = &np->next; \
            word_started = 1; \
        } \
    } while (0)

#define ADD_EMPTY_Q() do { \
        FLUSH_LIT(); \
        { \
            WPart *np = calloc(1, sizeof *np); \
            if (!np) goto fail; \
            np->quoted = 1; \
            np->s = strdup(""); \
            if (!np->s) { free(np); goto fail; } \
            *ppart = np; \
            ppart = &np->next; \
            word_started = 1; \
        } \
    } while (0)

#define EMIT_OP(opv, fdv) do { \
        Tok *t = tok_new(T_OP); if (!t) goto fail; \
        t->op = (opv); t->fd = (fdv); \
        *tail = t; tail = &t->next; \
    } while (0)

#define FLUSH_WORD() do { \
        FLUSH_LIT(); \
        if (word_started) { \
            Tok *t = tok_new(T_WORD); \
            if (!t) goto fail; \
            t->parts = parts; \
            t->glob = glob; \
            *tail = t; tail = &t->next; \
            parts = NULL; ppart = &parts; \
            word_count++; \
            if (word_count > MAX_WORDS) { *errmsg = "too many words"; goto fail; } \
        } \
        glob = 0; word_started = 0; q_empty = 0; \
    } while (0)

#define SCAN_VAR(quotedv) do { \
        const char *name = NULL; \
        char nb[64]; \
        if (p[1] == '{') { \
            const char *e = strchr(p + 2, '}'); \
            if (!e) { *errmsg = "bad substitution"; goto fail; } \
            if ((size_t)(e - p - 2) < sizeof nb) { \
                memcpy(nb, p + 2, (size_t)(e - p - 2)); \
                nb[e - p - 2] = 0; \
                name = nb; \
            } \
            p = e; \
        } else if (p[1] == '?') { strcpy(nb, "?"); name = nb; p++; } \
        else if (p[1] == '$') { strcpy(nb, "$$"); name = nb; p++; } \
        else if (p[1] == '#') { strcpy(nb, "#"); name = nb; p++; } \
        else if (p[1] == '*') { strcpy(nb, "*"); name = nb; p++; } \
        else if (p[1] == '@') { strcpy(nb, "@"); name = nb; p++; } \
        else if (isdigit((unsigned char)p[1])) { nb[0] = p[1]; nb[1] = 0; name = nb; p++; } \
        else if (isalpha((unsigned char)p[1]) || p[1] == '_') { \
            const char *s = p + 1; \
            size_t i = 0; \
            while ((isalnum((unsigned char)*s) || *s == '_') && i < sizeof nb - 1) \
                nb[i++] = *s++; \
            nb[i] = 0; \
            name = nb; \
            p = s - 1; \
        } \
        if (name) { ADD_VAR(name, (quotedv)); } \
        else LIT_PUTC('$'); \
    } while (0)

    for (const char *p = line; *p; p++) {
        char c = *p;
        if (q == 1) {
            if (c == '\'') {
                q = 0;
                if (q_empty) ADD_EMPTY_Q();
            } else {
                LIT_PUTCQ(c);
                q_empty = 0;
            }
            continue;
        }
        if (q == 2) {
            if (c == '"') {
                q = 0;
                if (q_empty) ADD_EMPTY_Q();
            } else if (c == '\\') {
                char n = p[1];
                if (n == '$' || n == '`' || n == '"' || n == '\\') { LIT_PUTCQ(n); p++; }
                else if (n == '\n') p++;
                else LIT_PUTC(c);
                q_empty = 0;
            } else if (c == '$') {
                SCAN_VAR(1);
                q_empty = 0;
            } else {
                LIT_PUTCQ(c);
                q_empty = 0;
            }
            continue;
        }
        /* unquoted */
        switch (c) {
        case '\'':
            FLUSH_LIT();
            q = 1;
            q_empty = 1;
            break;
        case '"':
            FLUSH_LIT();
            q = 2;
            q_empty = 1;
            break;
        case '\\': {
            char n = p[1];
            if (n == '\n' || n == '\r') { p++; break; }
            LIT_PUTCQ(n ? n : '\\');
            if (n) p++;
            break;
        }
        case '#':
            if (!word_started) { p += strlen(p); break; }
            LIT_PUTC(c);
            break;
        case ' ': case '\t': case '\r':
            FLUSH_WORD();
            break;
        case '\n':
            FLUSH_WORD();
            EMIT_OP(OP_TERM, 0);
            break;
        case ';':
            FLUSH_WORD();
            EMIT_OP(OP_TERM, 0);
            break;
        case '&':
            FLUSH_WORD();
            if (p[1] == '&') { EMIT_OP(OP_AND, 0); p++; }
            else if (p[1] == '>') { EMIT_OP(OP_ROUT, 1); EMIT_OP(OP_ROUT, 2); p++; }
            else EMIT_OP(OP_BG, 0);
            break;
        case '|':
            FLUSH_WORD();
            if (p[1] == '|') { EMIT_OP(OP_OR, 0); p++; }
            else EMIT_OP(OP_PIPE, 0);
            break;
        case '>': {
            FLUSH_WORD();
            if (p[1] == '>') { EMIT_OP(OP_RAPP, 1); p++; }
            else if (p[1] == '&') {
                if (p[2] == '-') { EMIT_OP(OP_RCLOSE, 1); p += 2; }
                else if (isdigit((unsigned char)p[2])) { EMIT_OP(OP_RDUP, 1); p += 1; }
                else { EMIT_OP(OP_ROUT, 1); EMIT_OP(OP_ROUT, 2); p++; }
            }
            else EMIT_OP(OP_ROUT, 1);
            break;
        }
        case '<': {
            FLUSH_WORD();
            if (p[1] == '<') { *errmsg = "heredoc not supported"; goto fail; }
            else if (p[1] == '&') {
                if (p[2] == '-') { EMIT_OP(OP_RCLOSE, 0); p += 2; }
                else if (isdigit((unsigned char)p[2])) { EMIT_OP(OP_RDUP, 0); p += 1; }
                else { EMIT_OP(OP_RIN, 0); p++; }
            }
            else EMIT_OP(OP_RIN, 0);
            break;
        }
        case '*': case '?': case '[':
            glob = 1;
            LIT_PUTC(c);
            break;
        default:
            if (isdigit((unsigned char)c) && !word_started) {
                const char *d = p;
                int fd = 0;
                while (isdigit((unsigned char)*d)) {
                    if (fd > 1000000) { fd = -1; break; }
                    fd = fd * 10 + (*d - '0');
                    d++;
                }
                if (fd >= 0 && (*d == '>' || *d == '<') && d != p) {
                    if (*d == '>') {
                        if (d[1] == '>') { EMIT_OP(OP_RAPP, fd); p = d + 1; }
                        else if (d[1] == '&') {
                            if (d[2] == '-') { EMIT_OP(OP_RCLOSE, fd); p = d + 2; }
                            else if (isdigit((unsigned char)d[2])) { EMIT_OP(OP_RDUP, fd); p = d + 1; }
                            else { EMIT_OP(OP_ROUT, fd); p = d + 1; }
                        }
                        else { EMIT_OP(OP_ROUT, fd); p = d; }
                    } else {
                        if (d[1] == '<') { *errmsg = "heredoc not supported"; goto fail; }
                        else if (d[1] == '&') {
                            if (d[2] == '-') { EMIT_OP(OP_RCLOSE, fd); p = d + 2; }
                            else if (isdigit((unsigned char)d[2])) { EMIT_OP(OP_RDUP, fd); p = d + 1; }
                            else { EMIT_OP(OP_RIN, fd); p = d + 1; }
                        }
                        else { EMIT_OP(OP_RIN, fd); p = d; }
                    }
                    break;
                }
            }
            if (c == '$') {
                SCAN_VAR(0);
                break;
            }
            if (c == '~' && !word_started) {
                const char *e = p + 1;
                while (*e && *e != '/' && *e != ' ' && *e != '\t' &&
                       *e != '\n' && *e != ':' && *e != '\'' && *e != '"') e++;
                if (e == p + 1) {
                    const char *home = getvar("HOME");
                    if (home) { LIT_PUTS(home); if (lit) lit->quoted = 1; }
                    else LIT_PUTC('~');
                } else {
                    char uname[128];
                    if ((size_t)(e - p - 1) < sizeof uname) {
                        memcpy(uname, p + 1, (size_t)(e - p - 1));
                        uname[e - p - 1] = 0;
                        struct passwd *pw = getpwnam(uname);
                        if (pw && pw->pw_dir) {
                            LIT_PUTS(pw->pw_dir);
                            if (lit) lit->quoted = 1;
                        } else {
                            LIT_PUTC('~');
                            LIT_PUTS(uname);
                        }
                    } else {
                        LIT_PUTC('~');
                    }
                }
                p = e - 1;
                break;
            }
            LIT_PUTC(c);
            break;
        }
    }
    if (q != 0) {
        tok_free(head);
        return 1;
    }
    FLUSH_WORD();
    *out = head;
    return 0;

fail:
    tok_free(head);
    if (!*errmsg) *errmsg = "bad token";
    return -1;
}

/* ------------------------------------------------------------------ */
/* globbing                                                            */
/* ------------------------------------------------------------------ */
static int wildmatch(const char *pat, const char *str) {
    for (;;) {
        if (!*pat) return !*str;
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (const char *s = str; ; s++) {
                if (wildmatch(pat, s)) return 1;
                if (!*s) return 0;
            }
        }
        if (!*str) return 0;
        if (*pat == '?') { pat++; str++; continue; }
        if (*pat == '[') {
            const char *p = pat + 1;
            int neg = 0;
            if (*p == '!' || *p == '^') { neg = 1; p++; }
            int hit = 0;
            if (*p == ']') { if (*str == ']') hit = 1; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (*str >= p[0] && *str <= p[2]) hit = 1;
                    p += 3;
                } else {
                    if (*str == p[0]) hit = 1;
                    p++;
                }
            }
            if (neg) hit = !hit;
            if (!hit) return 0;
            pat = (*p == ']') ? p + 1 : p;
            str++;
            continue;
        }
        if (*pat != *str) return 0;
        pat++; str++;
    }
}

static int has_meta(const char *s) {
    return strpbrk(s, "*?[") != NULL;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char **match_entries(const char *dir, const char *pat, int *n);

static char **match_entries(const char *dir, const char *pat, int *n) {
    *n = 0;
    char **res = NULL;
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return NULL;
    int cap = 0, cnt = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (wildmatch(pat, de->d_name)) {
            if (cnt >= cap) {
                cap = cap ? cap * 2 : 16;
                char **nr = realloc(res, (size_t)cap * sizeof(char *));
                if (!nr) break;
                res = nr;
            }
            size_t plen = strlen(dir ? dir : "");
            int isdir = de->d_type == DT_DIR;
            char *full = malloc(plen + strlen(de->d_name) + 4);
            if (!full) break;
            if (plen && strcmp(dir, ".")) {
                memcpy(full, dir, plen);
                if (full[plen - 1] != '/') full[plen++] = '/';
            }
            strcpy(full + plen, de->d_name);
            plen += strlen(de->d_name);
            if (isdir) full[plen++] = '/';
            full[plen] = 0;
            res[cnt++] = full;
        }
    }
    closedir(d);
    if (cnt > 1) qsort(res, (size_t)cnt, sizeof(char *), cmp_str);
    *n = cnt;
    return res;
}

/* returns malloc'd list of matches (sorted); if no matches or no meta,
 * returns the pattern itself as a single literal */
static char **glob_expand(const char *pat, int *n) {
    *n = 0;
    int had_meta = has_meta(pat);
    if (!had_meta) {
        char **r = malloc(sizeof(char *));
        if (!r) return NULL;
        r[0] = strdup(pat);
        *n = 1;
        return r;
    }
    char *dup = strdup(pat);
    if (!dup) return NULL;
    int abs = (pat[0] == '/');
    char **paths = NULL;
    int npaths = 0;
    char *save = NULL;
    char *comp = strtok_r(dup, "/", &save);
    if (!comp) {
        char **r = malloc(sizeof(char *));
        if (r) { r[0] = strdup(pat); *n = 1; }
        free(dup);
        return r;
    }
    while (comp) {
        if (has_meta(comp)) {
            if (npaths == 0) {
                paths = malloc(sizeof(char *));
                if (paths) paths[0] = strdup(abs ? "/" : "");
                npaths = 1;
            }
            char **nr = NULL;
            int nn = 0, ncap = 0;
            for (int i = 0; i < npaths; i++) {
                int m = 0;
                char **ms = match_entries(paths[i], comp, &m);
                for (int k = 0; k < m; k++) {
                    if (nn >= ncap) {
                        ncap = ncap ? ncap * 2 : 16;
                        char **t = realloc(nr, (size_t)ncap * sizeof(char *));
                        if (!t) { free(ms); goto done; }
                        nr = t;
                    }
                    nr[nn++] = ms[k];
                }
                free(ms);
            }
            for (int i = 0; i < npaths; i++) free(paths[i]);
            free(paths);
            paths = nr;
            npaths = nn;
            if (nn == 0) goto done;
        } else {
            if (npaths == 0) {
                char **nr = malloc(sizeof(char *));
                if (!nr) goto done;
                size_t cl = strlen(comp);
                size_t al = abs ? 1 : 0;
                nr[0] = malloc(cl + al + 1);
                if (!nr[0]) { free(nr); goto done; }
                if (abs) nr[0][0] = '/';
                memcpy(nr[0] + al, comp, cl);
                nr[0][al + cl] = 0;
                paths = nr;
                npaths = 1;
            } else {
                char **nr = malloc((size_t)npaths * sizeof(char *));
                if (!nr) goto done;
                for (int i = 0; i < npaths; i++) {
                    size_t pl = strlen(paths[i]);
                    size_t cl = strlen(comp);
                    int need_slash = pl && paths[i][pl - 1] != '/';
                    nr[i] = malloc(pl + cl + 2);
                    if (!nr[i]) { for (int k = 0; k < i; k++) free(nr[k]); free(nr); goto done; }
                    memcpy(nr[i], paths[i], pl);
                    if (need_slash) nr[i][pl++] = '/';
                    memcpy(nr[i] + pl, comp, cl);
                    nr[i][pl + cl] = 0;
                }
                for (int i = 0; i < npaths; i++) free(paths[i]);
                free(paths);
                paths = nr;
            }
        }
        comp = strtok_r(NULL, "/", &save);
    }
done:
    free(dup);
    if (npaths == 0) {
        char **r = malloc(sizeof(char *));
        if (r) { r[0] = strdup(pat); *n = 1; }
        return r;
    }
    *n = npaths;
    return paths;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */
typedef struct Word {
    WPart *parts;
    int glob;
    struct Word *next;
} Word;

typedef struct Redir {
    int fd;
    Oper op;
    WPart *file_parts;   /* for RIN/ROUT/RAPP; expanded at exec time */
    char *file;          /* expanded value */
    int dupfd;           /* for RDUP */
    struct Redir *next;
} Redir;

typedef struct Stage {
    Word *words;
    Redir *redirs;
    struct Stage *next;
} Stage;

typedef struct Assign {
    char *name;
    WPart *value;        /* expanded at exec time */
} Assign;

typedef struct Cmd {
    Stage *stages;
    Stage *last_stage;
    Assign *assigns;
    int nassign;
    Oper op;          /* link to next: OP_AND/OP_OR, or OP_TERM/OP_BG */
    struct Cmd *next;
} Cmd;

/* take a word token's parts (ownership moves from the token) */
static Word *word_add(Stage *st, Tok *t) {
    Word *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->parts = t->parts;
    w->glob = t->glob;
    t->parts = NULL;
    Word **pp = &st->words;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
    return w;
}

/* check whether a word token is an assignment (e.g. X=1, X=$Y).
 * on success: returns 1, *name_out = var name (malloc'd),
 * *value_out = remaining parts list (ownership). */
static int is_assignment_parts(WPart *parts, char **name_out, WPart **value_out) {
    if (!parts || parts->isvar || parts->quoted) return 0;
    char *eq = strchr(parts->s, '=');
    if (!eq || eq == parts->s) return 0;
    size_t nl = (size_t)(eq - parts->s);
    if (nl >= 128) return 0;
    char name[128];
    memcpy(name, parts->s, nl);
    name[nl] = 0;
    if (!is_valid_name(name)) return 0;
    WPart *val = NULL, **vp = &val;
    if (eq[1]) {
        WPart *np = calloc(1, sizeof *np);
        if (!np) return 0;
        np->s = strdup(eq + 1);
        np->quoted = parts->quoted;
        np->len = strlen(eq + 1);
        *vp = np;
        vp = &np->next;
    }
    *vp = parts->next;   /* move the rest of the list */
    parts->next = NULL;
    free(parts->s);
    free(parts);
    *name_out = strdup(name);
    *value_out = val;
    return 1;
}

static void free_words(Word *w) {
    while (w) { Word *nx = w->next; part_free(w->parts); free(w); w = nx; }
}

static void free_redirs(Redir *r) {
    while (r) { Redir *nx = r->next; part_free(r->file_parts); free(r->file); free(r); r = nx; }
}

static void free_stages(Stage *s) {
    while (s) { Stage *nx = s->next; free_words(s->words); free_redirs(s->redirs); free(s); s = nx; }
}

static void bsh_free_cmd_tree(Cmd *c) {
    while (c) {
        Cmd *nx = c->next;
        free_stages(c->stages);
        if (c->assigns) {
            for (int i = 0; i < c->nassign; i++) {
                free(c->assigns[i].name);
                part_free(c->assigns[i].value);
            }
            free(c->assigns);
        }
        free(c);
        c = nx;
    }
}

/* returns 0 ok, -1 syntax error */
static int parse(Tok *toks, Cmd **out, const char **errmsg) {
    *out = NULL;
    *errmsg = NULL;
    Cmd *head = NULL, **ctail = &head;
    Cmd *cur = NULL;
    Stage *stage = NULL;
    int assign_possible = 1;

#define NEW_CMD() do { \
        cur = calloc(1, sizeof *cur); \
        if (!cur) goto oom; \
        cur->op = OP_TERM; \
        *ctail = cur; ctail = &cur->next; \
        stage = NULL; \
        assign_possible = 1; \
    } while (0)

#define NEW_STAGE() do { \
        stage = calloc(1, sizeof *stage); \
        if (!stage) goto oom; \
        if (!cur->stages) cur->stages = stage; \
        else cur->last_stage->next = stage; \
        cur->last_stage = stage; \
        assign_possible = (cur->stages == stage); \
    } while (0)

    for (Tok *t = toks; t; t = t->next) {
        if (t->type == T_OP) {
            switch (t->op) {
            case OP_PIPE:
                if (!cur || !cur->stages) { *errmsg = "syntax error near |"; goto fail; }
                NEW_STAGE();
                break;
            case OP_RIN: case OP_ROUT: case OP_RAPP: case OP_RDUP: case OP_RCLOSE: {
                if (!cur) NEW_CMD();
                if (!stage) NEW_STAGE();
                Redir *r = calloc(1, sizeof *r);
                if (!r) goto oom;
                r->fd = t->fd;
                r->op = t->op;
                if (t->op == OP_RDUP) {
                    if (!t->next || t->next->type != T_WORD) {
                        free(r);
                        *errmsg = "missing redirection target";
                        goto fail;
                    }
                    t = t->next;
                    r->dupfd = atoi(t->parts ? t->parts->s : "");
                    part_free(t->parts);
                    t->parts = NULL;
                } else if (t->op == OP_RCLOSE) {
                    /* the '-' operand was consumed by the tokenizer */
                } else {
                    if (!t->next || t->next->type != T_WORD) {
                        free(r);
                        *errmsg = "missing redirection target";
                        goto fail;
                    }
                    t = t->next;
                    r->file_parts = t->parts;
                    t->parts = NULL;
                }
                /* append in order so redirections apply left to right */
                if (!stage->redirs) stage->redirs = r;
                else {
                    Redir *last = stage->redirs;
                    while (last->next) last = last->next;
                    last->next = r;
                }
                break;
            }
            case OP_TERM: case OP_BG:
                if (!cur || (!cur->stages && !cur->nassign)) break; /* leading ';' etc */
                cur->op = t->op;
                NEW_CMD();
                break;
            case OP_AND: case OP_OR:
                if (!cur || (!cur->stages && !cur->nassign)) {
                    *errmsg = "syntax error near && or ||";
                    goto fail;
                }
                cur->op = t->op;
                NEW_CMD();
                break;
            default:
                break;
            }
            continue;
        }
        /* word token */
        if (!cur) NEW_CMD();
        if (assign_possible && (!stage || !stage->words)) {
            char *name = NULL;
            WPart *val = NULL;
            if (is_assignment_parts(t->parts, &name, &val)) {
                Assign *na = realloc(cur->assigns,
                                     (size_t)(cur->nassign + 1) * sizeof(Assign));
                if (!na) { free(name); part_free(val); goto oom; }
                cur->assigns = na;
                cur->assigns[cur->nassign].name = name;
                cur->assigns[cur->nassign].value = val;
                cur->nassign++;
                t->parts = NULL;   /* parts consumed by is_assignment_parts */
                continue;
            }
        }
        if (!stage) NEW_STAGE();
        if (!word_add(stage, t)) goto oom;
        assign_possible = 0;
    }
    /* validate: no empty stages (dangling pipes) */
    for (Cmd *c = head; c; c = c->next) {
        for (Stage *s = c->stages; s; s = s->next) {
            if (!s->words && !s->redirs) {
                *errmsg = "syntax error near |";
                goto fail;
            }
        }
    }
    /* drop trailing empty commands (e.g. after ';') */
    while (head && !head->stages && !head->nassign) {
        Cmd *nx = head->next;
        free(head);
        head = nx;
    }
    *out = head;
    return 0;

oom:
    *errmsg = "out of memory";
fail:
    bsh_free_cmd_tree(head);
    return -1;
}

/* ------------------------------------------------------------------ */
/* expansion (execution-time)                                          */
/* ------------------------------------------------------------------ */
static const char *resolve_var(const char *name, char *tmp, size_t tsz) {
    if (!strcmp(name, "?")) { snprintf(tmp, tsz, "%d", last_status); return tmp; }
    if (!strcmp(name, "$$")) { snprintf(tmp, tsz, "%d", (int)shell_pid); return tmp; }
    if (!strcmp(name, "#")) { snprintf(tmp, tsz, "%d", pos_argc); return tmp; }
    if (!strcmp(name, "*") || !strcmp(name, "@")) {
        DBuf d;
        db_init(&d);
        for (int i = 1; i <= pos_argc && i < 10; i++) {
            if (!pos_params[i]) continue;
            if (d.len) db_putc(&d, ' ');
            db_puts(&d, pos_params[i]);
        }
        if (d.len >= tsz) d.len = tsz - 1;
        if (d.b) { memcpy(tmp, d.b, d.len); tmp[d.len] = 0; }
        else tmp[0] = 0;
        db_free(&d);
        return tmp;
    }
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) {
        int idx = name[0] - '0';
        return (idx <= pos_argc && pos_params[idx]) ? pos_params[idx] : "";
    }
    return getvar(name);
}

typedef struct StrList {
    char **v;
    int n, cap;
} StrList;

static void sl_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static int sl_addn(StrList *l, const char *s, size_t n) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        char **nv = realloc(l->v, (size_t)nc * sizeof(char *));
        if (!nv) return -1;
        l->v = nv;
        l->cap = nc;
    }
    char *dup = malloc(n + 1);
    if (!dup) return -1;
    memcpy(dup, s, n);
    dup[n] = 0;
    l->v[l->n++] = dup;
    return 0;
}

static int sl_add(StrList *l, const char *s) {
    return sl_addn(l, s, strlen(s));
}

/* expand a word's parts into zero or more strings.
 * split: unquoted variable values are split on IFS whitespace.
 * do_glob: glob-expand each produced string (falling back to literal). */
static int expand_parts(WPart *parts, int split, int do_glob, StrList *out) {
    DBuf cur;
    db_init(&cur);
    int have_any = 0;
    int rc = 0;
    for (WPart *p = parts; p; p = p->next) {
        if (!p->isvar) {
            if (p->quoted) have_any = 1;
            if (db_puts(&cur, p->s)) { rc = -1; break; }
            continue;
        }
        char tmp[512];
        const char *val = resolve_var(p->s, tmp, sizeof tmp);
        if (p->quoted) {
            have_any = 1;
            if (db_puts(&cur, val ? val : "")) { rc = -1; break; }
            continue;
        }
        /* unquoted variable */
        if (!val || !*val) continue;
        if (!split) {
            if (db_puts(&cur, val)) { rc = -1; break; }
            continue;
        }
        const char *s = val;
        int first_piece = 1;
        while (*s) {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (!*s) break;
            const char *e = s;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
            if (!first_piece) {
                if (cur.len || have_any) {
                    if (sl_addn(out, cur.b ? cur.b : "", cur.len)) { rc = -1; goto out; }
                    have_any = 0;
                }
                db_free(&cur);
                db_init(&cur);
            }
            if (db_putn(&cur, s, (size_t)(e - s))) { rc = -1; goto out; }
            first_piece = 0;
            s = e;
        }
    }
out:
    if (!rc && (cur.len || have_any)) {
        if (sl_addn(out, cur.b ? cur.b : "", cur.len)) rc = -1;
    }
    db_free(&cur);
    if (rc) return -1;
    if (do_glob) {
        StrList g = { 0 };
        for (int i = 0; i < out->n; i++) {
            int gn = 0;
            char **ms = glob_expand(out->v[i], &gn);
            if (!ms) { sl_free(&g); return -1; }
            for (int k = 0; k < gn; k++) {
                if (sl_add(&g, ms[k])) {
                    for (int k2 = 0; k2 < gn; k2++) free(ms[k2]);
                    free(ms);
                    sl_free(&g);
                    return -1;
                }
            }
            for (int k = 0; k < gn; k++) free(ms[k]);
            free(ms);
        }
        sl_free(out);
        *out = g;
    }
    return 0;
}

/* join all parts into a single string (no splitting; for assignments,
 * redirection filenames, fd operands). Returns malloc'd string. */
static char *parts_join(WPart *parts) {
    DBuf d;
    db_init(&d);
    char tmp[512];
    for (WPart *p = parts; p; p = p->next) {
        if (!p->isvar) { db_puts(&d, p->s); continue; }
        const char *val = resolve_var(p->s, tmp, sizeof tmp);
        if (val) db_puts(&d, val);
    }
    if (!d.b) return strdup("");
    if (db_reserve(&d, 1)) { db_free(&d); return strdup(""); }
    d.b[d.len] = 0;
    return d.b;   /* transfer ownership */
}

/* expand all words of a stage into a NULL-terminated argv */
static char **words_to_argv(Word *words, int *argc) {
    StrList l = { 0 };
    for (Word *w = words; w; w = w->next) {
        if (expand_parts(w->parts, 1, w->glob, &l)) { sl_free(&l); return NULL; }
    }
    char **argv = realloc(l.v, (size_t)(l.n + 1) * sizeof(char *));
    if (!argv) { sl_free(&l); return NULL; }
    argv[l.n] = NULL;
    if (argc) *argc = l.n;
    return argv;
}

/* ------------------------------------------------------------------ */
/* environment snapshot for exec                                       */
/* ------------------------------------------------------------------ */
static char **build_envp(Assign *assigns, int nassign) {
    size_t n = 0, cap = 64;
    char **envp = malloc(cap * sizeof(char *));
    if (!envp) return NULL;
    for (Var *v = vars; v; v = v->next) {
        if (!v->exported) continue;
        if (n + 1 >= cap) { cap *= 2; envp = realloc(envp, cap * sizeof(char *)); }
        char *e = malloc(strlen(v->name) + strlen(v->value) + 2);
        if (!e) continue;
        sprintf(e, "%s=%s", v->name, v->value);
        envp[n++] = e;
    }
    if (assigns) {
        for (int i = 0; i < nassign; i++) {
            char *val = parts_join(assigns[i].value);
            size_t nl = strlen(assigns[i].name);
            size_t vl = strlen(val);
            int found = 0;
            for (size_t j = 0; j < n; j++) {
                if (!strncmp(envp[j], assigns[i].name, nl) && envp[j][nl] == '=') {
                    char *rep = malloc(nl + vl + 2);
                    if (rep) {
                        memcpy(rep, assigns[i].name, nl);
                        rep[nl] = '=';
                        memcpy(rep + nl + 1, val, vl);
                        rep[nl + vl + 1] = 0;
                        free(envp[j]);
                        envp[j] = rep;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (n + 1 >= cap) { cap *= 2; envp = realloc(envp, cap * sizeof(char *)); }
                char *rep = malloc(nl + vl + 2);
                if (rep) {
                    memcpy(rep, assigns[i].name, nl);
                    rep[nl] = '=';
                    memcpy(rep + nl + 1, val, vl);
                    rep[nl + vl + 1] = 0;
                    envp[n++] = rep;
                }
            }
            free(val);
        }
    }
    envp[n] = NULL;
    return envp;
}

/* ------------------------------------------------------------------ */
/* redirection application                                             */
/* ------------------------------------------------------------------ */
static int apply_redirs(Redir *r) {
    for (; r; r = r->next) {
        int fd = -1;
        if (r->op == OP_RIN) {
            fd = open(r->file, O_RDONLY);
            if (fd < 0) { fprintf(stderr, "bsh: %s: %s\n", r->file, strerror(errno)); return -1; }
            if (dup2(fd, r->fd) < 0) { close(fd); return -1; }
            if (fd != r->fd) close(fd);
        } else if (r->op == OP_ROUT) {
            fd = open(r->file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) { fprintf(stderr, "bsh: %s: %s\n", r->file, strerror(errno)); return -1; }
            if (dup2(fd, r->fd) < 0) { close(fd); return -1; }
            if (fd != r->fd) close(fd);
        } else if (r->op == OP_RAPP) {
            fd = open(r->file, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (fd < 0) { fprintf(stderr, "bsh: %s: %s\n", r->file, strerror(errno)); return -1; }
            if (dup2(fd, r->fd) < 0) { close(fd); return -1; }
            if (fd != r->fd) close(fd);
        } else if (r->op == OP_RDUP) {
            if (r->dupfd < 0 || r->dupfd > 2) {
                fprintf(stderr, "bsh: %d: bad file descriptor\n", r->dupfd);
                return -1;
            }
            if (dup2(r->dupfd, r->fd) < 0) {
                fprintf(stderr, "bsh: %d: %s\n", r->dupfd, strerror(errno));
                return -1;
            }
        } else if (r->op == OP_RCLOSE) {
            close(r->fd);
        }
    }
    return 0;
}

/* expand redirection filenames at execution time (variables may have
 * changed between parse and exec) */
static int expand_redir_files(Redir *r) {
    for (; r; r = r->next) {
        if (r->op == OP_RIN || r->op == OP_ROUT || r->op == OP_RAPP) {
            char *s = parts_join(r->file_parts);
            free(r->file);
            r->file = s;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */
typedef int (*BiFn)(int argc, char **argv);

static int bi_cd(int argc, char **argv);
static int bi_pwd(int argc, char **argv);
static int bi_echo(int argc, char **argv);
static int bi_printf(int argc, char **argv);
static int bi_export(int argc, char **argv);
static int bi_unset(int argc, char **argv);
static int bi_env(int argc, char **argv);
static int bi_exit(int argc, char **argv);
static int bi_true(int argc, char **argv);
static int bi_false(int argc, char **argv);
static int bi_history(int argc, char **argv);
static int bi_alias(int argc, char **argv);
static int bi_unalias(int argc, char **argv);
static int bi_jobs(int argc, char **argv);
static int bi_fg(int argc, char **argv);
static int bi_bg(int argc, char **argv);
static int bi_help(int argc, char **argv);

static const struct {
    const char *name;
    BiFn fn;
} builtin_table[] = {
    { "cd", bi_cd }, { "pwd", bi_pwd }, { "echo", bi_echo },
    { "printf", bi_printf }, { "export", bi_export }, { "unset", bi_unset },
    { "env", bi_env }, { "exit", bi_exit }, { "true", bi_true },
    { "false", bi_false }, { "history", bi_history }, { "alias", bi_alias },
    { "unalias", bi_unalias }, { "jobs", bi_jobs }, { "fg", bi_fg },
    { "bg", bi_bg }, { "help", bi_help },
    { NULL, NULL }
};

static BiFn find_builtin(const char *name) {
    for (int i = 0; builtin_table[i].name; i++)
        if (!strcmp(builtin_table[i].name, name)) return builtin_table[i].fn;
    return NULL;
}

static void child_reset_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

static int bi_cd(int argc, char **argv) {
    static char dirbuf[PATH_MAX];
    const char *dir = NULL;
    const char *pwd = getvar("PWD");
    if (argc > 2) { fprintf(stderr, "cd: too many arguments\n"); return 1; }
    if (argc == 1) {
        dir = getenv("HOME");
        if (!dir) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
    } else {
        if (!strcmp(argv[1], "-")) {
            dir = getvar("OLDPWD");
            if (!dir) { fprintf(stderr, "cd: OLDPWD not set\n"); return 1; }
            printf("%s\n", dir);
            snprintf(dirbuf, sizeof dirbuf, "%s", dir);
            dir = dirbuf;
        } else if (argv[1][0] == '~') {
            if (argv[1][1] == '\0' || argv[1][1] == '/') {
                const char *home = getenv("HOME");
                if (!home) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
                if (argv[1][1] == '/')
                    snprintf(dirbuf, sizeof dirbuf, "%s%s", home, argv[1] + 1);
                else
                    snprintf(dirbuf, sizeof dirbuf, "%s", home);
                dir = dirbuf;
            } else {
                char uname[128];
                const char *sl = strchr(argv[1] + 1, '/');
                size_t nl = sl ? (size_t)(sl - argv[1] - 1) : strlen(argv[1] + 1);
                if (nl >= sizeof uname) { fprintf(stderr, "cd: bad user\n"); return 1; }
                memcpy(uname, argv[1] + 1, nl);
                uname[nl] = 0;
                struct passwd *pw = getpwnam(uname);
                if (!pw || !pw->pw_dir) { fprintf(stderr, "cd: %s: no such user\n", uname); return 1; }
                if (sl)
                    snprintf(dirbuf, sizeof dirbuf, "%s%s", pw->pw_dir, sl);
                else
                    snprintf(dirbuf, sizeof dirbuf, "%s", pw->pw_dir);
                dir = dirbuf;
            }
        } else {
            dir = argv[1];
        }
    }
    if (chdir(dir) < 0) {
        fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) {
        if (pwd) setvar("OLDPWD", pwd, 1);
        setvar("PWD", cwd, 1);
    }
    return 0;
}

static int bi_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) {
        printf("%s\n", cwd);
        return 0;
    }
    const char *pwd = getvar("PWD");
    if (pwd) { printf("%s\n", pwd); return 0; }
    perror("pwd");
    return 1;
}

static int bi_echo(int argc, char **argv) {
    int nflag = 0, eflag = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) nflag = 1;
        else if (!strcmp(argv[i], "-e")) eflag = 1;
        else if (!strcmp(argv[i], "-E")) eflag = 0;
        else if (!strcmp(argv[i], "-en") || !strcmp(argv[i], "-ne")) { eflag = 1; nflag = 1; }
        else break;
    }
    int wrote = 0;
    for (; i < argc; i++) {
        if (wrote) putchar(' ');
        wrote = 1;
        if (eflag) {
            for (const char *p = argv[i]; *p; p++) {
                if (*p == '\\' && p[1]) {
                    p++;
                    switch (*p) {
                    case 'n': putchar('\n'); break;
                    case 't': putchar('\t'); break;
                    case 'r': putchar('\r'); break;
                    case 'a': putchar('\a'); break;
                    case 'b': putchar('\b'); break;
                    case 'f': putchar('\f'); break;
                    case 'v': putchar('\v'); break;
                    case '\\': putchar('\\'); break;
                    case '0': {
                        int v = 0, k = 0;
                        while (p[1] && k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (p[1] - '0'); p++; k++; }
                        putchar((char)v);
                        break;
                    }
                    case 'c': nflag = 1; goto done_echo;
                    default: putchar('\\'); putchar(*p); break;
                    }
                } else {
                    putchar(*p);
                }
            }
        } else {
            fputs(argv[i], stdout);
        }
    }
done_echo:
    if (!nflag) putchar('\n');
    return 0;
}

static void print_escape(FILE *out, const char **pp) {
    const char *p = *pp;
    if (*p != '\\') { fputc(*p, out); (*pp)++; return; }
    p++;
    switch (*p) {
    case 'n': fputc('\n', out); (*pp) += 2; break;
    case 't': fputc('\t', out); (*pp) += 2; break;
    case 'r': fputc('\r', out); (*pp) += 2; break;
    case 'a': fputc('\a', out); (*pp) += 2; break;
    case 'b': fputc('\b', out); (*pp) += 2; break;
    case 'f': fputc('\f', out); (*pp) += 2; break;
    case 'v': fputc('\v', out); (*pp) += 2; break;
    case '\\': fputc('\\', out); (*pp) += 2; break;
    case '0': {
        p++;
        int v = 0, k = 0;
        while (p[1] && k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (p[1] - '0'); p++; k++; }
        fputc((char)v, out);
        *pp = p + 1;
        break;
    }
    case 'x': {
        p++;
        int v = 0, k = 0;
        while (p[1] && k < 2 && isxdigit((unsigned char)p[1])) {
            int d = p[1];
            if (d >= '0' && d <= '9') v = v * 16 + (d - '0');
            else if (d >= 'a' && d <= 'f') v = v * 16 + (d - 'a' + 10);
            else v = v * 16 + (d - 'A' + 10);
            p++; k++;
        }
        fputc((char)v, out);
        *pp = p + 1;
        break;
    }
    default:
        fputc('\\', out);
        (*pp)++;
        break;
    }
}

static int bi_printf(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "printf: usage: printf FORMAT [ARG...]\n");
        return 2;
    }
    const char *fmt = argv[1];
    int ai = 2;
    for (;;) {
        int consumed = 0;
        for (const char *p = fmt; *p; p++) {
            if (*p != '%') {
                if (*p == '\\') print_escape(stdout, &p);
                else fputc(*p, stdout);
                continue;
            }
            p++;
            while (*p == '-' || *p == '0' || *p == '#' || *p == ' ' || *p == '+') p++;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++; }
            if (*p == 'l' || *p == 'h') { p++; if (*p == 'l' || *p == 'h') p++; }
            char conv = *p;
            if (!conv) break;
            switch (conv) {
            case '%': fputc('%', stdout); break;
            case 's':
                if (ai < argc) { fputs(argv[ai], stdout); ai++; consumed = 1; }
                break;
            case 'd': case 'i':
                if (ai < argc) { printf("%d", atoi(argv[ai])); ai++; consumed = 1; }
                else printf("0");
                break;
            case 'u':
                if (ai < argc) { printf("%u", (unsigned)strtoul(argv[ai], NULL, 10)); ai++; consumed = 1; }
                else printf("0");
                break;
            case 'o':
                if (ai < argc) { printf("%lo", strtoul(argv[ai], NULL, 8)); ai++; consumed = 1; }
                else printf("0");
                break;
            case 'x': case 'X':
                if (ai < argc) {
                    if (conv == 'x') printf("%lx", strtoul(argv[ai], NULL, 16));
                    else printf("%lX", strtoul(argv[ai], NULL, 16));
                    ai++; consumed = 1;
                } else printf("0");
                break;
            case 'c':
                if (ai < argc) { fputc(argv[ai][0], stdout); ai++; consumed = 1; }
                break;
            case 'b':
                if (ai < argc) {
                    const char *sp = argv[ai];
                    while (*sp) {
                        if (*sp == '\\') print_escape(stdout, &sp);
                        else { fputc(*sp, stdout); sp++; }
                    }
                    ai++; consumed = 1;
                }
                break;
            default:
                fputc('%', stdout);
                fputc(conv, stdout);
                break;
            }
        }
        if (ai >= argc) break;
        if (!consumed) break;
    }
    return 0;
}

static int bi_export(int argc, char **argv) {
    int pflag = 0;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-p")) { pflag = 1; i++; }
    if (i >= argc) {
        for (Var *v = vars; v; v = v->next)
            if (v->exported) printf("export %s='%s'\n", v->name, v->value);
        return 0;
    }
    if (pflag) {
        for (; i < argc; i++) {
            const char *v = getvar(argv[i]);
            if (v) printf("export %s='%s'\n", argv[i], v);
        }
        return 0;
    }
    for (; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            char name[128];
            size_t nl = (size_t)(eq - argv[i]);
            if (nl >= sizeof name) { fprintf(stderr, "export: bad name\n"); continue; }
            memcpy(name, argv[i], nl);
            name[nl] = 0;
            if (!is_valid_name(name)) { fprintf(stderr, "export: %s: not a valid name\n", name); return 1; }
            setvar(name, eq + 1, 1);
        } else {
            if (!is_valid_name(argv[i])) { fprintf(stderr, "export: %s: not a valid name\n", argv[i]); return 1; }
            const char *v = getvar(argv[i]);
            setvar(argv[i], v ? v : "", 1);
        }
    }
    return 0;
}

static int bi_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-v") || !strcmp(argv[i], "-n")) continue;
        unsetvar(argv[i]);
    }
    return 0;
}

static int bi_env(int argc, char **argv) {
    if (argc == 1) {
        for (Var *v = vars; v; v = v->next)
            if (v->exported) printf("%s=%s\n", v->name, v->value);
        return 0;
    }
    char **envp = build_envp(NULL, 0);
    pid_t pid = fork();
    if (pid == 0) {
        child_reset_signals();
        execvpe(argv[1], argv + 1, envp);
        fprintf(stderr, "env: %s: %s\n", argv[1], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (pid > 0) status = wait_one(pid);
    for (int i = 0; envp && envp[i]; i++) free(envp[i]);
    free(envp);
    return status;
}

static int bi_exit(int argc, char **argv) {
    int code = last_status;
    if (argc > 1) {
        char *end;
        long v = strtol(argv[1], &end, 10);
        if (*end || v < 0 || v > 255) {
            fprintf(stderr, "exit: %s: numeric argument required\n", argv[1]);
            code = 2;
        } else {
            code = (int)v;
        }
    }
    exit_requested = 1;
    exit_code = code;
    return code;
}

static int bi_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int bi_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

static int bi_history(int argc, char **argv) {
    int n = 0;
    if (argc > 1 && argv[1][0] != '-') n = atoi(argv[1]);
    if (!n || n > hist_n) n = hist_n;
    for (int i = hist_n - n; i < hist_n; i++)
        printf("%5d  %s\n", i + 1, hist[i] ? hist[i] : "");
    return 0;
}

static int bi_alias(int argc, char **argv) {
    if (argc == 1) {
        for (Alias *a = aliases; a; a = a->next)
            printf("alias %s='%s'\n", a->name, a->value);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) continue;
        const char *eq = strchr(argv[i], '=');
        if (eq) {
            char name[128];
            size_t nl = (size_t)(eq - argv[i]);
            if (nl >= sizeof name) { fprintf(stderr, "alias: name too long\n"); return 1; }
            memcpy(name, argv[i], nl);
            name[nl] = 0;
            if (add_alias(name, eq + 1) < 0) {
                fprintf(stderr, "alias: %s: invalid or too many aliases\n", name);
                return 1;
            }
        } else {
            Alias *a = find_alias(argv[i]);
            if (!a) { fprintf(stderr, "alias: %s: not found\n", argv[i]); return 1; }
            printf("alias %s='%s'\n", a->name, a->value);
        }
    }
    return 0;
}

static int bi_unalias(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-a")) {
        Alias *a = aliases;
        while (a) { Alias *nx = a->next; free(a->name); free(a->value); free(a); a = nx; }
        aliases = NULL;
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        Alias *a = find_alias(argv[i]);
        if (!a) { fprintf(stderr, "unalias: %s: not found\n", argv[i]); rc = 1; continue; }
        remove_alias(argv[i]);
    }
    return rc;
}

static int bi_jobs(int argc, char **argv) {
    int lflag = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) lflag = 1;
        else if (argv[i][0] == '%' || isdigit((unsigned char)argv[i][0])) {
            Job *j = find_job_spec(argv[i]);
            if (!j) { fprintf(stderr, "jobs: %s: no such job\n", argv[i]); return 1; }
            if (lflag) printf("[%d] %6d %-8s %s\n", j->id, (int)j->pgid,
                              job_state_str(j->state), j->cmd ? j->cmd : "");
            else job_print(j, 0, 0);
            return 0;
        }
    }
    if (lflag) {
        for (Job *j = job_list; j; j = j->next)
            printf("[%d] %6d %-8s %s\n", j->id, (int)j->pgid,
                   job_state_str(j->state), j->cmd ? j->cmd : "");
    } else {
        job_print_all();
    }
    return 0;
}

static int bi_fg(int argc, char **argv) {
    const char *spec = (argc > 1) ? argv[1] : NULL;
    Job *j = find_job_spec(spec);
    if (!j) {
        fprintf(stderr, "fg: %s: no such job\n", spec ? spec : "%%");
        return 1;
    }
    if (j->state == JS_STOP) {
        fprintf(stderr, "%s\n", j->cmd ? j->cmd : "");
        kill(-j->pgid, SIGCONT);
        j->state = JS_RUN;
        for (int i = 0; i < j->nprocs; i++)
            if (j->procs[i].st == PS_STOP) j->procs[i].st = PS_RUN;
    }
    if (jobctl) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGTTOU);
        sigprocmask(SIG_BLOCK, &set, NULL);
        tcsetpgrp(ttyfd, j->pgid);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
    }
    fg_procs = j->procs;
    fg_nprocs = j->nprocs;
    int stopped = wait_procs(j->procs, j->nprocs);
    fg_procs = NULL;
    fg_nprocs = 0;
    if (jobctl) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGTTOU);
        sigprocmask(SIG_BLOCK, &set, NULL);
        tcsetpgrp(ttyfd, shell_pgrp);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
    }
    if (stopped) {
        j->state = JS_STOP;
        fprintf(stderr, "[%d]+  Stopped\t%s\n", j->id, j->cmd ? j->cmd : "");
        last_status = 0;
        return 0;
    }
    last_status = proc_status(j->procs, j->nprocs);
    Job **pp = &job_list;
    while (*pp) {
        if (*pp == j) {
            *pp = j->next;
            job_free(j);
            job_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    return last_status;
}

static int bi_bg(int argc, char **argv) {
    const char *spec = (argc > 1) ? argv[1] : NULL;
    Job *j = find_job_spec(spec);
    if (!j) {
        fprintf(stderr, "bg: %s: no such job\n", spec ? spec : "%%");
        return 1;
    }
    if (j->state != JS_STOP) {
        fprintf(stderr, "bg: job %d already in background\n", j->id);
        return 1;
    }
    j->state = JS_RUN;
    kill(-j->pgid, SIGCONT);
    fprintf(stderr, "[%d] %s\n", j->id, j->cmd ? j->cmd : "");
    return 0;
}

static int bi_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("%s %s\n", BSH_NAME, BSH_VERSION);
    printf("builtins: ");
    for (int i = 0; builtin_table[i].name; i++)
        printf("%s%s", i ? " " : "", builtin_table[i].name);
    printf("\n");
    printf("usage: bsh [-c CMD] [--help] [--version] [SCRIPT [ARG...]]\n");
    return 0;
}

static int run_builtin(int argc, char **argv) {
    BiFn fn = find_builtin(argv[0]);
    if (!fn) return -1;
    return fn(argc, argv);
}

/* ------------------------------------------------------------------ */
/* executor                                                            */
/* ------------------------------------------------------------------ */
static const char *cmdline_of(Stage *s) {
    static char buf[512];
    DBuf d;
    db_init(&d);
    for (Stage *st = s; st; st = st->next) {
        if (st != s) db_puts(&d, " | ");
        if (st->words) {
            char tmp[512];
            for (WPart *p = st->words->parts; p; p = p->next) {
                if (!p->isvar) { db_puts(&d, p->s); continue; }
                const char *v = resolve_var(p->s, tmp, sizeof tmp);
                if (v) db_puts(&d, v);
            }
        }
    }
    if (!d.b) { buf[0] = 0; return buf; }
    if (db_reserve(&d, 1)) { db_free(&d); buf[0] = 0; return buf; }
    d.b[d.len] = 0;
    snprintf(buf, sizeof buf, "%s", d.b);
    db_free(&d);
    return buf;
}

static void take_tty(pid_t pgid) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTTOU);
    sigprocmask(SIG_BLOCK, &set, NULL);
    tcsetpgrp(ttyfd, pgid);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/* exec a simple command in-process: builtin (with redirs save/restore) */
static int exec_inproc(Stage *s, Cmd *cmd, char **argv, int argc) {
    int saved[3] = { -1, -1, -1 };
    int have_redirs = 0;
    if (s->redirs) {
        have_redirs = 1;
        expand_redir_files(s->redirs);
        for (int i = 0; i < 3; i++) saved[i] = dup(i);
        if (apply_redirs(s->redirs) < 0) {
            for (int i = 0; i < 3; i++)
                if (saved[i] >= 0) { dup2(saved[i], i); close(saved[i]); }
            return 1;
        }
    }
    int nass = cmd ? cmd->nassign : 0;
    char **oldv = nass ? calloc((size_t)nass, sizeof(char *)) : NULL;
    int *olde = nass ? malloc((size_t)nass * sizeof(int)) : NULL;
    for (int i = 0; i < nass; i++) {
        Var *v = find_var(cmd->assigns[i].name);
        oldv[i] = v ? strdup(v->value ? v->value : "") : NULL;
        olde[i] = v ? v->exported : 0;
        char *val = parts_join(cmd->assigns[i].value);
        setvar(cmd->assigns[i].name, val ? val : "", 1);
        free(val);
    }

    int status = run_builtin(argc, argv);
    fflush(stdout);
    fflush(stderr);

    for (int i = 0; i < nass; i++) {
        if (oldv[i]) {
            Var *v = find_var(cmd->assigns[i].name);
            if (v) { free(v->value); v->value = oldv[i]; v->exported = olde[i]; }
            else { setvar(cmd->assigns[i].name, oldv[i], olde[i]); free(oldv[i]); }
        } else {
            unsetvar(cmd->assigns[i].name);
        }
    }
    free(oldv);
    free(olde);

    if (have_redirs) {
        for (int i = 0; i < 3; i++)
            if (saved[i] >= 0) { dup2(saved[i], i); close(saved[i]); }
    }
    return status;
}

/* external command with fork; returns exit status via *status.
 * argv is owned by the caller (freed after this returns). */
static void exec_external(Stage *s, Cmd *cmd, char **argv, int *status) {
    char **envp = build_envp(cmd ? cmd->assigns : NULL, cmd ? cmd->nassign : 0);

    pid_t pid = fork();
    if (pid == 0) {
        child_reset_signals();
        if (jobctl) setpgid(0, 0);
        expand_redir_files(s->redirs);
        if (apply_redirs(s->redirs) < 0) _exit(1);
        execvpe(argv[0], argv, envp);
        if (errno == ENOENT)
            fprintf(stderr, "bsh: %s: command not found\n", argv[0]);
        else
            fprintf(stderr, "bsh: %s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    if (envp) {
        for (int i = 0; envp[i]; i++) free(envp[i]);
        free(envp);
    }
    if (pid < 0) {
        fprintf(stderr, "bsh: fork: %s\n", strerror(errno));
        *status = 126;
        return;
    }
    if (jobctl) {
        setpgid(pid, pid);
        take_tty(pid);
        Proc procs[1];
        procs[0].pid = pid;
        procs[0].st = PS_RUN;
        fg_procs = procs;
        fg_nprocs = 1;
        int stopped = wait_procs(procs, 1);
        fg_procs = NULL;
        fg_nprocs = 0;
        take_tty(shell_pgrp);
        if (stopped) {
            Job *j = job_add(pid, procs, 1, cmdline_of(s), JS_STOP, NULL);
            fprintf(stderr, "[%d]+  Stopped\t%s\n", j ? j->id : 1, cmdline_of(s));
            *status = 0;
        } else {
            *status = proc_status(procs, 1);
        }
    } else {
        *status = wait_one(pid);
    }
}

/* simple command: builtin (in-process), assignment, or external */
static int exec_simple(Stage *s, Cmd *cmd) {
    int argc = 0;
    char **argv = (s && s->words) ? words_to_argv(s->words, &argc) : NULL;
    if (s && s->words && !argv) {
        fprintf(stderr, "bsh: out of memory\n");
        return 2;
    }
    BiFn bi = (argc > 0) ? find_builtin(argv[0]) : NULL;
    if (bi) {
        int st = exec_inproc(s, cmd, argv, argc);
        free(argv);
        return st;
    }
    if (argc == 0) {
        /* assignment-only or bare redirection */
        if (cmd && cmd->nassign) {
            for (int i = 0; i < cmd->nassign; i++) {
                char *val = parts_join(cmd->assigns[i].value);
                setvar(cmd->assigns[i].name, val ? val : "", 0);
                free(val);
            }
        }
        int status = 0;
        if (s && s->redirs) {
            int sv[3] = { -1, -1, -1 };
            expand_redir_files(s->redirs);
            for (int i = 0; i < 3; i++) sv[i] = dup(i);
            if (apply_redirs(s->redirs) < 0) status = 1;
            for (int i = 0; i < 3; i++)
                if (sv[i] >= 0) { dup2(sv[i], i); close(sv[i]); }
        }
        return status;
    }
    int status = 127;
    exec_external(s, cmd, argv, &status);
    free(argv);
    return status;
}

/* run a pipeline; returns exit status (of the last stage) */
static int exec_pipeline(Stage *stages, Cmd *cmd) {
    int n = 0;
    for (Stage *s = stages; s; s = s->next) n++;
    if (n > MAX_STAGES) {
        fprintf(stderr, "bsh: too many pipeline stages\n");
        return 2;
    }
    if (n == 1) return exec_simple(stages, cmd);

    int pipes[MAX_STAGES - 1][2];
    pid_t pids[MAX_STAGES] = { 0 };
    Proc procs[MAX_STAGES];
    int prev = -1;
    Stage *st = stages;

    for (int i = 0; i < n; i++) pids[i] = -1;
    for (int i = 0; i < n; i++, st = st->next) {
        if (i < n - 1) {
            if (pipe(pipes[i]) < 0) {
                fprintf(stderr, "bsh: pipe: %s\n", strerror(errno));
                for (int k = 0; k < i; k++) if (pids[k] > 0) kill(pids[k], SIGKILL);
                return 2;
            }
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "bsh: fork: %s\n", strerror(errno));
            return 2;
        }
        pids[i] = pid;
        if (pid == 0) {
            child_reset_signals();
            if (jobctl) setpgid(0, 0);
            if (i > 0) {
                dup2(prev, 0);
                close(prev);
            }
            if (i < n - 1) {
                dup2(pipes[i][1], 1);
                close(pipes[i][0]);
                close(pipes[i][1]);
            }
            expand_redir_files(st->redirs);
            if (apply_redirs(st->redirs) < 0) _exit(1);
            if (st->words) {
                int argc = 0;
                char **argv = words_to_argv(st->words, &argc);
                if (!argv) _exit(126);
                if (argc > 0) {
                    /* prefix assignments apply to the first stage */
                    if (i == 0 && cmd && cmd->nassign) {
                        for (int k = 0; k < cmd->nassign; k++) {
                            char *val = parts_join(cmd->assigns[k].value);
                            setvar(cmd->assigns[k].name, val ? val : "", 1);
                            free(val);
                        }
                    }
                    BiFn bi = find_builtin(argv[0]);
                    if (bi) {
                        int st2 = bi(argc, argv);
                        fflush(stdout);
                        free(argv);
                        _exit(st2);
                    }
                    char **envp = build_envp((i == 0) ? (cmd ? cmd->assigns : NULL) : NULL,
                                             (i == 0) ? (cmd ? cmd->nassign : 0) : 0);
                    execvpe(argv[0], argv, envp);
                    if (errno == ENOENT)
                        fprintf(stderr, "bsh: %s: command not found\n", argv[0]);
                    else
                        fprintf(stderr, "bsh: %s: %s\n", argv[0], strerror(errno));
                    _exit(errno == ENOENT ? 127 : 126);
                }
                free(argv);
            }
            _exit(0);
        }
        if (i > 0) close(prev);
        if (i < n - 1) {
            close(pipes[i][1]);
            prev = pipes[i][0];
        }
        procs[i].pid = pid;
        procs[i].st = PS_RUN;
    }
    if (prev >= 0) close(prev);

    if (jobctl) {
        pid_t pgrp = pids[0];
        for (int i = 0; i < n; i++) setpgid(pids[i], pgrp);
        take_tty(pgrp);
        fg_procs = procs;
        fg_nprocs = n;
        int stopped = wait_procs(procs, n);
        fg_procs = NULL;
        fg_nprocs = 0;
        take_tty(shell_pgrp);
        if (stopped) {
            Job *j = job_add(pgrp, procs, n, cmdline_of(stages), JS_STOP, NULL);
            fprintf(stderr, "[%d]+  Stopped\t%s\n", j ? j->id : 1, cmdline_of(stages));
            return 0;
        }
        return proc_status(procs, n);
    }
    /* Non-job-control wait.  Wait on the full procs[] array as fg_procs so the
     * SIGCHLD reap handler records each stage's real exit status (waiting each
     * pid in isolation lets a fast pipeline's later stages get reaped by the
     * handler first, which then shows up as ECHILD -> reported status 0). */
    fg_procs = procs;
    fg_nprocs = n;
    wait_procs(procs, n);
    fg_procs = NULL;
    fg_nprocs = 0;
    return proc_status(procs, n);
}

/* execute a chain of commands connected by && / || */
static int exec_chain_fg(Cmd *c) {
    int status = 0;
    Cmd *first = c;
    if (first->stages) status = exec_pipeline(first->stages, first);
    else if (first->nassign) status = exec_simple(NULL, first);
    Cmd *cur = first;
    while (cur->next) {
        Oper op = cur->op;
        cur = cur->next;
        if ((op == OP_AND && status != 0) || (op == OP_OR && status == 0))
            continue;
        if (cur->stages)
            status = exec_pipeline(cur->stages, cur);
        else if (cur->nassign)
            status = exec_simple(NULL, cur);
    }
    return status;
}

/* execute a statement (chain); if bg, run in a subshell */
static int exec_statement(Cmd *c, int bg) {
    if (!c) return 0;
    if (bg) {
        const char *cmdtext = cmdline_of(c->stages);
        pid_t pid = fork();
        if (pid == 0) {
            child_reset_signals();
            if (jobctl) setpgid(0, 0);
            jobctl = 0;
            fg_procs = NULL;
            fg_nprocs = 0;
            int st = exec_chain_fg(c);
            exit(st);
        }
        if (pid > 0) {
            Job *j = NULL;
            if (jobctl) {
                setpgid(pid, pid);
                Proc procs[1];
                procs[0].pid = pid;
                procs[0].st = PS_RUN;
                j = job_add(pid, procs, 1, cmdtext, JS_RUN, c);
                printf("[%d] %d\n", j ? j->id : 1, (int)pid);
            } else {
                Proc procs[1];
                procs[0].pid = pid;
                procs[0].st = PS_RUN;
                j = job_add(0, procs, 1, cmdtext, JS_RUN, c);
            }
            (void)j;
            pending_bg_tree = NULL;
        }
        return 0;
    }
    return exec_chain_fg(c);
}

/* ------------------------------------------------------------------ */
/* statement execution                                                 */
/* ------------------------------------------------------------------ */
static int exec_string(const char *line, int is_rc) {
    (void)is_rc;
    if (!line || !*line) return 0;
    Tok *toks = NULL;
    const char *errmsg = NULL;
    int rc = tokenize(line, &toks, &errmsg);
    if (rc == 1) {
        fprintf(stderr, "bsh: syntax error: unterminated quote\n");
        return 2;
    }
    if (rc < 0) {
        fprintf(stderr, "bsh: syntax error: %s\n", errmsg ? errmsg : "bad token");
        return 2;
    }
    /* alias expansion: first word after any operator */
    {
        int depth = 0;
        Tok *t = toks;
        int at_start = 1;
        while (t) {
            if (t->type == T_OP) {
                at_start = 1;
                t = t->next;
                continue;
            }
            if (at_start && depth < MAX_ALIAS_DEPTH) {
                char *wname = NULL;
                WPart *p = t->parts;
                if (p && !p->isvar && !p->quoted && !p->next) wname = strdup(p->s);
                Alias *a = wname ? find_alias(wname) : NULL;
                free(wname);
                if (a && strcmp(a->name, a->value)) {
                    Tok *at = NULL;
                    const char *emsg2 = NULL;
                    int rc2 = tokenize(a->value, &at, &emsg2);
                    if (rc2 == 0 && at) {
                        Tok *at_end = at;
                        while (at_end->next) at_end = at_end->next;
                        at_end->next = t->next;
                        Tok **pp = &toks;
                        while (*pp && *pp != t) pp = &(*pp)->next;
                        *pp = at;
                        part_free(t->parts);
                        free(t);
                        depth++;
                        t = at;
                        continue;
                    }
                    tok_free(at);
                }
                at_start = 0;
            }
            t = t->next;
        }
    }
    Cmd *cmds = NULL;
    rc = parse(toks, &cmds, &errmsg);
    tok_free(toks);
    if (rc < 0) {
        fprintf(stderr, "bsh: syntax error: %s\n", errmsg ? errmsg : "bad parse");
        return 2;
    }
    Cmd *cur = cmds;
    int status = 0;
    while (cur) {
        if (!cur->stages && !cur->nassign) { Cmd *nx = cur->next; free(cur); cur = nx; continue; }
        Cmd *last = cur;
        while (last->next && last->op != OP_TERM && last->op != OP_BG)
            last = last->next;
        int bg = (last->op == OP_BG);
        Cmd *next = last->next;
        last->next = NULL;
        pending_bg_tree = bg ? cur : NULL;
        last_status = exec_statement(cur, bg);
        status = last_status;
        if (exit_requested) break;
        if (!bg) {
            bsh_free_cmd_tree(cur);
        }
        job_trim();
        cur = next;
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* prompts                                                             */
/* ------------------------------------------------------------------ */
static void expand_prompt(const char *ps, DBuf *out) {
    char hostname[256] = "host";
    gethostname(hostname, sizeof hostname);
    char *dot = strchr(hostname, '.');
    if (dot) *dot = 0;
    const char *user = getenv("USER");
    if (!user) user = "user";
    for (const char *p = ps; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'u': db_puts(out, user); break;
            case 'h': db_puts(out, hostname); break;
            case 'H': {
                char full[256];
                gethostname(full, sizeof full);
                db_puts(out, full);
                break;
            }
            case 'w': {
                char cwd[PATH_MAX];
                const char *home = getenv("HOME");
                if (getcwd(cwd, sizeof cwd)) {
                    if (home && !strncmp(cwd, home, strlen(home))) {
                        db_puts(out, "~");
                        db_puts(out, cwd + strlen(home));
                    } else {
                        db_puts(out, cwd);
                    }
                }
                break;
            }
            case 'W': {
                char cwd[PATH_MAX];
                if (getcwd(cwd, sizeof cwd)) {
                    char *base = strrchr(cwd, '/');
                    db_puts(out, base ? base + 1 : cwd);
                }
                break;
            }
            case '$': db_putc(out, geteuid() == 0 ? '#' : '$'); break;
            case 'n': db_putc(out, '\n'); break;
            case 't': {
                time_t now = time(NULL);
                struct tm *tm = localtime(&now);
                db_printf(out, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
                break;
            }
            case '!': db_printf(out, "%d", hist_n + 1); break;
            case '\\': db_putc(out, '\\'); break;
            case '[': case ']': break;
            default: db_putc(out, *p); break;
            }
        } else if (*p == '$') {
            if (p[1] == '{') {
                const char *e = strchr(p + 2, '}');
                if (e) {
                    char name[64];
                    if ((size_t)(e - p - 2) < sizeof name) {
                        memcpy(name, p + 2, (size_t)(e - p - 2));
                        name[e - p - 2] = 0;
                        const char *v = getvar(name);
                        if (v) db_puts(out, v);
                        p = e;
                    }
                    continue;
                }
            }
            const char *s = p + 1;
            if (isalpha((unsigned char)*s) || *s == '_') {
                char name[64];
                size_t i = 0;
                while ((isalnum((unsigned char)*s) || *s == '_') && i < sizeof name - 1)
                    name[i++] = *s++;
                name[i] = 0;
                const char *v = getvar(name);
                if (v) db_puts(out, v);
                p = s - 1;
                continue;
            }
            db_putc(out, '$');
        } else {
            db_putc(out, *p);
        }
    }
}

static void print_prompt(const char *ps) {
    DBuf b;
    db_init(&b);
    expand_prompt(ps, &b);
    if (b.b) {
        if (db_reserve(&b, 1)) { db_free(&b); return; }
        b.b[b.len] = 0;
        fputs(b.b, stdout);
        fflush(stdout);
    }
    db_free(&b);
}

/* ------------------------------------------------------------------ */
/* completion                                                          */
/* ------------------------------------------------------------------ */
static int is_word_break(char c) {
    return c == ' ' || c == '\t' || c == '|' || c == '&' || c == ';' ||
           c == '<' || c == '>' || c == '(' || c == ')' || c == '\n';
}

/* returns array of candidate strings (malloc'd), n set; caller frees */
static char **complete_prefix(const char *prefix, int *n, int is_cmd) {
    *n = 0;
    int cap = 0, cnt = 0;
    char **res = NULL;
    size_t plen = strlen(prefix);

    if (is_cmd) {
        for (int i = 0; builtin_table[i].name; i++) {
            if (!strncmp(builtin_table[i].name, prefix, plen)) {
                if (cnt >= cap) { cap = cap ? cap * 2 : 16; res = realloc(res, (size_t)cap * sizeof(char *)); }
                res[cnt++] = strdup(builtin_table[i].name);
            }
        }
        for (Alias *a = aliases; a; a = a->next) {
            if (!strncmp(a->name, prefix, plen)) {
                if (cnt >= cap) { cap = cap ? cap * 2 : 16; res = realloc(res, (size_t)cap * sizeof(char *)); }
                res[cnt++] = strdup(a->name);
            }
        }
    }
    if (is_cmd) {
        const char *path = getenv("PATH");
        if (path) {
            char *dup = strdup(path);
            if (dup) {
                char *save = NULL;
                for (char *d = strtok_r(dup, ":", &save); d && cnt <= 512;
                     d = strtok_r(NULL, ":", &save)) {
                    DIR *dir = opendir(*d ? d : ".");
                    if (!dir) continue;
                    struct dirent *de;
                    while ((de = readdir(dir)) && cnt <= 512) {
                        if (strncmp(de->d_name, prefix, plen)) continue;
                        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                        if (cnt >= cap) { cap = cap ? cap * 2 : 32; res = realloc(res, (size_t)cap * sizeof(char *)); }
                        res[cnt++] = strdup(de->d_name);
                    }
                    closedir(dir);
                }
                free(dup);
            }
        }
    } else {
        const char *slash = strrchr(prefix, '/');
        char dir[PATH_MAX], base[PATH_MAX];
        if (slash) {
            size_t dl = (size_t)(slash - prefix) + 1;
            if (dl >= sizeof dir) dl = sizeof dir - 1;
            memcpy(dir, prefix, dl);
            dir[dl] = 0;
            strncpy(base, slash + 1, sizeof base - 1);
            base[sizeof base - 1] = 0;
        } else {
            strcpy(dir, "./");
            strncpy(base, prefix, sizeof base - 1);
            base[sizeof base - 1] = 0;
        }
        DIR *d = opendir(*dir ? dir : ".");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) && cnt <= 512) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                if (strncmp(de->d_name, base, strlen(base))) continue;
                size_t dl = strlen(dir);
                size_t nl = strlen(de->d_name);
                int isdir = de->d_type == DT_DIR;
                char *full = malloc(dl + nl + 4);
                if (!full) continue;
                if (strcmp(dir, "./")) {
                    strcpy(full, dir);
                    if (dl && full[dl - 1] != '/') { full[dl] = '/'; full[dl + 1] = 0; }
                } else {
                    full[0] = 0;
                }
                size_t fl = strlen(full);
                strcpy(full + fl, de->d_name);
                fl += nl;
                if (isdir) { full[fl++] = '/'; full[fl] = 0; }
                if (cnt >= cap) { cap = cap ? cap * 2 : 16; res = realloc(res, (size_t)cap * sizeof(char *)); }
                res[cnt++] = full;
            }
            closedir(d);
        }
    }
    if (cnt > 1) {
        qsort(res, (size_t)cnt, sizeof(char *), cmp_str);
        int k = 0;
        for (int i = 0; i < cnt; i++) {
            if (k == 0 || strcmp(res[i], res[k - 1]))
                res[k++] = res[i];
            else
                free(res[i]);
        }
        cnt = k;
    }
    *n = cnt;
    return res;
}

/* returns 0 nothing, 1 line modified, 2 candidates printed */
static int do_completion(char **linep, size_t *cap, size_t *len, size_t *pos) {
    char *buf = *linep;
    size_t pl = *pos;
    while (pl > 0 && !is_word_break(buf[pl - 1])) pl--;
    size_t wl = *pos - pl;
    int is_cmd = 0;
    if (pl == 0) is_cmd = 1;
    else {
        size_t q = pl - 1;
        while (q > 0 && (buf[q] == ' ' || buf[q] == '\t')) q--;
        if (q == 0) is_cmd = 1;
        else is_cmd = is_word_break(buf[q]);
    }
    if (wl == 0) return 0;
    char *word = malloc(wl + 1);
    if (!word) return 0;
    memcpy(word, buf + pl, wl);
    word[wl] = 0;

    if (word[0] == '$') {
        const char *pname = word + 1;
        size_t pnl = strlen(pname);
        int cnt = 0;
        char *matches[256];
        for (Var *v = vars; v && cnt < 255; v = v->next) {
            if (!strncmp(v->name, pname, pnl)) {
                char *m = malloc(strlen(v->name) + 2);
                if (m) { sprintf(m, "$%s", v->name); matches[cnt++] = m; }
            }
        }
        if (cnt == 1) {
            size_t cl = strlen(matches[0]);
            memmove(buf + pl, matches[0], cl);
            *len = pl + cl;
            buf[*len] = 0;
            *pos = *len;
            free(matches[0]);
            free(word);
            return 1;
        }
        for (int i = 0; i < cnt; i++) free(matches[i]);
        free(word);
        return 0;
    }

    int n = 0;
    char **cands = complete_prefix(word, &n, is_cmd);
    free(word);
    if (n == 0) return 0;
    if (n == 1) {
        size_t cl = strlen(cands[0]);
        size_t rest = *len - *pos;
        size_t need = pl + cl + rest;
        if (need + 1 > *cap) {
            char *nb = realloc(buf, need + 65);
            if (!nb) { free(cands[0]); free(cands); return 0; }
            buf = nb;
            *linep = buf;
            *cap = need + 65;
        }
        memmove(buf + pl + cl, buf + *pos, rest + 1);
        memcpy(buf + pl, cands[0], cl);
        *len = pl + cl + rest;
        buf[*len] = 0;
        *pos = pl + cl;
        free(cands[0]);
        free(cands);
        return 1;
    }
    size_t common = strlen(cands[0]);
    for (int i = 1; i < n && common > 0; i++) {
        size_t k = 0;
        while (k < common && cands[i][k] == cands[0][k]) k++;
        common = k;
    }
    if (common > wl) {
        size_t rest = *len - *pos;
        size_t need = pl + common + rest;
        if (need + 1 > *cap) {
            char *nb = realloc(buf, need + 65);
            if (!nb) { for (int i = 0; i < n; i++) free(cands[i]); free(cands); return 0; }
            buf = nb;
            *linep = buf;
            *cap = need + 65;
        }
        memmove(buf + pl + common, buf + *pos, rest + 1);
        memcpy(buf + pl, cands[0], common);
        *len = pl + common + rest;
        buf[*len] = 0;
        *pos = pl + common;
        for (int i = 0; i < n; i++) free(cands[i]);
        free(cands);
        return 1;
    }
    putchar('\n');
    int col = 0;
    for (int i = 0; i < n && i < 64; i++) {
        if (col && col + (int)strlen(cands[i]) > 76) { putchar('\n'); col = 0; }
        if (col) { putchar(' '); col++; }
        fputs(cands[i], stdout);
        col += (int)strlen(cands[i]);
    }
    if (n > 64) printf("\n... %d candidates", n);
    putchar('\n');
    fflush(stdout);
    for (int i = 0; i < n; i++) free(cands[i]);
    free(cands);
    return 2;
}

/* ------------------------------------------------------------------ */
/* line editor                                                         */
/* ------------------------------------------------------------------ */
static char *ed_line = NULL;
static size_t ed_cap = 0;
static size_t ed_len = 0, ed_pos = 0;
static char saved_edit[1024];
static int has_saved_edit = 0;

static void redraw(void) {
    fputs("\r\x1b[K", stdout);
    print_prompt(getvar("PS1") ? getvar("PS1") : ps1_default);
    fputs(ed_line ? ed_line : "", stdout);
    size_t back = (ed_line ? strlen(ed_line) : 0) - ed_pos;
    if (back) printf("\x1b[%dD", (int)back);
    fflush(stdout);
}

static void ed_putc(char c) {
    if (ed_len + 2 > ed_cap) {
        size_t ncap = ed_cap ? ed_cap * 2 : 128;
        char *nl = realloc(ed_line, ncap);
        if (!nl) return;
        ed_line = nl;
        ed_cap = ncap;
    }
    memmove(ed_line + ed_pos + 1, ed_line + ed_pos, ed_len - ed_pos);
    ed_line[ed_pos] = c;
    ed_len++;
    ed_pos++;
    ed_line[ed_len] = 0;
    redraw();
}

static char *edit_line(void) {
    if (!ed_line) {
        ed_line = malloc(128);
        if (!ed_line) return NULL;
        ed_cap = 128;
    }
    ed_line[0] = 0;
    ed_len = 0;
    ed_pos = 0;
    has_saved_edit = 0;
    int need_prompt = 1;

    for (;;) {
        if (need_prompt) {
            if (jobctl) take_tty(shell_pgrp);
            job_notify();
            print_prompt(getvar("PS1") ? getvar("PS1") : ps1_default);
            need_prompt = 0;
        }
        char c;
        ssize_t n = read(ttyfd, &c, 1);
        if (n <= 0) {
            if (errno == EINTR) {
                if (exit_requested) break;
                if (sigint_flag) {
                    sigint_flag = 0;
                    putchar('\n');
                    fflush(stdout);
                    ed_len = 0;
                    ed_pos = 0;
                    ed_line[0] = 0;
                    need_prompt = 1;
                }
                continue;
            }
            if (n == 0) break;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (ed_len == 0) { need_prompt = 1; continue; }
            putchar('\r');
            putchar('\n');
            fflush(stdout);
            break;
        }
        if (c == 0x03) {
            sigint_flag = 0;
            putchar('\n');
            fflush(stdout);
            ed_len = 0;
            ed_pos = 0;
            ed_line[0] = 0;
            need_prompt = 1;
            continue;
        }
        if (c == 0x04) {
            if (ed_len == 0) break;   /* EOF */
            if (ed_pos < ed_len) {
                memmove(ed_line + ed_pos, ed_line + ed_pos + 1, ed_len - ed_pos - 1);
                ed_len--;
                ed_line[ed_len] = 0;
                redraw();
            }
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            if (ed_pos > 0) {
                memmove(ed_line + ed_pos - 1, ed_line + ed_pos, ed_len - ed_pos);
                ed_pos--;
                ed_len--;
                ed_line[ed_len] = 0;
                redraw();
            }
            continue;
        }
        if (c == 0x1b) {
            char seq[4];
            ssize_t r1 = read(ttyfd, &seq[0], 1);
            if (r1 != 1) continue;
            if (seq[0] == '[' || seq[0] == 'O') {
                ssize_t r2 = read(ttyfd, &seq[1], 1);
                if (r2 != 1) continue;
                if (seq[1] == 'A') {
                    if (hist_pos > 0) {
                        if (!has_saved_edit) {
                            strncpy(saved_edit, ed_line, sizeof saved_edit - 1);
                            saved_edit[sizeof saved_edit - 1] = 0;
                            has_saved_edit = 1;
                        }
                        hist_pos--;
                        const char *entry = (hist_pos < hist_n) ? hist[hist_pos] : "";
                        strcpy(ed_line, entry);
                        ed_len = strlen(ed_line);
                        ed_pos = ed_len;
                        redraw();
                    }
                } else if (seq[1] == 'B') {
                    if (hist_pos < hist_n) {
                        hist_pos++;
                        if (hist_pos < hist_n) {
                            strcpy(ed_line, hist[hist_pos]);
                            ed_len = strlen(ed_line);
                            ed_pos = ed_len;
                        } else {
                            if (has_saved_edit) {
                                strcpy(ed_line, saved_edit);
                                has_saved_edit = 0;
                            } else {
                                ed_line[0] = 0;
                            }
                            ed_len = strlen(ed_line);
                            ed_pos = ed_len;
                        }
                        redraw();
                    }
                } else if (seq[1] == 'C') {
                    if (ed_pos < ed_len) { ed_pos++; redraw(); }
                } else if (seq[1] == 'D') {
                    if (ed_pos > 0) { ed_pos--; redraw(); }
                } else if (seq[1] == 'H') {
                    ed_pos = 0;
                    redraw();
                } else if (seq[1] == 'F') {
                    ed_pos = ed_len;
                    redraw();
                } else if (seq[1] == '1' || seq[1] == '3' || seq[1] == '4' ||
                           seq[1] == '7' || seq[1] == '8') {
                    char c3;
                    ssize_t r3 = read(ttyfd, &c3, 1);
                    if (r3 == 1 && c3 == '~') {
                        if (seq[1] == '1' || seq[1] == '7') { ed_pos = 0; redraw(); }
                        else if (seq[1] == '4' || seq[1] == '8') { ed_pos = ed_len; redraw(); }
                        else if (seq[1] == '3') {
                            if (ed_pos < ed_len) {
                                memmove(ed_line + ed_pos, ed_line + ed_pos + 1, ed_len - ed_pos - 1);
                                ed_len--;
                                ed_line[ed_len] = 0;
                                redraw();
                            }
                        }
                    }
                }
            } else if (seq[0] == 0x7f) {
                while (ed_pos > 0 && ed_line[ed_pos - 1] == ' ') ed_pos--;
                while (ed_pos > 0 && ed_line[ed_pos - 1] != ' ') ed_pos--;
                ed_line[ed_pos] = 0;
                ed_len = ed_pos;
                redraw();
            }
            continue;
        }
        if (c == 0x01) { ed_pos = 0; redraw(); continue; }
        if (c == 0x05) { ed_pos = ed_len; redraw(); continue; }
        if (c == 0x02) { if (ed_pos > 0) { ed_pos--; redraw(); } continue; }
        if (c == 0x06) { if (ed_pos < ed_len) { ed_pos++; redraw(); } continue; }
        if (c == 0x0b) {
            ed_line[ed_pos] = 0;
            ed_len = ed_pos;
            redraw();
            continue;
        }
        if (c == 0x15) {
            ed_len = 0;
            ed_pos = 0;
            ed_line[0] = 0;
            redraw();
            continue;
        }
        if (c == 0x17) {
            while (ed_pos > 0 && ed_line[ed_pos - 1] == ' ') ed_pos--;
            while (ed_pos > 0 && ed_line[ed_pos - 1] != ' ') ed_pos--;
            ed_line[ed_pos] = 0;
            ed_len = ed_pos;
            redraw();
            continue;
        }
        if (c == 0x0c) {
            fputs("\x1b[2J\x1b[H", stdout);
            redraw();
            continue;
        }
        if (c == 0x09) {
            if (ed_len > 0) {
                int r = do_completion(&ed_line, &ed_cap, &ed_len, &ed_pos);
                if (r == 1) redraw();
                else if (r == 2) {
                    /* candidates printed above; redraw prompt + line */
                    redraw();
                }
            }
            continue;
        }
        if (c == 0x1a) continue;   /* ^Z ignored at prompt */
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
            ed_putc(c);
            continue;
        }
    }
    if (ed_len > 0) {
        char *r = malloc(ed_len + 1);
        if (!r) return NULL;
        memcpy(r, ed_line, ed_len);
        r[ed_len] = 0;
        return r;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* rc files / stream execution                                         */
/* ------------------------------------------------------------------ */
static void source_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    DBuf content;
    db_init(&content);
    char tmp[512];
    size_t rd;
    while ((rd = fread(tmp, 1, sizeof tmp, f)) > 0)
        db_putn(&content, tmp, rd);
    fclose(f);
    if (content.b) {
        if (db_reserve(&content, 1)) { db_free(&content); return; }
        content.b[content.len] = 0;
        char *save = NULL;
        for (char *line = strtok_r(content.b, "\n", &save); line && !exit_requested;
             line = strtok_r(NULL, "\n", &save))
            exec_string(line, 1);
    }
    db_free(&content);
}

/* execute a whole stream (script file / stdin), honoring line
 * continuations and multi-line quoting */
static void exec_stream(const char *content) {
    if (!content) return;
    size_t clen = strlen(content);
    char *logical = malloc(clen + 2);
    if (!logical) return;
    size_t li = 0;
    int q = 0;
    for (size_t i = 0; i <= clen; i++) {
        char c = content[i];
        if (c == 0) break;
        if (q == 1) {
            if (c == '\'') q = 0;
            logical[li++] = c;
            continue;
        }
        if (q == 2) {
            if (c == '"') q = 0;
            logical[li++] = c;
            continue;
        }
        if (c == '\'') q = 1;
        else if (c == '"') q = 2;
        else if (c == '\\' && content[i + 1] == '\n') { i++; continue; }
        else if (c == '\n') {
            if (li > 0) {
                logical[li] = 0;
                exec_string(logical, 1);
                if (exit_requested) break;
                li = 0;
            }
            continue;
        }
        logical[li++] = c;
    }
    if (li > 0 && !exit_requested) {
        logical[li] = 0;
        exec_string(logical, 1);
    }
    free(logical);
}

/* ------------------------------------------------------------------ */
/* signals                                                             */
/* ------------------------------------------------------------------ */
static void on_sigint(int sig) { (void)sig; sigint_flag = 1; }
static void on_sigterm(int sig) { (void)sig; exit_requested = 1; exit_code = 128 + SIGTERM; }

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void usage(void) {
    printf("%s %s\n", BSH_NAME, BSH_VERSION);
    printf("usage: bsh [-c COMMAND] [-i] [-l] [--norc] [--help] [--version] [SCRIPT [ARG...]]\n");
    printf("  -c CMD     execute CMD and exit\n");
    printf("  -i         force interactive\n");
    printf("  -l         login shell (sources rc files)\n");
    printf("  --norc     do not read /etc/bshrc or ~/.bshrc\n");
    printf("  --help     show this help\n");
    printf("  --version  show version\n");
}

int main(int argc, char **argv) {
    const char *cmd_str = NULL;
    const char *script = NULL;
    int norc = 0;
    int force_int = 0;
    shell_pid = getpid();

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("%s %s\n", BSH_NAME, BSH_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--norc")) { norc = 1; continue; }
        if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--login")) { login_shell = 1; continue; }
        if (!strcmp(argv[i], "-i")) { force_int = 1; continue; }
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 < argc) cmd_str = argv[++i];
            else { fprintf(stderr, "bsh: -c requires an argument\n"); return 2; }
            continue;
        }
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "bsh: unknown option: %s\n", argv[i]);
            return 2;
        }
        break;
    }
    if (i < argc && !cmd_str) script = argv[i];
    if (script) {
        pos_params[0] = script;
        pos_argc = 0;
        for (int k = i + 1; k < argc && pos_argc < 9; k++) {
            pos_params[pos_argc + 1] = argv[k];
            pos_argc++;
        }
    } else if (cmd_str) {
        pos_params[0] = (i < argc) ? argv[i] : "bsh";
        pos_argc = 0;
        for (int k = i + 1; k < argc && pos_argc < 9; k++) {
            pos_params[pos_argc + 1] = argv[k];
            pos_argc++;
        }
    }

    if (!getenv("PATH")) setenv("PATH", DEFAULT_PATH, 1);
    setvar("PATH", getenv("PATH"), 1);

    /* import the environment into the shell variable table */
    for (char **ep = environ; ep && *ep; ep++) {
        char *eq = strchr(*ep, '=');
        if (!eq) continue;
        char name[128];
        size_t nl = (size_t)(eq - *ep);
        if (nl >= sizeof name) continue;
        memcpy(name, *ep, nl);
        name[nl] = 0;
        if (is_valid_name(name)) setvar(name, eq + 1, 1);
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) setvar("PWD", cwd, 1);
    if (!getenv("USER")) {
        struct passwd *pw = getpwuid(geteuid());
        if (pw) setenv("USER", pw->pw_name, 1);
    }
    char hn[256] = "";
    if (!gethostname(hn, sizeof hn)) setenv("HOSTNAME", hn, 1);
    setvar("USER", getenv("USER") ? getenv("USER") : "user", 1);
    setvar("HOSTNAME", hn, 1);
    setvar("PS1", ps1_default, 0);
    setvar("PS2", "> ", 0);

    interactive = force_int || (isatty(0) && isatty(1));

    if (interactive) {
        ttyfd = 0;
        shell_pgrp = getpgrp();
        if (shell_pgrp != getpid()) {
            if (setpgid(0, 0) == 0) shell_pgrp = getpid();
            else shell_pgrp = getpgrp();
        }
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        take_tty(shell_pgrp);
        jobctl = 1;
    }

    signal(SIGCHLD, on_sigchld);
    signal(SIGINT, interactive ? on_sigint : SIG_DFL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, on_sigterm);
    signal(SIGPIPE, SIG_IGN);

    if (interactive) {
        hist_load();
        if (!norc) {
            source_file("/etc/bshrc");
            if (!exit_requested) {
                const char *home = getenv("HOME");
                if (home) {
                    char path[PATH_MAX];
                    snprintf(path, sizeof path, "%s/.bshrc", home);
                    source_file(path);
                }
            }
        }
    }

    int rc = 0;
    if (cmd_str) {
        rc = exec_string(cmd_str, 0);
        if (exit_requested) rc = exit_code;
    } else if (script) {
        FILE *f = fopen(script, "r");
        if (!f) {
            fprintf(stderr, "bsh: %s: %s\n", script, strerror(errno));
            return 127;
        }
        DBuf content;
        db_init(&content);
        char tmp[512];
        size_t rd;
        while ((rd = fread(tmp, 1, sizeof tmp, f)) > 0)
            db_putn(&content, tmp, rd);
        fclose(f);
        if (content.b) {
            if (db_reserve(&content, 1)) { db_free(&content); rc = 1; }
            else {
                content.b[content.len] = 0;
                exec_stream(content.b);
                rc = last_status;
            }
        }
        db_free(&content);
        if (exit_requested) rc = exit_code;
    } else if (interactive) {
        for (;;) {
            char *line = NULL;
            term_raw();
            line = edit_line();
            term_restore();
            if (!line) break;
            if (line[0] && line[0] != '#') hist_add(line);
            exec_string(line, 0);
            free(line);
            rc = last_status;
            if (exit_requested) break;
        }
        if (exit_requested) rc = exit_code;
    } else {
        DBuf content;
        db_init(&content);
        char tmp[512];
        size_t rd;
        while ((rd = fread(tmp, 1, sizeof tmp, stdin)) > 0)
            db_putn(&content, tmp, rd);
        if (content.b) {
            if (db_reserve(&content, 1)) { db_free(&content); rc = 1; }
            else {
                content.b[content.len] = 0;
                exec_stream(content.b);
                rc = last_status;
            }
        }
        db_free(&content);
        if (exit_requested) rc = exit_code;
    }

    if (interactive) {
        hist_save();
        term_restore();
        for (Job *j = job_list; j; j = j->next) {
            if (j->state == JS_RUN || j->state == JS_STOP) {
                if (j->pgid) kill(-j->pgid, SIGHUP);
                else if (j->nprocs) kill(j->procs[0].pid, SIGHUP);
            }
        }
    }
    return rc;
}
