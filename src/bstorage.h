#ifndef BSTORAGE_H
#define BSTORAGE_H

#include <stddef.h>
#include <stdint.h>

#define BSTORAGE_NAME_MAX 64
#define BSTORAGE_PATH_MAX 256
#define BSTORAGE_FS_MAX 32
#define BSTORAGE_OPTIONS_MAX 256

typedef struct {
    char name[BSTORAGE_NAME_MAX];
    char path[BSTORAGE_PATH_MAX];
    char parent[BSTORAGE_NAME_MAX];
    char filesystem[BSTORAGE_FS_MAX];
    char mountpoint[BSTORAGE_PATH_MAX];
    char partition_type[BSTORAGE_FS_MAX];
    uint32_t major;
    uint32_t minor;
    uint32_t partition_number;
    uint64_t start_sector;
    uint64_t sector_count;
    uint64_t size_bytes;
    uint32_t sector_size;
    int is_partition;
    int removable;
    int read_only;
    int rotational;
    int mounted;
} BStorageDevice;

typedef struct {
    char source[BSTORAGE_PATH_MAX];
    char target[BSTORAGE_PATH_MAX];
    char filesystem[BSTORAGE_FS_MAX];
    char options[BSTORAGE_OPTIONS_MAX];
    uint32_t major;
    uint32_t minor;
    int read_only;
} BStorageMount;

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t available_bytes;
    int read_only;
} BStorageUsage;

int bstorage_scan(void);
size_t bstorage_device_count(void);
size_t bstorage_disk_count(void);
size_t bstorage_partition_count(void);
int bstorage_get_device(size_t index, BStorageDevice *out);
int bstorage_get_disk(size_t index, BStorageDevice *out);
int bstorage_get_partition(size_t index, BStorageDevice *out);
size_t bstorage_mount_count(void);
int bstorage_get_mount(size_t index, BStorageMount *out);
int bstorage_find_mount(const char *path, BStorageMount *out);
int bstorage_usage(const char *path, BStorageUsage *out);
int bstorage_filesystem(const char *device, BStorageMount *out);

#ifdef BSTORAGE_TESTING
void bstorage_set_roots(const char *sys_root, const char *proc_root,
                        const char *dev_root);
#endif

#endif