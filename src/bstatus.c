/*
 * BulumOS status tool - report kernel, memory, services.
 * Scans /proc for known daemons and reports OK/DOWN.
 */

#define _GNU_SOURCE

#include "bstorage.h"

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

static int emit_storage_json(void)
{
    BStorageMount root;
    BStorageUsage usage;
    int has_root = bstorage_find_mount("/", &root) == 0 &&
                   bstorage_usage("/", &usage) == 0;
    printf("\"storage\":{\"disk_count\":%llu,\"mounted_filesystem_count\":%llu",
           (unsigned long long)bstorage_disk_count(),
           (unsigned long long)bstorage_mount_count());
    if (has_root) {
        printf(",\"root_source\":\"%s\",\"root_filesystem\":\"%s\",\"root_mountpoint\":\"%s\",\"root_total_bytes\":%llu,\"root_used_bytes\":%llu,"
               "\"root_available_bytes\":%llu,\"root_read_only\":%s",
               root.source, root.filesystem, root.target,
               (unsigned long long)usage.total_bytes,
               (unsigned long long)usage.used_bytes,
               (unsigned long long)usage.available_bytes,
               root.read_only ? "true" : "false");
    }
    printf("}");
    return has_root;
}

static int emit_json(void)
{
    char rel[64] = "";
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        if (!fgets(rel, sizeof(rel), f))
            rel[0] = 0;
        fclose(f);
        rel[strcspn(rel, "\r\n")] = 0;
    }
    long uptime = read_kv("/proc/uptime", "");
    if (uptime < 0) {
        f = fopen("/proc/uptime", "r");
        if (f) {
            double seconds = 0;
            if (fscanf(f, "%lf", &seconds) != 1)
                seconds = 0;
            fclose(f);
            uptime = (long)seconds;
        }
    }
    long total = read_kv("/proc/meminfo", "MemTotal:");
    long available = read_kv("/proc/meminfo", "MemAvailable:");
    printf("{\"kernel\":\"%s\",\"uptime_seconds\":%ld,\"memory_total_kb\":%ld,"
           "\"memory_available_kb\":%ld,\"services\":[",
           rel, uptime < 0 ? 0 : uptime, total < 0 ? 0 : total,
           available < 0 ? 0 : available);
    int first = 1;
    for (const struct svc *s = services; s->label; s++) {
        if (!first)
            putchar(',');
        first = 0;
        printf("{\"name\":\"%s\",\"running\":%s}", s->label, comm_running(s->comm) ? "true" : "false");
    }
    printf("],\"network\":[");
    DIR *nd = opendir("/sys/class/net");
    first = 1;
    if (nd) {
        struct dirent *entry;
        while ((entry = readdir(nd))) {
            if (entry->d_name[0] == '.')
                continue;
            char path[256], state[32] = "unknown";
            snprintf(path, sizeof(path), "/sys/class/net/%.*s/operstate",
                     (int)sizeof(path) / 2, entry->d_name);
            FILE *state_file = fopen(path, "r");
            if (state_file) {
                if (!fgets(state, sizeof(state), state_file))
                    state[0] = 0;
                fclose(state_file);
                state[strcspn(state, "\r\n")] = 0;
            }
            if (!first)
                putchar(',');
            first = 0;
            printf("{\"name\":\"%s\",\"state\":\"%s\"}", entry->d_name, state);
            putchar('}');
        }
        closedir(nd);
    }
    printf("],");
    emit_storage_json();
    puts("}");
    return 0;
}

static void storage_line(void)
{
    BStorageMount root;
    BStorageUsage usage;
    printf("\n" C_BOLD "  Storage\n" C_RESET);
    printf("  %-14s %llu disk(s), %llu mounted filesystem(s)\n", "Devices",
           (unsigned long long)bstorage_disk_count(),
           (unsigned long long)bstorage_mount_count());
    if (bstorage_find_mount("/", &root) != 0 || bstorage_usage("/", &usage) != 0) {
        printf("  %-14s unavailable\n", "Root filesystem");
        return;
    }
    char total[32], used[32], available[32];
    bstorage_human_bytes(usage.total_bytes, total, sizeof(total));
    bstorage_human_bytes(usage.used_bytes, used, sizeof(used));
    bstorage_human_bytes(usage.available_bytes, available, sizeof(available));
    printf("  %-14s %s\n", "Root source", root.source);
    printf("  %-14s %s\n", "Filesystem", root.filesystem);
    printf("  %-14s %s total, %s used, %s available\n", "Capacity",
           total, used, available);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json") != 0)) {
        fprintf(stderr, "usage: bstatus [--json]\n");
        return 2;
    }
    bstorage_scan();
    if (argc == 2)
        return emit_json();

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
    storage_line();

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
