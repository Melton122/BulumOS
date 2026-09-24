/*
 * bpktest.c - host harness for the native .bpk extractor (Phase 4).
 * Compiled with bulum.c in the same translation unit (static access):
 *   gcc -O2 -I src -o /tmp/bpktest tools/bpktest.c src/bulum_crypto.c
 *
 * usage: bpktest <file.bpk> <stage-dir> <expect-file>
 *   - verifies the embedded signature + payload sha256, extracts the payload
 *     into stage-dir, then checks expect-file exists with the right content.
 *   - exit 0 only if all of that succeeded.
 */
#define main bulum_main
#include "bulum-bpk.c" /* sed'd copy of src/bulum.c with KEYS_DIR => /tmp/bpkkeys */
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    pkg_t ctrl;
    rmdir_recursive(argv[2]);
    int rc = bpk_extract(argv[1], argv[2], NULL, &ctrl);
    printf("bpk_extract rc=%d name=%s version=%s\n",
           rc, ctrl.name[0] ? ctrl.name : "(none)", ctrl.version);
    if (rc != 0)
        return 1;

    char p[700];
    snprintf(p, sizeof(p), "%s/%s", argv[2], argv[3]);
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("missing extracted file: %s\n", p);
        return 1;
    }
    if ((st.st_mode & 07777) != 0755) {
        printf("bad mode %o on %s\n", st.st_mode & 07777, p);
        return 1;
    }
    FILE *f = fopen(p, "r");
    if (!f)
        return 1;
    char buf[256];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = 0;
    fclose(f);
    if (!strstr(buf, "hello from bpk 2.0")) {
        printf("unexpected content: %s\n", buf);
        return 1;
    }
    return 0;
}