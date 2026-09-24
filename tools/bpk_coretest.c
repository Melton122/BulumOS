/*
 * bpk_coretest.c - host harness for Phase 12 pure-logic checks.
 * Compiled with bulum.c in the same translation unit (static access):
 *   gcc -O2 -I src -o /tmp/bpk_coretest tools/bpk_coretest.c src/bulum_crypto.c
 *
 * Covers: version comparison, architecture compatibility (+ BULUM_ARCH
 * override), dpkg-style field parsing incl. the new License/Keywords/Origin
 * fields, and JSON string escaping. Read-only: never touches the live db.
 */
#define main bulum_main
#include "bulum-bpk.c" /* sed'd copy of src/bulum.c (sandbox paths) */
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok:   %s\n", msg); \
} while (0)

static void test_vercmp(void)
{
    struct { const char *a; const char *b; int want; } cas[] = {
        { "1.0", "1.0", 0 },
        { "1.0", "1.1", -1 },
        { "1.1", "1.0", 1 },
        { "1.0", "1.0.1", -1 },
        { "2.0", "1.9", 1 },
        { "1.0.1", "1.0.2", -1 },
        /* dpkg epoch precedence: epoch 1 beats epoch 0 regardless of version */
        { "1:1.0", "1.0", 1 },
        { "0:1.0", "1:1.0", -1 },
        { "1.0-1", "1.0-2", -1 },
        { "1.0~rc1", "1.0", -1 },
        { "1.0", "1.0~rc1", 1 },
    };
    for (size_t i = 0; i < sizeof(cas) / sizeof(cas[0]); i++) {
        int got = vercmp(cas[i].a, cas[i].b);
        /* the port returns raw differences; callers only use the sign */
        int gs = (got > 0) - (got < 0);
        int ws = (cas[i].want > 0) - (cas[i].want < 0);
        char msg[128];
        snprintf(msg, sizeof(msg), "vercmp(%s,%s) sign=%d want %d", cas[i].a,
                 cas[i].b, gs, ws);
        CHECK(gs == ws, msg);
    }
}

static void test_arch(void)
{
    /* BULUM_ARCH=aarch64 forces the "system" arch aarch64 */
    setenv("BULUM_ARCH", "aarch64", 1);
    CHECK(!strcmp(sys_arch(), "aarch64"), "sys_arch()=aarch64 with override");
    CHECK(arch_compat("arm64"), "aarch64 sys accepts arm64 alias");
    CHECK(arch_compat("aarch64"), "aarch64 sys accepts aarch64");
    CHECK(arch_compat("all"), "any arch accepts 'all'");
    CHECK(arch_compat("any"), "any arch accepts 'any'");
    CHECK(!arch_compat("x86_64"), "aarch64 sys rejects x86_64");
    CHECK(!arch_compat("amd64"), "aarch64 sys rejects amd64");
    CHECK(!arch_compat(""), "empty arch rejected");

    setenv("BULUM_ARCH", "x86_64", 1);
    CHECK(!strcmp(sys_arch(), "x86_64"), "sys_arch()=x86_64 with override");
    CHECK(arch_compat("amd64"), "x86_64 sys accepts amd64 alias");
    CHECK(arch_compat("x86_64"), "x86_64 sys accepts x86_64");
    CHECK(!arch_compat("aarch64"), "x86_64 sys rejects aarch64");

    setenv("BULUM_ARCH", "riscv64", 1);
    CHECK(arch_compat("riscv64"), "riscv64 sys accepts riscv64");
    CHECK(!arch_compat("x86_64"), "riscv64 sys rejects x86_64");

    unsetenv("BULUM_ARCH");
    CHECK(sys_arch()[0] != 0, "sys_arch() works without override");
}

static void test_parse(void)
{
    static const char *rec =
        "Package: bterm\n"
        "Status: install ok installed\n"
        "Priority: required\n"
        "Section: base\n"
        "Installed-Size: 700\n"
        "Architecture: aarch64\n"
        "Version: 1.0.0\n"
        "Description: terminal emulator\n"
        "License: MIT\n"
        "Keywords: terminal, display\n"
        "Origin: official\n"
        "Maintainer: Team <t@bulum.local>\n";
    pkg_t p;
    parse_record(&p, (char *)rec, (char *)rec + strlen(rec));
    CHECK(!strcmp(p.name, "bterm"), "parse name=bterm");
    CHECK(!strcmp(p.version, "1.0.0"), "parse version");
    CHECK(!strcmp(p.arch, "aarch64"), "parse arch");
    CHECK(!strcmp(p.license, "MIT"), "parse license");
    CHECK(!strcmp(p.keywords, "terminal, display"), "parse keywords");
    CHECK(!strcmp(p.origin, "official"), "parse origin");
    CHECK(!strcmp(p.maintainer, "Team <t@bulum.local>"), "parse maintainer");
    CHECK(p.size == 700, "parse installed-size");
}

static void test_json(void)
{
    FILE *f = tmpfile();
    CHECK(f != NULL, "tmpfile open");
    json_str(f, "a\"b\\c\nd\te\r");
    fflush(f);
    char buf[64] = { 0 };
    rewind(f);
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = 0;
    fclose(f);
    CHECK(strstr(buf, "\\\"") != NULL, "json escapes double quotes");
    CHECK(strstr(buf, "\\\\") != NULL, "json escapes backslash");
    CHECK(strstr(buf, "\\n") != NULL, "json escapes newline");
    CHECK(strstr(buf, "\\t") != NULL, "json escapes tab");
}

int main(void)
{
    printf("== bpk_coretest: vercmp ==\n");
    test_vercmp();
    printf("== bpk_coretest: arch ==\n");
    test_arch();
    printf("== bpk_coretest: parse ==\n");
    test_parse();
    printf("== bpk_coretest: json ==\n");
    test_json();
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}