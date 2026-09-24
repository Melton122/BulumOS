/*
 * bhardware - BulumOS hardware report (Phase 11)
 *
 * usage:
 *   bhardware           full report (CPU, RAM, GPU, storage, net, USB, audio, power)
 *   bhardware --cpu     CPU summary only
 *   bhardware --gpu     GPU only
 *   bhardware --help    usage
 *   bhardware --version version
 */

#include "bhw.h"

#include <stdio.h>
#include <string.h>

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_CYAN   "\033[36m"
#define C_GRAY   "\033[90m"
#define C_GREEN  "\033[32m"

static void mem_human(unsigned long kb, char *out, size_t sz)
{
    double m = 0;
    const char *u = "KB";
    if (kb >= 1024 * 1024) { m = kb / 1024.0 / 1024.0; u = "GB"; }
    else if (kb >= 1024) { m = kb / 1024.0; u = "MB"; }
    else m = kb;
    snprintf(out, sz, "%.1f %s", m, u);
}

static void report_cpu(void)
{
    struct bhw_cpu c;
    bhw_cpu_info(&c);
    printf("CPU\n");
    printf("  Architecture : %s\n", c.arch[0] ? c.arch : "(unknown)");
    printf("  Model        : %s\n", c.model[0] ? c.model : "(unknown)");
    printf("  Cores        : %d (online %d)\n", c.cores, c.online);
}

static void report_mem(void)
{
    struct bhw_mem m;
    bhw_mem_info(&m);
    char t[64], f[64];
    mem_human(m.total_kb, t, sizeof(t));
    mem_human(m.free_kb, f, sizeof(f));
    printf("Memory\n");
    printf("  Total        : %s\n", t);
    printf("  Free         : %s\n", f);
    if (m.mem_available) {
        char a[64];
        mem_human(m.avail_kb, a, sizeof(a));
        printf("  Available    : %s\n", a);
    }
}

static void report_gpu(void)
{
    struct bhw_gpu_list gl;
    bhw_gpu_detect(&gl);
    printf("GPU\n");
    if (gl.count == 0) {
        printf("  Detection    : none (headless)\n");
    } else {
        int i;
        for (i = 0; i < gl.count; i++) {
            struct bhw_gpu *s = &gl.items[i];
            printf("  Card %d       : %s\n", i + 1, s->name[0] ? s->name : "(unnamed)");
            if (s->driver[0])
                printf("    Driver     : %s\n", s->driver);
            if (s->width > 0)
                printf("    Resolution : %dx%d\n", s->width, s->height);
            if (s->fb_path[0])
                printf("    Framebuffer: %s\n", s->fb_path);
        }
    }
    bhw_gpu_free(&gl);
}

static void report_storage(void)
{
    char names[16][BHW_MAX_STR];
    int n = bhw_block_names(names, 16);
    printf("Storage\n");
    if (n == 0) {
        printf("  Devices      : none\n");
        return;
    }
    printf("  Devices      :\n");
    int i;
    for (i = 0; i < n; i++) {
        printf("    /dev/%s\n", names[i]);
    }
}

static void report_net(void)
{
    struct bhw_netif nif[8];
    int n = bhw_net_list(nif, 8);
    printf("Network\n");
    if (n == 0) {
        printf("  Interfaces   : none\n");
        return;
    }
    int i;
    for (i = 0; i < n; i++) {
        printf("  %-8s : %s (%s)\n", nif[i].name,
               nif[i].up ? "up" : "down", nif[i].mac[0] ? nif[i].mac : "no-mac");
    }
}

static void report_usb(void)
{
    printf("USB\n");
    printf("  Devices      : %d\n", bhw_usb_count());
}

static void report_audio(void)
{
    char cards[8][BHW_MAX_STR];
    int n = bhw_audio_cards(cards, 8);
    printf("Audio\n");
    if (n == 0) {
        printf("  Cards        : none\n");
    } else {
        int i;
        for (i = 0; i < n; i++)
            printf("  Card         : %s\n", cards[i]);
    }
}

static void report_power(void)
{
    int level, ac;
    printf("Power\n");
    if (bhw_power_battery(&level, &ac)) {
        if (level >= 0)
            printf("  Battery      : %d%%\n", level);
        if (ac >= 0)
            printf("  AC power     : %s\n", ac ? "connected" : "disconnected");
    } else {
        printf("  Battery      : none (AC powered VM)\n");
    }
}

static int report(void)
{
    printf(C_BOLD C_CYAN "BulumOS Hardware\n" C_RESET
           C_GRAY "=====================\n\n" C_RESET);
    report_cpu();
    printf("\n");
    report_mem();
    printf("\n");
    report_gpu();
    printf("\n");
    report_storage();
    printf("\n");
    report_net();
    printf("\n");
    report_usb();
    printf("\n");
    report_audio();
    printf("\n");
    report_power();
    printf("\n");

    /* Kernel framework line */
    char klines[8][BHW_MAX_STR];
    int kn = bhw_bulum_hardware(klines, 8);
    if (kn > 0) {
        int i;
        printf(C_BOLD "Kernel framework (/proc/bulum)\n" C_RESET);
        for (i = 0; i < kn; i++)
            printf("  %s\n", klines[i]);
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        return report();
    }
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("usage: bhardware [--cpu] [--gpu] [--help] [--version]\n"
                   "Reports BulumOS hardware configuration.\n");
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("bhardware %s (BulumOS Phase 11)\n", BHW_VERSION);
            return 0;
        }
        if (strcmp(argv[1], "--cpu") == 0) { report_cpu(); return 0; }
        if (strcmp(argv[1], "--gpu") == 0) { report_gpu(); return 0; }
        printf("usage: bhardware [--cpu] [--gpu] [--help] [--version]\n");
        return 2;
    }
    printf("usage: bhardware [--cpu] [--gpu] [--help] [--version]\n");
    return 2;
}
