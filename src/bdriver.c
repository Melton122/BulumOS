/*
 * bdriver - BulumOS driver registry report (Phase 11)
 *
 * Reports the BulumOS driver framework (/proc/bulum/drivers) populated by
 * the in-kernel bulum driver subsystem.  On a stock kernel without the
 * framework it reports the framework is not loaded.
 *
 *   bdriver             list registered BulumOS drivers
 *   bdriver info        show which are present/bound
 *   bdriver --help      usage
 *   bdriver --version   version
 */

#include "bhw.h"

#include <stdio.h>
#include <string.h>

#define C_BOLD  "\033[1m"
#define C_RESET "\033[0m"
#define C_GRAY  "\033[90m"

int main(int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : "";
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        printf("usage: bdriver [info] [--help] [--version]\n"
               "Reports the BulumOS kernel driver registry.\n");
        return 0;
    }
    if (strcmp(arg, "--version") == 0) {
        printf("bdriver %s (BulumOS Phase 11)\n", BHW_VERSION);
        return 0;
    }

    char lines[32][BHW_MAX_STR];
    int n = bhw_bulum_drivers(lines, 32);
    printf(C_BOLD "BulumOS driver registry\n" C_RESET);
    printf("------------------------\n");
    if (n == 0) {
        printf("(no BulumOS drivers registered)\n");
        printf(C_GRAY "  The bulum kernel driver framework is not present in\n"
               "  this running kernel. This is expected on a stock kernel;\n"
               "  the framework is built in for BulumOS kernels.\n" C_RESET);
        return 0;
    }
    int i;
    for (i = 0; i < n; i++)
        printf("  %s\n", lines[i]);
    return 0;
}
