/*
 * bpower - BulumOS power manager (Phase 11)
 *
 * usage:
 *   bpower               show power/battery status
 *   bpower shutdown      power the system off
 *   bpower reboot        reboot the system
 *   bpower status        battery + AC status
 *   bpower --help        usage
 *   bpower --version     version
 */

#include "bhw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

#define C_BOLD  "\033[1m"
#define C_RESET "\033[0m"

static void show_status(void)
{
    int level, ac;

    printf(C_BOLD "Power status\n" C_RESET);
    printf("------------\n");
    if (bhw_power_battery(&level, &ac)) {
        if (level >= 0)
            printf("  Battery      : %d%%\n", level);
        else
            printf("  Battery      : present (no capacity node)\n");
        if (ac >= 0)
            printf("  AC power     : %s\n", ac ? "connected" : "disconnected");
        else
            printf("  AC power     : (not exposed)\n");
    } else {
        printf("  Battery      : none\n");
        printf("  AC power     : (AC-powered system)\n");
    }
}

int main(int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : "";

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        printf("usage: bpower [shutdown|reboot] [--help] [--version]\n"
               "Reports power status and controls shut down/reboot.\n");
        return 0;
    }
    if (strcmp(arg, "--version") == 0) {
        printf("bpower %s (BulumOS Phase 11)\n", BHW_VERSION);
        return 0;
    }
    if (argc == 1 || strcmp(arg, "status") == 0) {
        show_status();
        return 0;
    }
    if (strcmp(arg, "shutdown") == 0) {
        printf("Power down...\n");
        sync();
        reboot(RB_POWER_OFF);
        /* not reached on success */
        printf("reboot() failed (permission?)\n");
        return 1;
    }
    if (strcmp(arg, "reboot") == 0) {
        printf("Rebooting...\n");
        sync();
        reboot(RB_AUTOBOOT);
        printf("reboot() failed (permission?)\n");
        return 1;
    }
    printf("usage: bpower [shutdown|reboot] [--help] [--version]\n");
    return 2;
}
