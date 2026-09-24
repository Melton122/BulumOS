/*
 * bctl - BulumOS service manager (Phase 2)
 *
 * Commands:
 *   bctl boot                 start all enabled services (used by binit)
 *   bctl shutdown             stop all enabled services (reverse order)
 *   bctl start <name>         start a service (dependencies first)
 *   bctl stop  <name>         stop a service (dependents first)
 *   bctl restart <name>       restart a service
 *   bctl status [name]        report service state(s)
 *   bctl list                 list known services + enabled flag
 *   bctl enable <name>        enable at boot
 *   bctl disable <name>       disable at boot
 *   bctl supervise <name>     respawn monitor (started by boot)
 *   bctl diagnose             system diagnostics
 *   bctl log [name]           view service log
 *
 * Layout:
 *   /etc/bulum/services/<name>/meta      key=value service metadata
 *   /etc/bulum/services/<name>/control   start|stop|restart|status script
 *   /etc/bulum/enabled/<name>            marker file, present = enabled
 *   /var/log/bulum/<name>.log            per-service log
 *   /var/log/bulum/boot.log              boot/shutdown summary
 *   /var/run/bulum/<name>.state          last known state
 *   /var/run/bulum/<name>.supervise      pid of respawn monitor
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SVC_DIR   "/etc/bulum/services"
#define ENABLED   "/etc/bulum/enabled"
#define LOG_DIR   "/var/log/bulum"
#define STATE_DIR "/var/run/bulum"

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_YELLOW "\033[33m"
#define C_CYAN   "\033[36m"
#define C_GRAY   "\033[90m"

#define MAX_SVC  128
#define MAX_DEPS 16
#define START_TIMEOUT 8
#define STOP_TIMEOUT 15

typedef struct {
    char name[64];
    char desc[128];
    char deps[MAX_DEPS][64];
    int  ndeps;
    int  respawn;
    int  oneshot;
    char pidfile[128];
} svc_t;

static svc_t list[MAX_SVC];
static int   nlist;

/* ---------- low-level helpers ---------- */

static const char *now_stamp(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void ensure_dirs(void)
{
    mkdir_p(SVC_DIR);
    mkdir_p(ENABLED);
    mkdir_p(LOG_DIR);
    mkdir_p(STATE_DIR);
}

static void write_state(const char *name, const char *fmt, ...)
{
    va_list ap;
    char buf[512], path[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    snprintf(path, sizeof(path), "%s/%s.state", STATE_DIR, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

static void append_file(const char *path, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

static void boot_log(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char path[512];
    snprintf(path, sizeof(path), "%s/boot.log", LOG_DIR);
    append_file(path, "[%s] %s", now_stamp(), buf);
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }

static int read_int_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int v = 0;
    fscanf(f, "%d", &v);
    fclose(f);
    return v;
}

/* ---------- service registry ---------- */

static int svc_index(const char *name)
{
    for (int i = 0; i < nlist; i++)
        if (strcmp(list[i].name, name) == 0)
            return i;
    return -1;
}

static int is_enabled(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ENABLED, name);
    return file_exists(path);
}

static void parse_meta(svc_t *s)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/meta", SVC_DIR, s->name);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "DESC"))
            snprintf(s->desc, sizeof(s->desc), "%s", v);
        else if (!strcmp(line, "RESPAWN"))
            s->respawn = atoi(v);
        else if (!strcmp(line, "ONESHOT"))
            s->oneshot = atoi(v);
        else if (!strcmp(line, "PIDFILE"))
            snprintf(s->pidfile, sizeof(s->pidfile), "%s", v);
        else if (!strcmp(line, "DEPS")) {
            char *tok = strtok(v, " \t,");
            while (tok && s->ndeps < MAX_DEPS) {
                snprintf(s->deps[s->ndeps], sizeof(s->deps[0]), "%s", tok);
                s->ndeps++;
                tok = strtok(NULL, " \t,");
            }
        }
    }
    fclose(f);
}

static void load_services(int only_enabled)
{
    nlist = 0;
    DIR *d = opendir(SVC_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nlist < MAX_SVC) {
        if (e->d_name[0] == '.')
            continue;
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s/control", SVC_DIR, e->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (only_enabled && !is_enabled(e->d_name))
            continue;
        svc_t *s = &list[nlist];
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name), "%s", e->d_name);
        snprintf(s->desc, sizeof(s->desc), "%s", e->d_name);
        parse_meta(s);
        nlist++;
    }
    closedir(d);
}

/* live pid: pidfile first, then /proc comm match */
static int find_pid(const svc_t *s)
{
    if (s->pidfile[0] && file_exists(s->pidfile)) {
        int pid = read_int_file(s->pidfile);
        if (pid > 0 && kill(pid, 0) == 0)
            return pid;
    }
    if (s->oneshot)
        return 0;
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[64], comm[128] = "";
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        fgets(comm, sizeof(comm), f);
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (strcmp(comm, s->name) != 0)
            continue;
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (f) {
            char st[64] = "";
            fscanf(f, "%63s %63s %c", st, st, &st[0]);
            fclose(f);
            if (st[0] == 'Z')
                continue;
        }
        found = atoi(e->d_name);
        break;
    }
    closedir(d);
    return found;
}

static int is_running(const char *name)
{
    int i = svc_index(name);
    if (i < 0)
        return 0;
    return find_pid(&list[i]) > 0;
}

/* ---------- execution ---------- */

static int run_control(const char *name, const char *action, int timeout_s,
                       int log_output)
{
    char script[512], logpath[512];
    snprintf(script, sizeof(script), "%s/%s/control", SVC_DIR, name);
    snprintf(logpath, sizeof(logpath), "%s/%s.log", LOG_DIR, name);

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setsid();
        int fd = log_output ? open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644)
                            : open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            if (log_output)
                dprintf(fd, "--- %s %s at %s\n", name, action, now_stamp());
            if (fd > 2)
                close(fd);
        }
        char *argv[] = { "/bin/sh", (char *)script, (char *)action, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }

    time_t start = time(NULL);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (time(NULL) - start > timeout_s) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(50000);
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* ---------- topological ordering (deps first) ---------- */

static int  visit_mark[MAX_SVC];
static char order[MAX_SVC][64];
static int  norder;

static void visit(int i)
{
    if (visit_mark[i])
        return;
    visit_mark[i] = 1;
    for (int d = 0; d < list[i].ndeps; d++) {
        int j = svc_index(list[i].deps[d]);
        if (j >= 0)
            visit(j);
    }
    snprintf(order[norder], sizeof(order[0]), "%s", list[i].name);
    norder++;
}

static void build_order(void)
{
    norder = 0;
    memset(visit_mark, 0, sizeof(visit_mark));
    for (int i = 0; i < nlist; i++)
        visit(i);
}

/* ---------- per-service operations ---------- */

static void kill_supervisor(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.supervise", STATE_DIR, name);
    int sp = read_int_file(path);
    if (sp > 1) {
        kill(sp, SIGTERM);
        unlink(path);
    }
}

static int start_one(const char *name, int silent)
{
    int i = svc_index(name);
    if (i < 0) {
        printf(C_RED "bctl: unknown service '%s'\n" C_RESET, name);
        return -1;
    }

    if (is_running(name)) {
        write_state(name, "RUNNING pid=%d", find_pid(&list[i]));
        if (!silent)
            printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
                   "%-12s already running\n", name);
        return 0;
    }
    if (!silent)
        printf("  [ ... ] %-12s\n", name);

    time_t t0 = time(NULL);
    int rc = run_control(name, "start", START_TIMEOUT, 1);
    float dt = (float)(time(NULL) - t0);
    if (rc == 0) {
        if (list[i].oneshot) {
            write_state(name, "DONE rc=0");
        } else {
            write_state(name, "RUNNING pid=%d", find_pid(&list[i]));
            /* spawn respawn monitor only after the daemon is up */
            if (list[i].respawn) {
                pid_t pid = fork();
                if (pid == 0) {
                    setsid();
                    int fd = open("/dev/null", O_RDWR);
                    if (fd >= 0) {
                        dup2(fd, 0);
                        dup2(fd, 1);
                        dup2(fd, 2);
                        if (fd > 2)
                            close(fd);
                    }
                    char *argv[] = { "/sbin/bctl", "supervise", (char *)name,
                                     NULL };
                    execv(argv[0], argv);
                    _exit(127);
                }
            }
        }
        if (!silent)
            printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
                   "%-12s (%.1fs)\n", name, dt);
        boot_log("%s up (%.1fs)\n", name, dt);
        return 0;
    }
    write_state(name, "FAILED rc=%d", rc);
    if (!silent)
        printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
               "%-12s failed (rc=%d, %.1fs)\n", name, rc, dt);
    boot_log("%s FAILED rc=%d\n", name, rc);
    return rc;
}

static int stop_one(const char *name)
{
    int i = svc_index(name);
    if (i < 0) {
        printf(C_RED "bctl: unknown service '%s'\n" C_RESET, name);
        return -1;
    }
    write_state(name, "STOPPING");
    kill_supervisor(name);
    int rc = run_control(name, "stop", STOP_TIMEOUT, 0);
    for (int t = 0; t < 10; t++) {
        int pid = find_pid(&list[i]);
        if (!pid)
            break;
        kill(pid, SIGTERM);
        usleep(300000);
    }
    write_state(name, "STOPPED rc=%d", rc);
    boot_log("%s stopped (rc=%d)\n", name, rc);
    return rc;
}

/* stop a service and everything that depends on it */
static void stop_dependents(const char *name)
{
    for (int i = 0; i < nlist; i++) {
        for (int d = 0; d < list[i].ndeps; d++) {
            if (strcmp(list[i].deps[d], name) == 0) {
                stop_dependents(list[i].name);
                stop_one(list[i].name);
                break;
            }
        }
    }
}

/* start a service after its dependencies (recursive) */
static void start_deps_recurse(int i)
{
    for (int d = 0; d < list[i].ndeps; d++) {
        int j = svc_index(list[i].deps[d]);
        if (j >= 0 && !is_running(list[j].name)) {
            start_deps_recurse(j);
            start_one(list[j].name, 1);
        }
    }
}

/* ---------- commands ---------- */

static int cmd_boot(void)
{
    load_services(1); /* enabled only */
    if (nlist == 0) {
        load_services(0);
        printf(C_YELLOW "  (no enabled services; starting none)\n" C_RESET);
        return 1;
    }
    printf(C_CYAN "  Starting services\n" C_RESET);
    build_order();
    int nfail = 0;
    for (int i = 0; i < norder; i++) {
        int rc = start_one(order[i], norder > 1 ? 0 : 0);
        if (rc != 0 && rc != 1)
            nfail++;
    }
    int ok = norder - nfail;
    boot_log("boot: %d ok, %d failed, %d services\n", ok, nfail, norder);
    printf(C_BOLD "  [ %s ] " C_RESET "boot complete: %d/%d services\n",
           nfail ? C_RED "!!" : C_GREEN "OK", ok, norder);
    return nfail ? 1 : 0;
}

static int cmd_shutdown(void)
{
    printf(C_YELLOW "  Stopping services\n" C_RESET);
    build_order();
    for (int i = norder - 1; i >= 0; i--)
        stop_one(order[i]);
    printf(C_BOLD "  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
           "all services stopped\n");
    return 0;
}

static int cmd_start(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl start <service>\n");
        return 2;
    }
    load_services(0);
    int i = svc_index(argv[0]);
    if (i < 0) {
        printf(C_RED "bctl: unknown service '%s'\n" C_RESET, argv[0]);
        return 1;
    }
    start_deps_recurse(i);
    int rc = start_one(argv[0], 0);
    return rc == 0 ? 0 : 1;
}

static int cmd_stop(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl stop <service>\n");
        return 2;
    }
    load_services(0);
    if (svc_index(argv[0]) < 0) {
        printf(C_RED "bctl: unknown service '%s'\n" C_RESET, argv[0]);
        return 1;
    }
    stop_dependents(argv[0]);
    stop_one(argv[0]);
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl restart <service>\n");
        return 2;
    }
    load_services(0);
    if (svc_index(argv[0]) < 0) {
        printf(C_RED "bctl: unknown service '%s'\n" C_RESET, argv[0]);
        return 1;
    }
    stop_dependents(argv[0]);
    stop_one(argv[0]);
    start_deps_recurse(svc_index(argv[0]));
    return start_one(argv[0], 0);
}

static int cmd_status(int argc, char **argv)
{
    load_services(0);
    if (argc >= 1) {
        int i = svc_index(argv[0]);
        if (i < 0) {
            printf(C_RED "bctl: unknown service '%s'\n" C_RESET, argv[0]);
            return 1;
        }
        int pid = find_pid(&list[i]);
        printf("%-12s %s %s\n", list[i].name,
               pid ? C_GREEN "[RUNNING]" C_RESET : C_RED "[STOPPED]" C_RESET,
               pid ? "yes" : "no");
        return pid ? 0 : 1;
    }
    printf(C_BOLD "  %-12s %-10s %-7s %-8s %s\n" C_RESET,
           "SERVICE", "STATE", "PID", "ENABLED", "DESCRIPTION");
    int all = 1;
    for (int i = 0; i < nlist; i++) {
        int pid = find_pid(&list[i]);
        int down = !pid && !list[i].oneshot;
        if (down)
            all = 0;
        printf("  %-12s %s %-7d %-8s %s\n",
               list[i].name,
               down ? C_RED "[STOPPED]" C_RESET : C_GREEN "[UP]" C_RESET,
               pid, is_enabled(list[i].name) ? "yes" : "no", list[i].desc);
    }
    return all ? 0 : 1;
}

static int cmd_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    load_services(0);
    printf(C_BOLD "  %-12s %-8s %s\n" C_RESET, "SERVICE", "ENABLED", "DESCRIPTION");
    for (int i = 0; i < nlist; i++) {
        printf("  %-12s %-8s %s\n", list[i].name,
               is_enabled(list[i].name) ? "yes" : "no", list[i].desc);
    }
    return 0;
}

static int cmd_enable(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl enable <service>\n");
        return 2;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ENABLED, argv[0]);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
    printf("%s enabled\n", argv[0]);
    return 0;
}

static int cmd_disable(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl disable <service>\n");
        return 2;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ENABLED, argv[0]);
    unlink(path);
    printf("%s disabled\n", argv[0]);
    return 0;
}

/* respawn monitor: restart the service whenever its daemon exits */
static int cmd_supervise(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl supervise <service>\n");
        return 2;
    }
    char debug_mode = 0;
    if (argc >= 2 && !strcmp(argv[1], "-d"))
        debug_mode = 1;
    const char *name = argv[0];
    if (!debug_mode) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
        }
    }
    char sppath[512];
    snprintf(sppath, sizeof(sppath), "%s/%s.supervise", STATE_DIR, name);
    int fd = open(sppath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
        write(fd, buf, n);
        close(fd);
    }

    int restarts = 0;
    time_t window_start = time(NULL);
    for (;;) {
        svc_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.name, sizeof(s.name), "%s", name);
        /* parse meta standalone (registry may be stale in our snapshot) */
        int i = svc_index(name);
        int running;
        if (i >= 0) {
            running = find_pid(&list[i]);
        } else {
            load_services(0);
            i = svc_index(name);
            if (i < 0)
                return 1;
            running = find_pid(&list[i]);
        }
        if (running) {
            sleep(2);
            continue;
        }
        if (file_exists(sppath) == 0)
            return 0; /* supervisor was told to stop */
        time_t now = time(NULL);
        if (now - window_start > 60) {
            restarts = 0;
            window_start = now;
        }
        if (restarts >= 5) {
            sleep(30);
            continue;
        }
        restarts++;
        boot_log("supervise: %s respawning #%d\n", name, restarts);
        run_control(name, "start", START_TIMEOUT, 1);
        sleep(3);
    }
    return 0;
}

/* ---------- diagnostics ---------- */

static void diag_mem(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return;
    char line[256];
    long total = -1, avail = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9))
            total = atol(line + 9);
        else if (!strncmp(line, "MemAvailable:", 13))
            avail = atol(line + 13);
    }
    fclose(f);
    if (total > 0)
        printf("  Memory:   %ld MB total, %ld MB available\n",
               total / 1024, avail > 0 ? avail / 1024 : -1);
}

static int cmd_diag(void)
{
    char line[256];
    printf(C_BOLD C_CYAN "\n  BulumOS diagnostics\n" C_RESET);

    FILE *f = fopen("/proc/version", "r");
    if (f) {
        fgets(line, sizeof(line), f);
        fclose(f);
        printf("  Kernel:   %s", line);
    }
    f = fopen("/proc/uptime", "r");
    long up = 0;
    if (f) {
        fscanf(f, "%ld", &up);
        fclose(f);
    }
    printf("  Uptime:   %ld min\n", up / 60);
    diag_mem();
    printf("\n");

    load_services(0);
    build_order();
    printf(C_BOLD "  Services %d\n\n" C_RESET, nlist);
    cmd_status(0, NULL);
    printf("\n");

    printf(C_BOLD "  Last boot log\n" C_RESET);
    char path[512];
    snprintf(path, sizeof(path), "%s/boot.log", LOG_DIR);
    f = fopen(path, "r");
    if (f) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f))
            printf("    %s", buf);
        fclose(f);
    } else {
        printf("    (no boot log yet)\n");
    }
    printf("\n");
    return 0;
}

/* show a service's log */
static int cmd_log(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bctl log <service>\n");
        return 2;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, argv[0]);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf(C_RED "bctl: no log for '%s'\n" C_RESET, argv[0]);
        return 1;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), f))
        printf("%s", buf);
    fclose(f);
    return 0;
}

/* ---------- main ---------- */

static void usage(const char *p)
{
    printf("usage: %s <command> [args]\n"
           "  boot            start all enabled services\n"
           "  shutdown        stop all enabled services\n"
           "  start <svc>     start a service\n"
           "  stop <svc>      stop a service\n"
           "  restart <svc>   restart a service\n"
           "  status [svc]    service status\n"
           "  list            known services\n"
           "  enable <svc>    enable at boot\n"
           "  disable <svc>   disable at boot\n"
           "  supervise <svc> respawn monitor (internal)\n"
           "  diagnose        system diagnostics\n"
           "  log [svc]       show a service log\n", p);
}

int main(int argc, char **argv)
{
    ensure_dirs();
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *cmd = argv[1];
    int rc;
    if (!strcmp(cmd, "boot"))
        rc = cmd_boot();
    else if (!strcmp(cmd, "shutdown"))
        rc = cmd_shutdown();
    else if (!strcmp(cmd, "start"))
        rc = cmd_start(argc - 2, argv + 2);
    else if (!strcmp(cmd, "stop"))
        rc = cmd_stop(argc - 2, argv + 2);
    else if (!strcmp(cmd, "restart"))
        rc = cmd_restart(argc - 2, argv + 2);
    else if (!strcmp(cmd, "status"))
        rc = cmd_status(argc - 2, argv + 2);
    else if (!strcmp(cmd, "list"))
        rc = cmd_list(0, NULL);
    else if (!strcmp(cmd, "enable"))
        rc = cmd_enable(argc - 2, argv + 2);
    else if (!strcmp(cmd, "disable"))
        rc = cmd_disable(argc - 2, argv + 2);
    else if (!strcmp(cmd, "supervise"))
        rc = cmd_supervise(argc - 2, argv + 2);
    else if (!strcmp(cmd, "diagnose"))
        rc = cmd_diag();
    else if (!strcmp(cmd, "log"))
        rc = cmd_log(argc - 2, argv + 2);
    else {
        usage(argv[0]);
        rc = 2;
    }
    return rc;
}