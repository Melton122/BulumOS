#ifndef BSTORAGE_H
#define BSTORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char name[32];
    uint64_t size_bytes;
    bool removable;
    bool is_partition;
} BStorageDevice;

typedef struct {
    char device[64];
    char mount_point[256];
    char fs_type[32];
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t available_bytes;
} BStorageMount;

void bstorage_set_roots(const char *sys_root, const char *proc_root, const char *dev_root);
int bstorage_scan(void);
int bstorage_device_count(void);
int bstorage_get_device(int index, BStorageDevice *dev);
int bstorage_mount_count(void);
int bstorage_get_mount(int index, BStorageMount *mnt);
int bstorage_find_mount(const char *path, BStorageMount *mnt);

#endif
