/*
 * bsecure - BulumOS security audit (Phase 7)
 *
 * Deterministic, read-only checks of the local system.
 *
 *   bsecure status     short summary
 *   bsecure audit      full audit report
 *
 * Optional --root <dir> prefix for testing against a fixture tree.
 * No system(), no popen(); all checks use direct file inspection.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BSECURE_VERSION "1.0.0"

static const char *ROOT = "";

static const char *rooted(const char *rel)
{
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s", ROOT, rel);
    return buf;
}

static int st(const char *full, struct stat *s)
{
    return lstat(full, s);
}

/* fixture mode: owner checks are meaningless when the audit runs against
 * a build tree owned by a non-root user, so relax uid checks */
static int owner_ok(uid_t uid)
{
    if (getenv("BSECURE_ANY_OWNER"))
        return 1;
    return uid == 0;
}

/* ---------- audit state ---------- */

static int npass, nwarn, ncrit;
static char report[64][256];
static int nreport;

static void emit(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(report[nreport], sizeof(report[0]), fmt, ap);
    va_end(ap);
    nreport++;
    if (!strcmp(tag, "PASS"))
        npass++;
    else if (!strcmp(tag, "WARN"))
        nwarn++;
    else
        ncrit++;
    printf("[%s] %s\n", tag, report[nreport - 1]);
}

/* ---------- checks ---------- */

static void check_shadow(void)
{
    const char *f = rooted("/etc/shadow");
    struct stat s;
    if (st(f, &s) != 0) {
        emit("FAIL", "/etc/shadow missing");
        return;
    }
    if (!S_ISREG(s.st_mode)) {
        emit("FAIL", "/etc/shadow not a regular file");
        return;
    }
    int bad = 0;
    if (!owner_ok(s.st_uid))
        bad = 1;
    if (s.st_mode & 0044)
        bad = 1;
    emit(bad ? "FAIL" : "PASS",
         bad ? "/etc/shadow readable by non-root" : "/etc/shadow permissions");
}

static void check_etc_files(void)
{
    static const char *files[] = {
        "/etc/passwd", "/etc/group", "/etc/hostname", "/etc/fstab",
        "/etc/bulum/network.conf", NULL
    };
    for (int i = 0; files[i]; i++) {
        const char *f = rooted(files[i]);
        struct stat s;
        if (st(f, &s) != 0) {
            emit("WARN", "missing /etc file: %s", files[i]);
            continue;
        }
        int bad = 0;
        if (!owner_ok(s.st_uid))
            bad = 1;
        if (s.st_mode & 0022)
            bad = 1;
        emit(bad ? "FAIL" : "PASS",
             bad ? "%s permissions" : "%s permissions", files[i]);
    }
}

static void check_root_dir(void)
{
    const char *f = rooted("/root");
    struct stat s;
    if (st(f, &s) != 0) {
        emit("WARN", "/root missing");
        return;
    }
    int bad = 0;
    if (!owner_ok(s.st_uid))
        bad = 1;
    if ((s.st_mode & 0077) != 0)
        bad = 1;
    emit(bad ? "WARN" : "PASS",
         bad ? "/root permissions (should be 700)" : "/root permissions");
}

static void check_tmp(void)
{
    const char *f = rooted("/tmp");
    struct stat s;
    if (st(f, &s) != 0) {
        emit("WARN", "/tmp missing");
        return;
    }
    int bad = (s.st_mode & 01000) == 0;
    emit(bad ? "FAIL" : "PASS",
         bad ? "/tmp missing sticky bit" : "/tmp sticky bit");
}

static void check_ssh(void)
{
    /* /etc/dropbear must be root-only; keys must be 600 */
    const char *dir = rooted("/etc/dropbear");
    struct stat s;
    if (st(dir, &s) != 0) {
        emit("WARN", "/etc/dropbear missing");
        return;
    }
    int bad = 0;
    if (!owner_ok(s.st_uid))
        bad = 1;
    if ((s.st_mode & 0077) != 0)
        bad = 1;
    if (!bad) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.')
                    continue;
                if (strstr(e->d_name, "_key")) {
                    char k[1024];
                    snprintf(k, sizeof(k), "%s/%s", dir, e->d_name);
                    struct stat ks;
                    if (lstat(k, &ks) == 0 &&
                        (ks.st_mode & 0077) != 0)
                        bad = 1;
                }
            }
            closedir(d);
        }
    }
    emit(bad ? "FAIL" : "PASS",
         bad ? "SSH host keys protected" : "SSH host keys protected");
    /* authorized_keys */
    const char *ak = rooted("/root/.ssh/authorized_keys");
    if (st(ak, &s) == 0) {
        int b = (s.st_mode & 0077) != 0 || !owner_ok(s.st_uid);
        emit(b ? "FAIL" : "PASS",
             b ? "/root/.ssh/authorized_keys permissions"
               : "/root/.ssh/authorized_keys permissions");
    }
}

static void check_service_users(void)
{
    const char *f = rooted("/etc/passwd");
    FILE *fp = fopen(f, "r");
    if (!fp) {
        emit("WARN", "/etc/passwd unreadable");
        return;
    }
    char line[512];
    int has_httpd = 0, has_root = 0, has_melton = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (feof(fp))
            break;
        line[strcspn(line, "\r\n")] = 0;
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        if (!strcmp(line, "root"))
            has_root = 1;
        else if (!strcmp(line, "httpd"))
            has_httpd = 1;
        else if (!strcmp(line, "melton"))
            has_melton = 1;
    }
    fclose(fp);
    if (!has_root)
        emit("FAIL", "root account missing from /etc/passwd");
    if (!has_httpd)
        emit("WARN", "httpd service user not configured");
    else
        emit("PASS", "httpd service user configured");
    (void)has_melton;
    if (has_root && has_httpd)
        emit("PASS", "service users configured");
}

static void walk_dir(const char *dir, int depth, char *setuid_list,
                     size_t su_sz, int *n_suid)
{
    if (depth > 3)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat s;
        if (lstat(full, &s) != 0)
            continue;
        if (S_ISDIR(s.st_mode))
            walk_dir(full, depth + 1, setuid_list, su_sz, n_suid);
        else if (S_ISREG(s.st_mode) && (s.st_mode & 06000)) {
            if (*n_suid < 32 && su_sz > 0) {
                size_t used = strlen(setuid_list);
                snprintf(setuid_list + used, su_sz - used, "%s\n",
                         full + strlen(ROOT));
                (*n_suid)++;
            }
        }
    }
    closedir(d);
}

static void check_suid(void)
{
    char suid[4096] = "";
    int n = 0;
    static const char *dirs[] = { "/bin", "/sbin", "/usr/bin",
                                  "/usr/sbin", NULL };
    for (int i = 0; dirs[i]; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s%s", ROOT, dirs[i]);
        walk_dir(full, 0, suid, sizeof(suid), &n);
    }
    if (n == 0) {
        emit("PASS", "no unexpected SUID/SGID binaries");
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "%d SUID/SGID binaries found:\n%s",
                 n, suid);
        emit("WARN", msg);
    }
}

static void check_world_writable(void)
{
    static const char *dirs[] = { "/etc", "/bin", "/sbin", "/usr/bin",
                                  "/usr/sbin", NULL };
    char found[2048] = "";
    int n = 0;
    for (int i = 0; dirs[i]; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s%s", ROOT, dirs[i]);
        DIR *d = opendir(full);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char f2[1024];
            snprintf(f2, sizeof(f2), "%.*s/%.*s",
                     (int)sizeof(f2) - 42, full, 30, e->d_name);
            struct stat s;
            if (lstat(f2, &s) != 0)
                continue;
            if ((s.st_mode & 0002) && S_ISREG(s.st_mode)) {
                if (n < 8) {
                    size_t used = strlen(found);
                    snprintf(found + used, sizeof(found) - used, "%s ",
                             f2 + strlen(ROOT));
                }
                n++;
            }
        }
        closedir(d);
    }
    if (n == 0) {
        emit("PASS", "no world-writable files in system dirs");
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "%d world-writable file(s): %s", n,
                 found);
        emit("WARN", msg);
    }
}

static void check_listeners(void)
{
    int telnet = 0, ssh = 0, http = 0, other = 0;
    char others[512] = "";
    static const char *paths[] = { "/proc/net/tcp", "/proc/net/tcp6",
                                   "/proc/net/udp", "/proc/net/udp6" };
    for (int i = 0; i < 4; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s%s", ROOT, paths[i]);
        FILE *f = fopen(full, "r");
        if (!f)
            continue;
        char line[512];
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            continue;
        }
        while (fgets(line, sizeof(line), f)) {
            char local[64] = "";
            unsigned int st = 0;
            if (sscanf(line, "%*d: %63s %*s %x", local, &st) != 2)
                continue;
            if (st != 10 && st != 7)
                continue;
            char *colon = strrchr(local, ':');
            if (!colon)
                continue;
            unsigned int port = 0;
            sscanf(colon + 1, "%x", &port);
            if (port == 23)
                telnet = 1;
            else if (port == 22)
                ssh = 1;
            else if (port == 80)
                http = 1;
            else if (port != 0) {
                if (other < 6) {
                    size_t used = strlen(others);
                    snprintf(others + used, sizeof(others) - used,
                             "%s:%u ", paths[i] + 10, port);
                }
                other++;
            }
        }
        fclose(f);
    }
    if (ssh)
        emit("PASS", "SSH listener (port 22)");
    if (http)
        emit("PASS", "HTTP listener (port 80)");
    if (telnet) {
        emit("WARN", "telnetd enabled (plaintext; use SSH)");
    } else {
        emit("PASS", "no telnet listener");
    }
    if (other)
        emit("WARN", "other listeners: %s", others);
}

static void check_firewall(void)
{
    /* iptables/nft userspace availability */
    if (access(rooted("/sbin/iptables"), X_OK) == 0 ||
        access(rooted("/usr/sbin/iptables"), X_OK) == 0 ||
        access(rooted("/usr/sbin/nft"), X_OK) == 0) {
        emit("PASS", "firewall tooling available");
    } else {
        emit("WARN", "no iptables/nft userspace (kernel netfilter limited)");
    }
}

static void check_enabled_services(void)
{
    const char *f = rooted("/etc/bulum/enabled");
    DIR *d = opendir(f);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (!strcmp(e->d_name, "telnetd"))
            emit("WARN", "telnetd service enabled");
        else if (!strcmp(e->d_name, "httpd"))
            emit("WARN", "httpd service enabled (unauthenticated)");
    }
    closedir(d);
}

/* ---------- commands ---------- */

static void cmd_audit(void)
{
    printf("BulumOS Security Audit\n\n");
    check_shadow();
    check_etc_files();
    check_root_dir();
    check_tmp();
    check_ssh();
    check_service_users();
    check_suid();
    check_world_writable();
    check_listeners();
    check_firewall();
    check_enabled_services();
    printf("\nResult: %d passed, %d warning, %d critical\n",
           npass, nwarn, ncrit);
}

static void cmd_status(void)
{
    check_shadow();
    check_ssh();
    check_service_users();
    check_listeners();
    printf("\nSecurity: %d passed, %d warning, %d critical\n",
           npass, nwarn, ncrit);
}

static void usage(const char *p)
{
    printf("usage: %s <command>\n"
           "  status          quick security summary\n"
           "  audit           full security audit\n"
           "  --root <dir>    inspect a fixture tree instead of /\n"
           "  --help | --version\n", p);
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            ROOT = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            printf("bsecure %s (BulumOS)\n", BSECURE_VERSION);
            return 0;
        } else {
            cmd = argv[i];
        }
    }
    if (!cmd) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(cmd, "status")) {
        cmd_status();
        return (ncrit ? 2 : (nwarn ? 1 : 0));
    }
    if (!strcmp(cmd, "audit")) {
        cmd_audit();
        return (ncrit ? 2 : (nwarn ? 1 : 0));
    }
    usage(argv[0]);
    return 2;
}