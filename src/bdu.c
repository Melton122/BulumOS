/*
 * bdu - Directory Usage utility (Phase 13)
 *
 * Usage: bdu [PATH]
 */

#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>

#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

static void human_bytes(uint64_t bytes, char *buf, size_t sz)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double dbytes = (double)bytes;
    while (dbytes >= 1024 && unit_idx < 4) {
        dbytes /= 1024;
        unit_idx++;
    }
    snprintf(buf, sz, "%.1f %s", dbytes, units[unit_idx]);
}

static uint64_t calculate_dir_size(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return st.st_size;

    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        total += calculate_dir_size(full);
    }
    closedir(d);
    return total;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : ".";
    
    printf(C_BOLD "Calculating usage for: %s\n" C_RESET, path);
    uint64_t size = calculate_dir_size(path);
    char hsize[32];
    human_bytes(size, hsize, sizeof(hsize));
    printf("Total size: %s\n", hsize);
    
    return 0;
}
