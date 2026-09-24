/*
 * bnetwork - BulumOS network hardware report (Phase 11)
 *
 * usage:
 *   bnetwork            list interfaces with MAC/state/statistics
 *   bnetwork ip         show current IPv4 addresses
 *   bnetwork --help     usage
 *   bnetwork --version  version
 */

#include "bhw.h"

#include <stdio.h>
#include <string.h>

#define C_BOLD  "\033[1m"
#define C_RESET "\033[0m"

static void list_ifaces(void)
{
    struct bhw_netif nif[8];
    int n = bhw_net_list(nif, 8);
    int i;

    printf(C_BOLD "Network interfaces\n" C_RESET);
    printf("-------------------\n");
    if (n == 0) {
        printf("(none)\n");
        return;
    }
    for (i = 0; i < n; i++) {
        printf("%-8s  %-4s  MAC %-17s\n", nif[i].name,
               nif[i].up ? "up" : "down", nif[i].mac);
        printf("          rx=%lu tx=%lu bytes\n", nif[i].rx_bytes,
               nif[i].tx_bytes);
    }
}

static void show_ip(void)
{
    /* Parse /proc/net/fib_trie for IPv4 addresses assigned locally. */
    FILE *f = fopen("/proc/net/fib_trie", "r");
    if (!f) {
        printf("(cannot read fib_trie)\n");
        return;
    }
    char line[256];
    char lastip[64] = "";
    int shown = 0;

    printf(C_BOLD "IPv4 addresses\n" C_RESET);
    printf("---------------\n");
    while (fgets(line, sizeof(line), f)) {
        if (strchr(line, '|')) {
            char ip[64];
            if (sscanf(line, " %63[0-9.]", ip) == 1) {
                if (strcmp(ip, lastip) != 0 && strcmp(ip, "0.0.0.0") != 0) {
                    printf("  %s\n", ip);
                    shown++;
                    snprintf(lastip, sizeof(lastip), "%s", ip);
                }
            }
        }
    }
    fclose(f);
    if (!shown)
        printf("  (none)\n");
}

int main(int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : "";

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        printf("usage: bnetwork [ifaces|ip] [--help] [--version]\n"
               "Reports BulumOS network hardware.\n");
        return 0;
    }
    if (strcmp(arg, "--version") == 0) {
        printf("bnetwork %s (BulumOS Phase 11)\n", BHW_VERSION);
        return 0;
    }
    if (argc == 1 || strcmp(arg, "ifaces") == 0) {
        list_ifaces();
        if (argc == 1) {
            printf("\n");
            show_ip();
        }
        return 0;
    }
    if (strcmp(arg, "ip") == 0) {
        show_ip();
        return 0;
    }
    printf("usage: bnetwork [ifaces|ip] [--help] [--version]\n");
    return 2;
}
