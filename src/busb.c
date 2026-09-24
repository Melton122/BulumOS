/*
 * BulumOS USB tool — enumerate and inspect USB devices.
 * usage:
 *   busb list            list USB buses/devices (name, idVendor:idProduct)
 *   busb info DEV        show detailed sysfs info for a USB device node
 *   busb storage         list USB mass-storage devices and partitions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#define SYS_USB "/sys/bus/usb/devices"
#define SYS_USBDEV "/sys/bus/usb/devices"

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GRAY   "\033[90m"
#define C_GREEN  "\033[32m"

static char *readfile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n && buf[n-1] == '\n') buf[n-1] = 0;
    return strdup(buf);
}

static void print_dev(const char *dev, int verbose)
{
    char p[512];
    const char *name = NULL, *vid = NULL, *pid = NULL, *prod = NULL;

    snprintf(p, sizeof(p), "%s/%s/manufacturer", SYS_USBDEV, dev);
    name = readfile(p);
    snprintf(p, sizeof(p), "%s/%s/idVendor", SYS_USBDEV, dev);
    vid = readfile(p);
    snprintf(p, sizeof(p), "%s/%s/idProduct", SYS_USBDEV, dev);
    pid = readfile(p);
    snprintf(p, sizeof(p), "%s/%s/product", SYS_USBDEV, dev);
    prod = readfile(p);

    printf("%-8s ", dev);
    if (name) printf("%s", name);
    else printf("(unknown)");
    if (vid && pid) printf(" [%s:%s]", vid, pid);
    if (prod && verbose) printf("  (%s)", prod);
    printf("\n");

    free((void*)name); free((void*)vid); free((void*)pid); free((void*)prod);
}

static int cmd_list(int verbose)
{
    DIR *d = opendir(SYS_USB);
    if (!d) {
        fprintf(stderr, "busb: cannot open %s (no USB subsystem?)\n", SYS_USB);
        return 1;
    }
    printf(C_BOLD "USB devices:" C_RESET "\n");
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        print_dev(e->d_name, verbose);
    }
    closedir(d);
    return 0;
}

static int cmd_storage(void)
{
    /* list block devices under /sys/block and their removable/uevent */
    struct dirent *e;
    DIR *d = opendir("/sys/block");
    if (!d) return 1;
    printf(C_BOLD "block devices:" C_RESET "\n");
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[512], rm[8]= "?", size[32] = "?";
        snprintf(p, sizeof(p), "/sys/block/%s/removable", e->d_name);
        char *r = readfile(p);
        if (r) { strncpy(rm, r, 3); free(r); }
        snprintf(p, sizeof(p), "/sys/block/%s/size", e->d_name);
        char *s = readfile(p);
        if (s) { snprintf(size, sizeof(size), "%s", s); free(s); }
        int removable = (rm[0] == '1');
        printf("  %-8s removable=%s size-blocks=%s%s\n",
               e->d_name, rm,
               (strcmp(size, "?") ? size : "?"),
               removable ? C_GREEN " (USB/removable)" C_RESET : "");
    }
    closedir(d);
    return 0;
}

static int cmd_info(const char *dev)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", SYS_USBDEV, dev);
    if (access(p, F_OK) != 0) {
        fprintf(stderr, "busb: no such USB device '%s'\n", dev);
        return 1;
    }
    printf(C_BOLD "USB device %s:" C_RESET "\n", dev);
    const char *keys[] = {
        "idVendor", "idProduct", "manufacturer", "product",
        "serial", "bcdDevice", "bDeviceClass", "bDeviceSubClass",
        "bMaxPacketSize0", "speed", "busnum", "devnum",
        "driver",   /* symlink; skip */
        NULL
    };
    for (int i = 0; keys[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s/%s", SYS_USBDEV, dev, keys[i]);
        char *v = readfile(p);
        if (v) {
            printf("  %-16s %s\n", keys[i], v);
            free(v);
        }
    }
    return 0;
}

static void usage(void)
{
    printf("usage: busb [list|storage|info DEV]\n"
           "  list        list USB devices\n"
           "  info DEV    show sysfs info for USB device node (e.g. 1-1)\n"
           "  storage     list block devices (removable = USB)\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "list";
    if (strcmp(cmd, "list") == 0)        return cmd_list(argc > 2);
    if (strcmp(cmd, "storage") == 0)     return cmd_storage();
    if (strcmp(cmd, "info") == 0)        return argc > 2 ? cmd_info(argv[2]) : (usage(), 2);
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(); return 0;
    }
    fprintf(stderr, "busb: unknown command '%s'\n", cmd);
    usage();
    return 2;
}
