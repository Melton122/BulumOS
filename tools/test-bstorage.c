#define _GNU_SOURCE
#include "../src/bstorage.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
        failures++;
}

static int make_dir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int put_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (!file)
        return -1;
    fputs(text, file);
    return fclose(file);
}

static void path_join(char *out, size_t size, const char *root,
                      const char *relative)
{
    snprintf(out, size, "%s/%s", root, relative);
}

static int remove_entry(const char *path, const struct stat *info, int type,
                        struct FTW *state)
{
    (void)info;
    (void)type;
    (void)state;
    return remove(path);
}

int main(void)
{
    char template[] = "/tmp/bstorage-test.XXXXXX";
    char *root = mkdtemp(template);
    char path[1024], sys[1024], proc[1024], dev[1024];
    if (!root) {
        perror("mkdtemp");
        return 1;
    }
    path_join(sys, sizeof(sys), root, "sys");
    path_join(proc, sizeof(proc), root, "proc");
    path_join(dev, sizeof(dev), root, "dev");
    const char *dirs[] = {
        "sys", "sys/class", "sys/class/block", "sys/class/block/nvme0n1",
        "sys/class/block/nvme0n1/queue", "sys/class/block/nvme0n1p1",
        "sys/class/block/nvme0n1p1/queue", "proc", "proc/self", "dev", NULL
    };
    for (size_t i = 0; dirs[i]; i++) {
        path_join(path, sizeof(path), root, dirs[i]);
        if (make_dir(path) != 0) {
            perror(path);
            return 1;
        }
    }

    const struct { const char *file; const char *data; } fixtures[] = {
        {"sys/class/block/nvme0n1/dev", "259:0\n"},
        {"sys/class/block/nvme0n1/size", "2097152\n"},
        {"sys/class/block/nvme0n1/removable", "0\n"},
        {"sys/class/block/nvme0n1/ro", "0\n"},
        {"sys/class/block/nvme0n1/queue/logical_block_size", "4096\n"},
        {"sys/class/block/nvme0n1/queue/rotational", "0\n"},
        {"sys/class/block/nvme0n1p1/dev", "259:1\n"},
        {"sys/class/block/nvme0n1p1/size", "2000000\n"},
        {"sys/class/block/nvme0n1p1/partition", "1\n"},
        {"sys/class/block/nvme0n1p1/start", "32768\n"},
        {"proc/partitions", "major minor  #blocks  name\n 259 0 1048576 nvme0n1\n 259 1 1000000 nvme0n1p1\n 8 17 2048 sdb1\n"},
        {"proc/self/mountinfo",
         "36 25 259:1 / / rw,relatime - ext4 /dev/nvme0n1p1 rw\n"
         "37 25 0:55 / /mnt/data\\040disk ro,nosuid - mystery /dev/loop0 ro\n"
         "38 25 0:1 / /proc rw,nosuid - proc proc rw\n"},
        {NULL, NULL}
    };
    for (size_t i = 0; fixtures[i].file; i++) {
        path_join(path, sizeof(path), root, fixtures[i].file);
        if (put_file(path, fixtures[i].data) != 0) {
            perror(path);
            return 1;
        }
    }

    bstorage_set_roots(sys, proc, dev);
    check(bstorage_scan() == 0, "scan returns success with fixtures");
        check(bstorage_device_count() == 3, "sysfs and proc discovery deduplicate devices");
        check(bstorage_disk_count() == 1 && bstorage_partition_count() == 2,
            "disk and partition counts");

    BStorageDevice disk, partition;
    check(bstorage_get_disk(0, &disk) == 0 &&
          strcmp(disk.name, "nvme0n1") == 0 && disk.size_bytes == 1073741824ULL,
          "NVMe disk capacity and name");
    check(bstorage_get_partition(0, &partition) == 0 &&
          strcmp(partition.parent, "nvme0n1") == 0 &&
          partition.partition_number == 1 && partition.start_sector == 32768,
          "NVMe pN partition parent and metadata");
    check(partition.sector_size == 512,
          "partition sector size has safe default when sysfs omits queue data");
        check(bstorage_get_partition(1, &partition) == 0 &&
            strcmp(partition.name, "sdb1") == 0 &&
            strcmp(partition.parent, "sdb") == 0,
            "proc-only sd partition inference");

    check(bstorage_mount_count() == 3, "mountinfo rows parsed");
    BStorageMount mount;
    check(bstorage_find_mount("/mnt/data disk/file", &mount) == 0 &&
          strcmp(mount.target, "/mnt/data disk") == 0 && mount.read_only &&
          strcmp(mount.filesystem, "mystery") == 0,
          "escaped mount target and unknown read-only filesystem");
    check(bstorage_filesystem("/dev/nvme0n1p1", &mount) == 0 &&
          strcmp(mount.filesystem, "ext4") == 0,
          "device filesystem association");
    check(bstorage_find_mount("/proc/not-a-prefix", &mount) == 0 &&
          strcmp(mount.target, "/proc") == 0,
          "mount path boundary matching");
        check(bstorage_find_mount("/not-under-proc", &mount) == 0 &&
            strcmp(mount.target, "/") == 0,
            "root mount matches arbitrary paths");

    BStorageUsage usage;
    check(bstorage_usage(root, &usage) == 0 && usage.total_bytes > 0 &&
          usage.available_bytes <= usage.total_bytes,
          "statvfs usage has bounded values");
    check(bstorage_get_device(9999, &disk) != 0 &&
          bstorage_get_mount(9999, &mount) != 0,
          "out-of-range access fails safely");

    nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}