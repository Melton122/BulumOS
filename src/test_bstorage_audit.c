/*
 * test_bstorage_audit.c - Phase 14 Reliability & Safety Audit
 * 
 * This test suite uses bstorage_set_roots to simulate various system
 * states (broken, huge, messy) and verifies that the bstorage library
 * handles them gracefully without crashing or overflowing.
 */

#define BSTORAGE_TESTING
#include "bstorage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define FIXTURE_ROOT "test_fixtures"

static void make_dir(const char *path) {
    mkdir(path, 0755);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* --- Test Case 1: The Messy System --- */
/* Tests: Octal escapes, spaces in paths, missing attributes */
static void test_messy_system() {
    printf("[TEST] Messy System Discovery... ");
    char root[256], sys[256], proc[256], dev[256];
    snprintf(root, sizeof(root), "%s/messy", FIXTURE_ROOT);
    snprintf(sys, sizeof(sys), "%s/sys", root);
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(dev, sizeof(dev), "%s/dev", root);

    make_dir(root);
    make_dir(sys);
    make_dir(proc);
    make_dir(dev);
    
    // Create a block device with missing 'size' attribute
    char block_dir[512];
    snprintf(block_dir, sizeof(block_dir), "%s/class/block/sda", sys);
    make_dir(block_dir);
    write_file(block_dir "/removable", "0");
    // Size is missing!

    // Create a mount with octal escapes and spaces
    // /dev/sda1 /mnt/my folder ext4 rw,relatime 0 0
    char mountinfo[512];
    snprintf(mountinfo, sizeof(mountinfo), "1 0 8:1 / /mnt/my\\040folder ext4 rw,relatime 0 0 -");
    char mountinfo_path[512];
    snprintf(mountinfo_path, sizeof(mountinfo_path), "%s/self/mountinfo", proc);
    make_dir(proc);
    make_dir(proc "/self");
    write_file(mountinfo_path, mountinfo);

    bstorage_set_roots(sys, proc, dev);
    if (bstorage_scan() != 0) {
        printf("FAILED (scan error)\n");
        return;
    }

    BStorageMount mnt;
    if (bstorage_find_mount("/mnt/my folder", &mnt) == 0) {
        printf("PASSED\n");
    } else {
        printf("FAILED (octal decode failed)\n");
    }
}

/* --- Test Case 2: The Huge System --- */
/* Tests: Integer overflows, huge sector counts */
static void test_huge_system() {
    printf("[TEST] Huge System Capacity... ");
    char root[256], sys[256], proc[256], dev[256];
    snprintf(root, sizeof(root), "%s/huge", FIXTURE_ROOT);
    snprintf(sys, sizeof(sys), "%s/sys", root);
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(dev, sizeof(dev), "%s/dev", root);

    make_dir(root);
    make_dir(sys);
    make_dir(proc);
    make_dir(dev);

    char block_dir[512];
    snprintf(block_dir, sizeof(block_dir), "%s/class/block/sdb", sys);
    make_dir(block_dir);
    // 2^60 sectors * 512 = huge size
    write_file(block_dir "/size", "1152921504606846976"); 

    bstorage_set_roots(sys, proc, dev);
    bstorage_scan();

    BStorageDevice dev_info;
    if (bstorage_get_disk(0, &dev_info) == 0 && dev_info.size_bytes > 0) {
        printf("PASSED\n");
    } else {
        printf("FAILED (overflow or scan failure)\n");
    }
}

/* --- Test Case 3: The Broken System --- */
/* Tests: Permission errors, nonexistent roots */
static void test_broken_system() {
    printf("[TEST] Broken System Handling... ");
    bstorage_set_roots("/nonexistent/sys", "/nonexistent/proc", "/nonexistent/dev");
    if (bstorage_scan() != 0) {
        printf("PASSED (handled missing roots)\n");
    } else {
        printf("FAILED (didn't report error for missing roots)\n");
    }
}

int main() {
    printf("=== BulumOS Phase 14: Storage Audit Suite ===\n");
    
    // Setup fixture root
    mkdir(FIXTURE_ROOT, 0755);

    test_messy_system();
    test_huge_system();
    test_broken_system();

    printf("==============================================\n");
    return 0;
}
