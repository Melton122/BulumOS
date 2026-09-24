/*
 * BulumOS time tool — show/set time, NTP sync, hwclock, timezone.
 * usage: btime [show|set|sync|hwclock|tz|status]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_CYAN   "\033[36m"
#define C_GRAY   "\033[90m"

static const char *TZ_FILE  = "/etc/TZ";
static const char *NTP_CONF = "/etc/ntp.conf";

static const char *path(const char *env, const char *def) {
    const char *v = getenv(env);
    return (v && *v) ? v : def;
}

static void cmd_show(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", t);
    printf(C_BOLD "Current time:" C_RESET " %s\n", buf);

    const char *tz = getenv("TZ");
    if (!tz) {
        FILE *f = fopen(path("BULUM_TZ_FILE", TZ_FILE), "r");
        if (f) {
            static char tzbuf[64];
            if (fgets(tzbuf, sizeof(tzbuf), f)) {
                tzbuf[strcspn(tzbuf, "\n")] = 0;
                tz = tzbuf;
            }
            fclose(f);
        }
    }
    if (tz && *tz)
        printf(C_GRAY "Timezone:" C_RESET "     %s\n", tz);
    else
        printf(C_GRAY "Timezone:" C_RESET "     UTC\n");
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: btime set YYYY-MM-DD HH:MM[:SS]\n");
        return 2;
    }
    struct tm t = {0};
    if (sscanf(argv[0], "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) {
        fprintf(stderr, "btime: bad date format (expected YYYY-MM-DD)\n");
        return 2;
    }
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    if (argc < 2 || sscanf(argv[1], "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec) < 2) {
        fprintf(stderr, "btime: bad time format (expected HH:MM[:SS])\n");
        return 2;
    }
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch < 0) {
        fprintf(stderr, "btime: invalid date/time\n");
        return 1;
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) < 0) {
        perror("settimeofday");
        return 1;
    }
    printf(C_GREEN "[ OK ]" C_RESET " time set to %s %s\n", argv[0], argv[1]);
    return 0;
}

static int cmd_sync(void)
{
    if (access("/sbin/ntpd", X_OK) == 0 || access("/usr/sbin/ntpd", X_OK) == 0) {
        const char *ntpd = access("/sbin/ntpd", X_OK) == 0 ? "/sbin/ntpd" : "/usr/sbin/ntpd";
        printf("Syncing time via NTP (using %s)...\n", ntpd);
        pid_t pid = fork();
        if (pid == 0) {
            execl(ntpd, "ntpd", "-q", "-p", path("BULUM_NTP_CONF", NTP_CONF), NULL);
            _exit(127);
        }
        if (pid > 0) {
            int st;
            waitpid(pid, &st, 0);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                printf(C_GREEN "[ OK ]" C_RESET " NTP sync complete\n");
                return 0;
            }
        }
        fprintf(stderr, C_YELLOW "[WARN]" C_RESET " NTP sync failed (network may be unavailable)\n");
        return 1;
    }
    fprintf(stderr, C_YELLOW "[WARN]" C_RESET " ntpd not found\n");
    return 1;
}

static int cmd_hwclock(int argc, char **argv)
{
    int show = (argc < 1 || strcmp(argv[0], "show") == 0);
    int set  = (argc >= 1 && strcmp(argv[0], "set") == 0);

    if (show) {
        pid_t pid = fork();
        if (pid == 0) { execl("/sbin/hwclock", "hwclock", "--show", NULL); _exit(127); }
        if (pid > 0) { int st; waitpid(pid, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : 1; }
        return 1;
    }
    if (set) {
        pid_t pid = fork();
        if (pid == 0) { execl("/sbin/hwclock", "hwclock", "--systohc", NULL); _exit(127); }
        if (pid > 0) {
            int st; waitpid(pid, &st, 0);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                printf(C_GREEN "[ OK ]" C_RESET " hardware clock set from system clock\n");
                return 0;
            }
        }
        fprintf(stderr, "hwclock: failed to set hardware clock\n");
        return 1;
    }
    fprintf(stderr, "usage: btime hwclock [show|set]\n");
    return 2;
}

static int cmd_tz(int argc, char **argv)
{
    if (argc < 1) {
        FILE *f = fopen(path("BULUM_TZ_FILE", TZ_FILE), "r");
        char buf[64] = "UTC";
        if (f) {
            if (fgets(buf, sizeof(buf), f))
                buf[strcspn(buf, "\n")] = 0;
            fclose(f);
        }
        printf(C_GRAY "Timezone:" C_RESET " %s\n", buf);
        return 0;
    }
    const char *newtz = argv[0];
    FILE *f = fopen(path("BULUM_TZ_FILE", TZ_FILE), "w");
    if (!f) { perror("TZ"); return 1; }
    fprintf(f, "%s\n", newtz);
    fclose(f);
    setenv("TZ", newtz, 1);
    printf(C_GREEN "[ OK ]" C_RESET " timezone set to %s\n", newtz);
    return 0;
}

static void cmd_status(void)
{
    cmd_show();
    printf("\n");
    pid_t pid = fork();
    if (pid == 0) { execl("/sbin/hwclock", "hwclock", "--show", NULL); _exit(127); }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }

    FILE *f = fopen(path("BULUM_NTP_PID", "/var/run/ntpd.pid"), "r");
    if (f) {
        char buf[16];
        if (fgets(buf, sizeof(buf), f)) {
            pid_t ntp_pid = (pid_t)atoi(buf);
            if (ntp_pid > 0 && kill(ntp_pid, 0) == 0)
                printf(C_GREEN "[ OK ]" C_RESET " ntpd running (pid %d)\n", ntp_pid);
        }
        fclose(f);
    } else {
        printf(C_GRAY "[ -- ]" C_RESET " ntpd not running\n");
    }

    f = fopen(path("BULUM_NTP_CONF", NTP_CONF), "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            if (line[0] && line[0] != '#')
                printf(C_GRAY "  NTP:" C_RESET " %s\n", line);
        }
        fclose(f);
    }
}

static void usage(void)
{
    printf("usage: btime [show|set|sync|hwclock|tz|status]\n"
           "  (no args)  show current time\n"
           "  set  DATE TIME   set system time (YYYY-MM-DD HH:MM)\n"
           "  sync            NTP time sync\n"
           "  hwclock [show|set]  hardware clock\n"
           "  tz [TIMEZONE]   show/set timezone (e.g. UTC, US/Eastern)\n"
           "  status          full time status\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "show";
    char **args = argv + 2;
    int nargs = argc - 2;

    if (strcmp(cmd, "show") == 0 || argc == 1) { cmd_show(); return 0; }
    if (strcmp(cmd, "set") == 0)      return cmd_set(nargs, args);
    if (strcmp(cmd, "sync") == 0)     return cmd_sync();
    if (strcmp(cmd, "hwclock") == 0)  return cmd_hwclock(nargs, args);
    if (strcmp(cmd, "tz") == 0)       return cmd_tz(nargs, args);
    if (strcmp(cmd, "status") == 0)   { cmd_status(); return 0; }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(); return 0;
    }
    fprintf(stderr, "btime: unknown command '%s'\n", cmd);
    usage();
    return 2;
}
