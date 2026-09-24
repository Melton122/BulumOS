/*
 * BulumOS status tool - report kernel, memory, services.
 * Scans /proc for known daemons and reports OK/DOWN.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_GREEN "\033[32m"
#define C_RED "\033[31m"
#define C_CYAN "\033[36m"
#define C_GRAY "\033[90m"

struct svc {
    const char *label;
    const char *comm; /* process comm name to probe */
};

static const struct svc services[] = {
    { "syslogd",   "syslogd"   },
    { "mdev",      "mdev"      },
    { "network",   "udhcpc"    },
    { "dropbear",  "dropbear"  },
    { "httpd",     "httpd"     },
    { "telnetd",   "telnetd"   },
    { "crond",     "crond"     },
    { "ntpd",      "ntpd"      },
    { NULL, NULL }
};

static int comm_running(const char *want)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[64], comm[64];
        snprintf(path, sizeof(path), "/proc/%.*s/comm", 10, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(comm, sizeof(comm), f))
            comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (strcmp(comm, want) == 0) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static long read_kv(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    char line[256];
    long v = -1;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            v = atol(line + strlen(key));
            break;
        }
    }
    fclose(f);
    return v;
}

static void mem_line(void)
{
    long total = read_kv("/proc/meminfo", "MemTotal:");
    long avail = read_kv("/proc/meminfo", "MemAvailable:");
    if (total > 0 && avail >= 0)
        printf(C_BOLD "  %-14s" C_RESET " %6ld MB total, %6ld MB available\n",
               "Memory", total / 1024, avail / 1024);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf(C_BOLD C_CYAN "\n  BulumOS status\n" C_RESET);

    char rel[64] = "";
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        if (!fgets(rel, sizeof(rel), f))
            rel[0] = 0;
        fclose(f);
    }
    long ups = 0;
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%ld", &ups) != 1)
            ups = 0;
        fclose(f);
    }
    printf(C_BOLD "  %-14s" C_RESET "  Linux %s\n", "Kernel", rel);
    printf(C_BOLD "  %-14s" C_RESET "  %ld min\n", "Uptime", ups / 60);
    mem_line();

    printf("\n" C_BOLD "  Service status\n" C_RESET);
    int all_ok = 1;
    for (const struct svc *s = services; s->label; s++) {
        int up = comm_running(s->comm);
        if (!up)
            all_ok = 0;
        printf("  %s %-12s\n",
               up ? C_GREEN "[OK]" C_RESET : C_RED "[DOWN]" C_RESET,
               s->label);
    }

    printf("\n" C_BOLD "  Network\n" C_RESET);
    int any_up = 0;
    DIR *nd = opendir("/sys/class/net");
    if (nd) {
        struct dirent *e;
        while ((e = readdir(nd))) {
            if (e->d_name[0] == '.')
                continue;
            char path[128];
            snprintf(path, sizeof(path), "/sys/class/net/%.*s/operstate",
                     15, e->d_name);
            FILE *f = fopen(path, "r");
            char op[16] = "unknown";
            if (f) {
                if (!fgets(op, sizeof(op), f))
                    op[0] = 0;
                fclose(f);
            }
            op[strcspn(op, "\r\n")] = 0;
            printf("  %-12s %s\n", e->d_name,
                   strcmp(op, "up") == 0 ? C_GREEN "[UP]" C_RESET
                                         : C_GRAY "[down]" C_RESET);
            if (strcmp(op, "up") == 0 && strcmp(e->d_name, "lo") != 0)
                any_up = 1;
        }
        closedir(nd);
    }
    /* first non-loopback IPv4 */
    f = fopen("/proc/net/route", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f)) {
                char iface[32] = "";
                unsigned long dest = 0, gw = 0;
                if (sscanf(line, "%31s %lx %lx", iface, &dest, &gw) != 3)
                    continue;
                if (dest == 0 && gw != 0) {
                    struct in_addr ga = { .s_addr = (uint32_t)gw };
                    char g[64];
                    inet_ntop(AF_INET, &ga, g, sizeof(g));
                    printf("  %-12s %s (%s)\n", "Gateway", g, iface);
                    break;
                }
            }
        }
        fclose(f);
    }
    f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[256];
        int shown = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "nameserver", 10) == 0) {
                line[strcspn(line, "\r\n")] = 0;
                if (!shown)
                    printf("  %-12s", "DNS");
                printf("%s%s", shown ? "," : " ", line + 11);
                shown = 1;
            }
        }
        fclose(f);
        if (shown)
            printf("\n");
        else
            printf("  %-12s (none configured)\n", "DNS");
    } else {
        printf("  %-12s (none configured)\n", "DNS");
    }
    printf("  %-12s %s\n", "Summary",
           any_up ? C_GREEN "network up" C_RESET : C_RED "network down" C_RESET);

    printf("\n");
    return all_ok ? 0 : 1;
}