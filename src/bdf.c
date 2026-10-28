/*
 * bdf - Disk Free utility (Phase 13)
 *
 * Usage: bdf [PATH|--json]
 */

#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

static void human_bytes(uint64_t bytes, char *buf, size_t sz)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double dbytes = (double)bytes;
    while (dbytes >= 1024 && unit_idx < 4) {
        dbytes /= 1024;
        unit_idx++;
    }
    snprintf(buf, sz, "%.1f %s", dbytes, units[unit_idx]);
}

static int emit_json(void)
{
    printf("[");
    size_t count = bstorage_mount_count();
    for (size_t i = 0; i < count; i++) {
        BStorageMount mnt;
        BStorageUsage usage;
        if (bstorage_get_mount(i, &mnt) != 0) continue;
        if (bstorage_usage(mnt.target, &usage) != 0) continue;
        if (i > 0) printf(",");
        printf("{\"source\":\"%s\",\"target\":\"%s\",\"total\":%llu,\"used\":%llu,\"available\":%llu}",
               mnt.source, mnt.target, (unsigned long long)usage.total_bytes,
               (unsigned long long)usage.used_bytes, (unsigned long long)usage.available_bytes);
    }
    printf("]\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (bstorage_scan() != 0) {
        fprintf(stderr, "bdf: failed to scan storage\n");
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "--json") == 0)
        return emit_json();

    if (argc == 2) {
        BStorageUsage usage;
        if (bstorage_usage(argv[1], &usage) != 0) {
            fprintf(stderr, "bdf: cannot get usage for %s\n", argv[1]);
            return 1;
        }
        char total[32], used[32], avail[32];
        human_bytes(usage.total_bytes, total, sizeof(total));
        human_bytes(usage.used_bytes, used, sizeof(used));
        human_bytes(usage.available_bytes, avail, sizeof(avail));
        printf(C_BOLD "Path: %s\n" C_RESET, argv[1]);
        printf("Total:     %s\n", total);
        printf("Used:      %s\n", used);
        printf("Available: %s\n", avail);
        return 0;
    }

    printf(C_BOLD "%-20s %-10s %-10s %-10s %-5s %s\n" C_RESET,
           "FILESYSTEM", "SIZE", "USED", "AVAIL", "USE%", "MOUNTED ON");
    printf("----------------------------------------------------------------------------------\n");
    for (size_t i = 0; i < bstorage_mount_count(); i++) {
        BStorageMount mnt;
        BStorageUsage usage;
        if (bstorage_get_mount(i, &mnt) != 0) continue;
        if (bstorage_usage(mnt.target, &usage) != 0) continue;
        
        char total[32], used[32], avail[32];
        human_bytes(usage.total_bytes, total, sizeof(total));
        human_bytes(usage.used_bytes, used, sizeof(used));
        human_bytes(usage.available_bytes, avail, sizeof(avail));
        
        int pct = 0;
        if (usage.total_bytes > 0)
            pct = (int)((usage.used_bytes * 100) / usage.total_bytes);

        printf("%-20s %-10s %-10s %-10s %-5d%% %s\n",
               mnt.source, total, used, avail, pct, mnt.target);
    }

    return 0;
}
