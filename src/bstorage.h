#ifndef BSTORAGE_H
#define BSTORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char name[32];
    char path[64];
    uint64_t size_bytes;
    bool removable;
    bool is_partition;
    bool read_only;
    bool rotational;
    bool mounted;
    char filesystem[32];
    char mountpoint[256];
    char parent[64];
    uint32_t partition_number;
} BStorageDevice;

typedef struct {
    char source[64];
    char target[256];
    char filesystem[32];
    char options[256];
    bool read_only;
} BStorageMount;

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t available_bytes;
} BStorageUsage;

void bstorage_set_roots(const char *sys_root, const char *proc_root, const char *dev_root);
int bstorage_scan(void);
int bstorage_device_count(void);
int bstorage_get_device(int index, BStorageDevice *dev);
int bstorage_mount_count(void);
int bstorage_get_mount(int index, BStorageMount *mnt);
int bstorage_find_mount(const char *path, BStorageMount *mnt);
int bstorage_usage(const char *path, BStorageUsage *usage);
int bstorage_filesystem(const char *device_path, BStorageMount *mnt);
int bstorage_disk_count(void);
int bstorage_partition_count(void);
void bstorage_human_bytes(uint64_t bytes, char *buf, size_t sz);

#endif
