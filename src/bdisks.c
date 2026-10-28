/*
 * bdisks - Block Device Lister (Phase 13)
 *
 * Usage: bdisks [list|info DEVICE|--json]
 */

#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

/* Simple human-readable bytes converter */
static void human_bytes(uint64_t bytes, char *buf, size_t sz)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit_idx = 0;
    double dbytes = (double)bytes;
    while (dbytes >= 1024 && unit_idx < 5) {
        dbytes /= 1024;
        unit_idx++;
    }
    snprintf(buf, sz, "%.1f %s", dbytes, units[unit_idx]);
}

static int emit_json(void)
{
    printf("{\"devices\":[");
    size_t count = bstorage_device_count();
    for (size_t i = 0; i < count; i++) {
        BStorageDevice dev;
        if (bstorage_get_device(i, &dev) != 0) continue;
        if (i > 0) printf(",");
        printf("{\"name\":\"%s\",\"path\":\"%s\",\"size\":%llu,\"is_partition\":%d,\"removable\":%d,\"read_only\":%d,\"rotational\":%d,\"mounted\":%d,\"filesystem\":\"%s\",\"mountpoint\":\"%s\"}",
               dev.name, dev.path, (unsigned long long)dev.size_bytes, dev.is_partition,
               dev.removable, dev.read_only, dev.rotational, dev.mounted, dev.filesystem, dev.mountpoint);
    }
    printf("],\"disk_count\":%llu,\"partition_count\":%llu}\n",
           (unsigned long long)bstorage_disk_count(),
           (unsigned long long)bstorage_partition_count());
    return 0;
}

static int emit_device(const BStorageDevice *dev)
{
    char size[32];
    human_bytes(dev->size_bytes, size, sizeof(size));
    printf(C_BOLD "Device: %s" C_RESET " (%s)\n", dev->path, dev->name);
    printf("  Size:       %s\n", size);
    printf("  Type:       %s\n", dev->is_partition ? "Partition" : "Disk");
    printf("  RO/Remov:   %s / %s\n", dev->read_only ? "Yes" : "No", dev->removable ? "Yes" : "No");
    printf("  Rotational:  %s\n", dev->rotational ? "Yes" : "No");
    if (dev->is_partition) {
        printf("  Parent:     %s\n", dev->parent);
        printf("  Number:     %u\n", dev->partition_number);
    }
    if (dev->mounted) {
        printf("  Mounted:    %s (%s)\n", dev->mountpoint, dev->filesystem);
    } else {
        printf("  Mounted:    (not mounted)\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (bstorage_scan() != 0) {
        fprintf(stderr, "bdisks: failed to scan storage\n");
        return 1;
    }

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        printf(C_BOLD "%-15s %-12s %-10s %-5s %-5s %-10s\n" C_RESET,
               "DEVICE", "SIZE", "TYPE", "RO", "RM", "MOUNT");
        printf("--------------------------------------------------------------------------\n");
        for (size_t i = 0; i < bstorage_device_count(); i++) {
            BStorageDevice dev;
            if (bstorage_get_device(i, &dev) != 0) continue;
            char size[32];
            human_bytes(dev.size_bytes, size, sizeof(size));
            printf("%-15s %-12s %-10s %-5s %-5s %-10s\n",
                   dev.path, size, dev->is_partition ? "part" : "disk",
                   dev->read_only ? "Y" : "N", dev->removable ? "Y" : "N",
                   dev->mounted ? dev->mountpoint : "");
        }
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--json") == 0)
        return emit_json();

    if (argc == 3 && strcmp(argv[1], "info") == 0) {
        for (size_t i = 0; i < bstorage_device_count(); i++) {
            BStorageDevice dev;
            if (bstorage_get_device(i, &dev) == 0 &&
                (strcmp(argv[2], dev.path) == 0 || strcmp(argv[2], dev.name) == 0)) {
                return emit_device(&dev);
            }
        }
        fprintf(stderr, "bdisks: device not found: %s\n", argv[2]);
        return 1;
    }

    fprintf(stderr, "usage: bdisks [list|info DEVICE|--json]\n");
    return 2;
}
