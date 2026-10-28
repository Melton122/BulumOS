/*
 * bformat - Filesystem Formatter (Phase 13 - Informational Only)
 *
 * Usage: bformat DEVICE
 */

#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_RED    "\033[31m"
#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: bformat DEVICE\n");
        return 2;
    }

    printf(C_RED C_BOLD "CRITICAL WARNING: Formatting will erase all data on %s\n" C_RESET, argv[1]);
    printf("BulumOS Phase 13 implementation: Formatting is currently NOT implemented.\n");
    printf("This tool exists for API completeness. Please use external tools for formatting.\n");

    return 1;
}
