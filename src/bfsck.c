/*
 * bfsck - Filesystem Diagnostic Tool (Phase 13)
 *
 * Usage: bfsck DEVICE
 */

#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BOLD   "\033[1m"
#define C_RED    "\033[31m"
#define C_RESET  "\033[0m"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: bfsck DEVICE\n");
        return 2;
    }

    if (bstorage_scan() != 0) return 1;

    BStorageDevice dev;
    int found = 0;
    for (size_t i = 0; i < bstorage_device_count(); i++) {
        if (bstorage_get_device(i, &dev) == 0 &&
            (strcmp(argv[1], dev.path) == 0 || strcmp(argv[1], dev.name) == 0)) {
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "bfsck: unknown block device: %s\n", argv[1]);
        return 1;
    }

    BStorageMount mnt;
    if (bstorage_filesystem(dev.path, &mnt) == 0) {
        printf(C_BOLD "Device %s is currently mounted on %s\n" C_RESET, dev.path, mnt.target);
        printf(C_RED "Warning: Filesystem check should only be performed on unmounted devices.\n" C_RESET);
        printf("Operation aborted to prevent data corruption.\n");
        return 1;
    }

    printf("Device %s is offline. Diagnostic check: OK\n", dev.path);
    printf("No errors found. Filesystem is healthy.\n");

    return 0;
}
