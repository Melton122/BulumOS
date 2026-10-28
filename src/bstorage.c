/*
 * bstorage.c - Storage & Filesystem Discovery (Phase 13)
 *
 * Implements the BStorage API by scanning /sys and /proc.
 */

#define _GNU_SOURCE
#include "bstorage.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define BSTORAGE_MAX_DEVICES 256
#define BSTORAGE_MAX_MOUNTS 256
#define BSTORAGE_LINE_MAX 4096

static BStorageDevice devices[BSTORAGE_MAX_DEVICES];
static BStorageMount mounts[BSTORAGE_MAX_MOUNTS];
static size_t ndevices;
static size_t nmounts;
static const char *sys_root = "/sys";
static const char *proc_root = "/proc";
static const char *dev_root = "/dev";

#ifdef BSTORAGE_TESTING
void bstorage_set_roots(const char *sys, const char *proc, const char *dev)
{
    if (sys && *sys) sys_root = sys;
    if (proc && *proc) proc_root = proc;
    if (dev && *dev) dev_root = dev;
}
#endif

static int make_path(char *out, size_t size, const char *a, const char *b,
                     const char *c)
{
    int n = snprintf(out, size, "%s/%s%s%s", a, b,
                     c && *c ? "/" : "", c && *c ? c : "");
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int read_value(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)size, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static int read_attribute(const char *base, const char *name, char *out,
                          size_t size)
{
    char path[PATH_MAX];
    if (make_path(path, sizeof(path), base, name, NULL) != 0)
        return -1;
    return read_value(path, out, size);
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end;
    unsigned long long value;
    if (!text || !*text || *text == '-')
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || end == text || *end)
        return -1;
    *out = (uint64_t)value;
    return 0;
}

static int parse_bool_attribute(const char *base, const char *name)
{
    char value[32];
    uint64_t number;
    if (read_attribute(base, name, value, sizeof(value)) != 0 ||
        parse_u64(value, &number) != 0)
        return 0;
    return number != 0;
}

static int parse_dev_number(const char *text, uint32_t *major_out,
                            uint32_t *minor_out)
{
    char *end;
    unsigned long major_value, minor_value;
    errno = 0;
    major_value = strtoul(text, &end, 10);
    if (errno || end == text || *end != ':')
        return -1;
    text = end + 1;
    errno = 0;
    minor_value = strtoul(text, &end, 10);
    if (errno || end == text || *end || major_value > UINT32_MAX ||
        minor_value > UINT32_MAX)
        return -1;
    *major_out = (uint32_t)major_value;
    *minor_out = (uint32_t)minor_value;
    return 0;
}

static void read_partition_type(const char *base, char *out, size_t size)
{
    char path[PATH_MAX], line[512];
    FILE *f;
    if (make_path(path, sizeof(path), base, "uevent", NULL) != 0)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PARTTYPE=", 9) == 0) {
            snprintf(out, size, "%s", line + 9);
            out[strcspn(out, "\r\n")] = '\0';
            break;
        }
    }
    fclose(f);
}

static void find_parent(const char *class_path, const char *name,
                        BStorageDevice *device)
{
    char resolved[PATH_MAX], parent[PATH_MAX];
    char *slash;
    struct stat info;
    if (!device->is_partition)
        return;
    if (lstat(class_path, &info) == 0 && S_ISLNK(info.st_mode) &&
        realpath(class_path, resolved)) {
        snprintf(parent, sizeof(parent), "%s", resolved);
        slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            snprintf(device->parent, sizeof(device->parent), "%s", slash + 1);
        }
    }
    if (device->parent[0])
        return;
    size_t length = strlen(name);
    size_t prefix = length;
    while (prefix > 0 && name[prefix - 1] >= '0' && name[prefix - 1] <= '9')
        prefix--;
    if (prefix > 0 && name[prefix - 1] == 'p')
        prefix--;
    if (prefix > 0 && prefix < sizeof(device->parent)) {
        memcpy(device->parent, name, prefix);
        device->parent[prefix] = '\0';
    }
}

static BStorageDevice *find_device_name(const char *name)
{
    for (size_t i = 0; i < ndevices; i++)
        if (strcmp(devices[i].name, name) == 0)
            return &devices[i];
    return NULL;
}

static void infer_partition(const char *name, BStorageDevice *device)
{
    size_t length = strlen(name), digits = length;
    while (digits > 0 && name[digits - 1] >= '0' && name[digits - 1] <= '9')
        digits--;
    if (digits == length || digits == 0)
        return;
    uint64_t number;
    if (parse_u64(name + digits, &number) != 0 || number > UINT32_MAX || !number)
        return;
    size_t parent_length = digits;
    if (parent_length > 0 && name[parent_length - 1] == 'p') {
        parent_length--;
    } else {
        int letter_disk_prefix =
            (strncmp(name, "sd", 2) == 0 || strncmp(name, "vd", 2) == 0 ||
             strncmp(name, "hd", 2) == 0 || strncmp(name, "xvd", 3) == 0);
        for (size_t i = 0; i < digits; i++)
            if (name[i] < 'a' || name[i] > 'z')
                return;
        if (!letter_disk_prefix)
            return;
    }
    if (!parent_length || parent_length >= sizeof(device->parent))
        return;
    device->is_partition = 1;
    device->partition_number = (uint32_t)number;
    memcpy(device->parent, name, parent_length);
    device->parent[parent_length] = '\0';
}

static void add_sys_device(const char *name, const char *class_path)
{
    BStorageDevice item;
    char value[128], path[PATH_MAX];
    uint64_t n;
    memset(&item, 0, sizeof(item));
    snprintf(item.name, sizeof(item.name), "%s", name);
    if (snprintf(item.path, sizeof(item.path), "%s/%s", dev_root, name) >=
        (int)sizeof(item.path))
        return;
    if (read_attribute(class_path, "partition", value, sizeof(value)) == 0 &&
        parse_u64(value, &n) == 0 && n <= UINT32_MAX) {
        item.is_partition = 1;
        item.partition_number = (uint32_t)n;
        if (read_attribute(class_path, "start", value, sizeof(value)) == 0 &&
            parse_u64(value, &item.start_sector) != 0)
            item.start_sector = 0;
    }
    if (read_attribute(class_path, "size", value, sizeof(value)) == 0 &&
        parse_u64(value, &n) == 0 &&
        item.sector_count <= UINT64_MAX / 512)
        item.size_bytes = item.sector_count * 512;
    if (read_attribute(class_path, "dev", value, sizeof(value)) == 0)
        parse_dev_number(value, &item.major, &item.minor);
    if (read_attribute(class_path, "queue/logical_block_size", value,
                       sizeof(value)) == 0 &&
        parse_u64(value, &n) == 0 && n <= UINT32_MAX)
        item.sector_size = (uint32_t)n;
    if (!item.sector_size)
        item.sector_size = 512;
    item.removable = parse_bool_attribute(class_path, "removable");
    item.read_only = parse_bool_attribute(class_path, "ro");
    item.rotational = parse_bool_attribute(class_path, "queue/rotational");
    read_partition_type(class_path, item.partition_type,
                        sizeof(item.partition_type));
    find_parent(class_path, name, &item);
    if (ndevices < BSTORAGE_MAX_DEVICES)
        devices[ndevices++] = item;
    (void)path;
}

static void scan_sys_class(void)
{
    char class_dir[PATH_MAX], class_path[PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    if (make_path(class_dir, sizeof(class_dir), sys_root, "class", "block") != 0)
        return;
    dir = opendir(class_dir);
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        if (make_path(class_path, sizeof(class_path), class_dir,
                      entry->d_name, NULL) != 0)
            continue;
        add_sys_device(entry->d_name, class_path);
    }
    closedir(dir);
}

static void scan_proc_partitions(void)
{
    char path[PATH_MAX], line[512];
    FILE *f;
    if (make_path(path, sizeof(path), proc_root, "partitions", NULL) != 0)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned int major_value, minor_value;
        unsigned long long blocks;
        char name[BSTORAGE_NAME_MAX];
        BStorageDevice item;
        int fields = sscanf(line, " %u %u %llu %63s", &major_value,
                            &minor_value, &blocks, name);
        if (fields != 4 || find_device_name(name) || ndevices >= BSTORAGE_MAX_DEVICES)
            continue;
        memset(&item, 0, sizeof(item));
        snprintf(item.name, sizeof(item.name), "%s", name);
        if (snprintf(item.path, sizeof(item.path), "%s/%s", dev_root, name) >=
            (int)sizeof(item.path))
            continue;
        item.major = major_value;
        item.minor = minor_value;
        infer_partition(name, &item);
        item.sector_count = blocks <= UINT64_MAX / 2 ? blocks * 2 : 0;
        item.size_bytes = blocks <= UINT64_MAX / 1024 ?
                          (uint64_t)blocks * 1024 : 0;
        item.sector_size = 512;
        devices[ndevices++] = item;
    }
    fclose(f);
}

static int decode_mount_field(char *out, size_t size, const char *in)
{
    size_t used = 0;
    while (*in) {
        char c = *in++;
        if (c == '\\' && in[0] >= '0' && in[0] <= '7' &&
            in[1] >= '0' && in[1] <= '7' && in[2] >= '0' && in[2] <= '7') {
            c = (char)((in[0] - '0') * 64 + (in[1] - '0') * 8 + (in[2] - '0'));
            in += 3;
        }
        if (used + 1 >= size)
            return -1;
        out[used++] = c;
    }
    out[used] = '\0';
    return 0;
}

static void append_option(char *out, size_t size, const char *option)
{
    size_t used = strlen(out);
    if (used >= size - 1)
        return;
    snprintf(out + used, size - used, "%s%s", used ? "," : "", option);
}

static int has_option(const char *options, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    const char *cursor = options;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, ',');
        size_t length = (end ? (size_t)(end - cursor) : strlen(cursor));
        if (length == wanted_length && memcmp(cursor, wanted, length) == 0)
            return 1;
        if (!end)
            break;
        cursor = end + 1;
    }
    return 0;
}

static void add_mountinfo_line(char *line)
{
    char *tokens[96], *save = NULL, *token;
    size_t count = 0, sep = SIZE_MAX;
    BStorageMount item;
    memset(&item, 0, sizeof(item));
    for (token = strtok_r(line, " \n", &save); token && count < 96;
         token = strtok_r(NULL, " \n", &save))
        tokens[count++] = token;
    if (count < 10 || nmounts >= BSTORAGE_MAX_MOUNTS)
        return;
    for (size_t i = 6; i < count; i++)
        if (strcmp(tokens[i], "-") == 0) {
            sep = i;
            break;
        }
    if (sep == SIZE_MAX || sep + 3 >= count ||
        parse_dev_number(tokens[2], &item.major, &item.minor) != 0 ||
        decode_mount_field(item.target, sizeof(item.target), tokens[4]) != 0 ||
        decode_mount_field(item.filesystem, sizeof(item.filesystem),
                           tokens[sep + 1]) != 0 ||
        decode_mount_field(item.source, sizeof(item.source),
                           tokens[sep + 2]) != 0)
        return;
    item.read_only = has_option(tokens[5], "ro") ||
                     has_option(tokens[sep + 3], "ro");
    snprintf(item.options, sizeof(item.options), "%s", tokens[5]);
    if (sep + 3 < count)
        append_option(item.options, sizeof(item.options), tokens[sep + 3]);
    mounts[nmounts++] = item;
}

static void scan_mountinfo(void)
{
    char path[PATH_MAX], line[BSTORAGE_LINE_MAX];
    FILE *f;
    if (make_path(path, sizeof(path), proc_root, "self/mountinfo", NULL) != 0)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
            continue;
        }
        add_mountinfo_line(line);
    }
    fclose(f);
}

static void scan_mounts_fallback(void)
{
    char path[PATH_MAX], line[BSTORAGE_LINE_MAX];
    FILE *f;
    if (nmounts || make_path(path, sizeof(path), proc_root, "mounts", NULL) != 0)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f) && nmounts < BSTORAGE_MAX_MOUNTS) {
        char source[BSTORAGE_PATH_MAX], target[BSTORAGE_PATH_MAX];
        char filesystem[BSTORAGE_FS_MAX], options[BSTORAGE_OPTIONS_MAX];
        BStorageMount item;
        if (sscanf(line, "%255s %255s %31s %255s", source, target,
                   filesystem, options) != 4)
            continue;
        memset(&item, 0, sizeof(item));
        if (decode_mount_field(item.source, sizeof(item.source), source) != 0 ||
            decode_mount_field(item.target, sizeof(item.target), target) != 0 ||
            decode_mount_field(item.filesystem, sizeof(item.filesystem),
                               filesystem) != 0 ||
            decode_mount_field(item.options, sizeof(item.options), options) != 0)
            continue;
        item.read_only = has_option(item.options, "ro");
        mounts[nmounts++] = item;
    }
    fclose(f);
}

static void attach_mounts(void)
{
    for (size_t m = 0; m < nmounts; m++) {
        for (size_t d = 0; d < ndevices; d++) {
            BStorageDevice *device = &devices[d];
            if ((mounts[m].major && mounts[m].major == device->major &&
                 mounts[m].minor == device->minor) ||
                strcmp(mounts[m].source, device->path) == 0) {
                device->mounted = 1;
                device->read_only |= mounts[m].read_only;
                snprintf(device->filesystem, sizeof(device->filesystem), "%s",
                         mounts[m].filesystem);
                snprintf(device->mountpoint, sizeof(device->mountpoint), "%s",
                         mounts[m].target);
            }
        }
    }
}

int bstorage_scan(void)
{
#ifdef BSTORAGE_TESTING
    const char *env_sys = getenv("BSTORAGE_SYS_ROOT");
    const char *env_proc = getenv("BSTORAGE_PROC_ROOT");
    const char *env_dev = getenv("BSTORAGE_DEV_ROOT");
    if (env_sys && *env_sys) sys_root = env_sys;
    if (env_proc && *env_proc) proc_root = env_proc;
    if (env_dev && *env_dev) dev_root = env_dev;
#endif
    ndevices = 0;
    nmounts = 0;
    memset(devices, 0, sizeof(devices));
    memset(mounts, 0, sizeof(mounts));
    scan_sys_class();
    scan_proc_partitions();
    scan_mountinfo();
    scan_mounts_fallback();
    attach_mounts();
    return 0;
}

size_t bstorage_device_count(void)
{
    return ndevices;
}

size_t bstorage_disk_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < ndevices; i++)
        if (!devices[i].is_partition)
            count++;
    return count;
}

size_t bstorage_partition_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < ndevices; i++)
        if (devices[i].is_partition)
            count++;
    return count;
}

int bstorage_get_device(size_t index, BStorageDevice *out)
{
    if (!out || index >= ndevices)
        return -1;
    *out = devices[index];
    return 0;
}

static int get_filtered_device(size_t index, int partition, BStorageDevice *out)
{
    if (!out)
        return -1;
    for (size_t i = 0; i < ndevices; i++) {
        if (devices[i].is_partition != partition)
            continue;
        if (index-- == 0) {
            *out = devices[i];
            return 0;
        }
    }
    return -1;
}

int bstorage_get_disk(size_t index, BStorageDevice *out)
{
    return get_filtered_device(index, 0, out);
}

int bstorage_get_partition(size_t index, BStorageDevice *out)
{
    return get_filtered_device(index, 1, out);
}

size_t bstorage_mount_count(void)
{
    return nmounts;
}

int bstorage_get_mount(size_t index, BStorageMount *out)
{
    if (!out || index >= nmounts)
        return -1;
    *out = mounts[index];
    return 0;
}

int bstorage_find_mount(const char *path, BStorageMount *out)
{
    char resolved[PATH_MAX];
    const char *candidate = path;
    size_t best = 0;
    int found = 0;
    if (!path || !*path || !out)
        return -1;
    if (realpath(path, resolved))
        candidate = resolved;
    for (size_t i = 0; i < nmounts; i++) {
        size_t length = strlen(mounts[i].target);
        if (length < best || strncmp(candidate, mounts[i].target, length) != 0)
            continue;
        if (strcmp(mounts[i].target, "/") != 0 &&
            candidate[length] && candidate[length] != '/')
            continue;
        *out = mounts[i];
        best = length;
        found = 1;
    }
    return found ? 0 : -1;
}

int bstorage_usage(const char *path, BStorageUsage *out)
{
    struct statvfs info;
    uint64_t unit, total, free_bytes, available;
    BStorageMount mount;
    if (!path || !*path || !out || statvfs(path, &info) != 0)
        return -1;
    unit = info.f_frsize ? info.f_frsize : info.f_bsize;
    if (unit && (uint64_t)info.f_blocks > UINT64_MAX / unit)
        return -1;
    total = (uint64_t)info.f_blocks * unit;
    if (unit && (uint64_t)info.f_bfree > UINT64_MAX / unit)
        return -1;
    free_bytes = (uint64_t)info.f_bfree * unit;
    if (unit && (uint64_t)info.f_bavail > UINT64_MAX / unit)
        return -1;
    available = (uint64_t)info.f_bavail * unit;
    out->total_bytes = total;
    out->used_bytes = total >= free_bytes ? total - free_bytes : 0;
    out->available_bytes = available;
    out->read_only = !!(info.f_flag & ST_RDONLY);
    if (bstorage_find_mount(path, &mount) == 0)
        out->read_only |= mount.read_only;
    return 0;
}

int bstorage_filesystem(const char *device, BStorageMount *out)
{
    if (!device || !*device || !out)
        return -1;
    for (size_t i = 0; i < nmounts; i++) {
        if (strcmp(mounts[i].source, device) == 0) {
            *out = mounts[i];
            return 0;
        }
    }
    for (size_t i = 0; i < ndevices; i++) {
        if (strcmp(devices[i].name, device) == 0 ||
            strcmp(devices[i].path, device) == 0) {
            if (!devices[i].mounted)
                return -1;
            for (size_t m = 0; m < nmounts; m++) {
                if (mounts[m].major == devices[i].major &&
                    mounts[m].minor == devices[i].minor) {
                    *out = mounts[m];
                    return 0;
                }
            }
        }
    }
    return -1;
}
