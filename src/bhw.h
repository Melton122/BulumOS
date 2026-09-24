/*
 * bhw.h - BulumOS hardware API (Phase 11)
 *
 * A small, self-contained userspace library that aggregates hardware data
 * from sysfs/procfs.  Used by bhardware, bdevices, bdriver, bnetwork, bpower
 * and the desktop hardware apps.  All functions are plain libc and portable
 * across ARM64/x86-64 busybox rootfs.
 */
#ifndef BHW_H
#define BHW_H

#include <stddef.h>

#define BHW_VERSION "1.0.0"

#define BHW_MAX_STR 256

struct bhw_cpu {
    char model[BHW_MAX_STR];
    char arch[BHW_MAX_STR];
    int cores;
    int online;
    int bogo;
};

struct bhw_mem {
    unsigned long total_kb;
    unsigned long free_kb;
    unsigned long avail_kb;
    int    mem_available;   /* 1 if /proc/meminfo had MemAvailable */
};

struct bhw_netif {
    char name[BHW_MAX_STR];
    char addr[64];
    char mac[32];
    int  up;
    unsigned long rx_bytes;
    unsigned long tx_bytes;
};

struct bhw_pci {
    char slot[BHW_MAX_STR];     /* 0000:00:1f.6 style */
    char cls[BHW_MAX_STR];      /* class name */
    char vendor[BHW_MAX_STR];
    char device[BHW_MAX_STR];
};

struct bhw_gpu {
    char name[BHW_MAX_STR];
    char driver[BHW_MAX_STR];
    int  present;
    int  width, height;
    int  bpp;
    char fb_path[320];
};

struct bhw_gpu_list {
    struct bhw_gpu *items;
    int count;
};

/* Reads a single-line sysfs/proc file into buf. Returns 1 on success. */
int bhw_read_file(const char *path, char *buf, size_t size);

/* CPU / memory */
void bhw_cpu_info(struct bhw_cpu *out);
void bhw_mem_info(struct bhw_mem *out);

/* Enumerate PCI devices into caller's array (max maxn). Returns count. */
int bhw_pci_list(struct bhw_pci *out, int maxn);

/* GPU via DRM/framebuffer */
void bhw_gpu_detect(struct bhw_gpu_list *gl);
int bhw_gpu_first(struct bhw_gpu *out);

/* Network interfaces */
int bhw_net_list(struct bhw_netif *out, int maxn);

/* Storage: block devices (/sys/block) */
int bhw_block_count(void);
int bhw_block_names(char names[][BHW_MAX_STR], int maxn);

/* USB device count */
int bhw_usb_count(void);

/* Audio: cards under /proc/asound (or /dev/snd). 0 if none. */
int bhw_audio_cards(char cards[][BHW_MAX_STR], int maxn);

/* Battery/power reality check */
int bhw_power_battery(int *level, int *ac); /* returns 1 if a battery exists */

/* Driver registry reported by the bulum kernel framework */
int bhw_bulum_drivers(char lines[][BHW_MAX_STR], int maxn);
int bhw_bulum_hardware(char lines[][BHW_MAX_STR], int maxn);

/* Free a gpu list allocated by bhw_gpu_detect */
void bhw_gpu_free(struct bhw_gpu_list *gl);

#endif
