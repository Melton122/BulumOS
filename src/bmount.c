/*
 * bmount - Mount Point Lister (Phase 13)
 *
 * Usage: bmount [list|info PATH|--json]
 */

#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

static int emit_json(void)
{
    printf("[");
    size_t count = bstorage_mount_count();
    for (size_t i = 0; i < count; i++) {
        BStorageMount mnt;
        if (bstorage_get_mount(i, &mnt) != 0) continue;
        if (i > 0) printf(",");
        printf("{\"source\":\"%s\",\"target\":\"%s\",\"filesystem\":\"%s\",\"options\":\"%s\",\"read_only\":%d}",
               mnt.source, mnt.target, mnt.filesystem, mnt.options, mnt.read_only);
    }
    printf("]\n");
    return 0;
}

static int emit_mount_info(const BStorageMount *mnt)
{
    printf(C_BOLD "Mount Point: %s" C_RESET "\n", mnt->target);
    printf("  Source:     %s\n", mnt->source);
    printf("  Filesystem: %s\n", mnt->filesystem);
    printf("  Options:    %s\n", mnt->options);
    printf("  Read-Only:  %s\n", mnt->read_only ? "Yes" : "No");
    return 0;
}

int main(int argc, char **argv)
{
    if (bstorage_scan() != 0) {
        fprintf(stderr, "bmount: failed to scan storage\n");
        return 1;
    }

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        printf(C_BOLD "%-20s %-20s %-10s %-10s\n" C_RESET,
               "SOURCE", "TARGET", "FS", "RO");
        printf("--------------------------------------------------------------------------\n");
        for (size_t i = 0; i < bstorage_mount_count(); i++) {
            BStorageMount mnt;
            if (bstorage_get_mount(i, &mnt) != 0) continue;
            printf("%-20s %-20s %-10s %-10s\n",
                   mnt.source, mnt.target, mnt.filesystem, mnt.read_only ? "Y" : "N");
        }
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--json") == 0)
        return emit_json();

    if (argc == 3 && strcmp(argv[1], "info") == 0) {
        BStorageMount mnt;
        if (bstorage_find_mount(argv[2], &mnt) == 0) {
            return emit_mount_info(&mnt);
        }
        fprintf(stderr, "bmount: mount point not found: %s\n", argv[2]);
        return 1;
    }

    fprintf(stderr, "usage: bmount [list|info PATH|--json]\n");
    return 2;
}
