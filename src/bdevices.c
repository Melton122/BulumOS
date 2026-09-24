/*
 * bdevices - BulumOS device enumeration (Phase 11)
 *
 * usage:
 *   bdevices            list PCI + block + net + input devices
 *   bdevices pci        PCI devices
 *   bdevices block      block devices
 *   bdevices net        network interfaces
 *   bdevices input      input devices
 *   bdevices --help     usage
 *   bdevices --version  version
 */

#include "bhw.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define C_BOLD  "\033[1m"
#define C_RESET "\033[0m"

static void list_pci(void)
{
    struct bhw_pci p[32];
    int n = bhw_pci_list(p, 32);
    printf(C_BOLD "PCI devices\n" C_RESET);
    if (n == 0) {
        printf("  (none)\n");
        return;
    }
    int i;
    for (i = 0; i < n; i++) {
        printf("  %-14s class=%s vendor=%s device=%s\n",
               p[i].slot, p[i].cls[0] ? p[i].cls : "?",
               p[i].vendor, p[i].device);
    }
}

static void list_block(void)
{
    char names[16][BHW_MAX_STR];
    int n = bhw_block_names(names, 16);
    int i;
    printf(C_BOLD "Block devices\n" C_RESET);
    if (n == 0) {
        printf("  (none)\n");
        return;
    }
    for (i = 0; i < n; i++)
        printf("  /dev/%s\n", names[i]);
}

static void list_net(void)
{
    struct bhw_netif nif[8];
    int n = bhw_net_list(nif, 8);
    int i;
    printf(C_BOLD "Network interfaces\n" C_RESET);
    if (n == 0) {
        printf("  (none)\n");
        return;
    }
    for (i = 0; i < n; i++) {
        printf("  %-8s %s  %-17s rx=%lu tx=%lu\n", nif[i].name,
               nif[i].up ? "up" : "down", nif[i].mac, nif[i].rx_bytes,
               nif[i].tx_bytes);
    }
}

static void list_input(void)
{
    DIR *d = opendir("/dev/input");
    printf(C_BOLD "Input devices\n" C_RESET);
    if (!d) {
        printf("  (none)\n");
        return;
    }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[320], name[128] = "unknown";
        snprintf(path, sizeof(path), "/sys/class/input/%s/device/name",
                 e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(name, sizeof(name), f))
                name[strcspn(name, "\n")] = 0;
            fclose(f);
        }
        printf("  /dev/input/%s  %s\n", e->d_name, name);
        n++;
    }
    closedir(d);
    if (n == 0)
        printf("  (none)\n");
}

int main(int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : "";
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        printf("usage: bdevices [pci|block|net|input] [--help] [--version]\n"
               "Lists BulumOS hardware devices.\n");
        return 0;
    }
    if (strcmp(arg, "--version") == 0) {
        printf("bdevices %s (BulumOS Phase 11)\n", BHW_VERSION);
        return 0;
    }
    if (argc == 1) {
        list_pci(); printf("\n");
        list_block(); printf("\n");
        list_net(); printf("\n");
        list_input();
        return 0;
    }
    if (strcmp(arg, "pci") == 0) { list_pci(); return 0; }
    if (strcmp(arg, "block") == 0) { list_block(); return 0; }
    if (strcmp(arg, "net") == 0) { list_net(); return 0; }
    if (strcmp(arg, "input") == 0) { list_input(); return 0; }
    printf("usage: bdevices [pci|block|net|input] [--help] [--version]\n");
    return 2;
}
