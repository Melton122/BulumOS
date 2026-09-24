/*
 * bulum/bpk - BulumOS package manager (Phase 12)
 *
 * Commands:
 *   bpk update                fetch repository indexes
 *   bpk install <pkg|deb>     install a package (resolves dependencies)
 *   bpk remove <pkg>          remove a package (checks reverse deps)
 *   bpk search <term>         search available packages
 *   bpk info <pkg>            show package details
 *   bpk list                  list installed packages
 *   bpk upgrade               upgrade installed packages
 *   bpk clean                 remove cached archives
 *   bpk doctor                verify database + recover interrupted installs
 *   bpk owns <file>           show which package owns a file
 *   bpk verify [pkg]          verify package integrity (md5sums)
 *   bpk repo list             list configured repositories
 *   bpk repo add <n u [a k]]> add a repository
 *   bpk repo remove <name>    remove a repository
 *   bpk repo enable <name>    enable a repository
 *   bpk repo disable <name>   disable a repository
 *   bpk arch                  print system architecture
 *   bpk version               print version
 *   bpk --json <cmd> [args]   machine-readable JSON output
 *
 * Layout (dpkg-compatible database):
 *   /var/lib/dpkg/status             package database (dpkg format)
 *   /var/lib/dpkg/info/<pkg>.list    files owned by package
 *   /var/lib/dpkg/info/<pkg>.md5sums md5 of owned files
 *   /var/lib/bulum/lists/<repo>.Packages   fetched repository index
 *   /var/cache/bpk/                  downloaded .deb/.bpk archives
 *   /var/lib/bulum/stage/            extraction scratch area
 *   /var/lib/bulum/backup/           rollback backups
 *   /var/lib/bulum/tx/steps/         transaction journal
 *   /etc/bulum/repos.conf            repository configuration
 *   /etc/bulum/keys/<name>.pub       RSA repository signing keys
 *
 * Safety:
 *   - every install/remove is journaled under /var/lib/bulum/tx; a commit
 *     marker is only written after all file ops succeed, so an interrupted
 *     operation is rolled back on the next bulum run.
 *   - files not owned by a package are never overwritten
 *   - dependency/conflict checks happen before any file is touched
 *
 * BPK package format (Phase 4, native format, no busybox dpkg-deb needed):
 *   BPK1\n
 *   Package: <name>
 *   Version: <version>
 *   Architecture: <arch>
 *   Maintainer: <...>
 *   Description: <...>
 *   Depends: <...>            (optional)
 *   Conflicts: <...>          (optional)
 *   SHA256: <64 hex>          sha256 of the payload bytes
 *   Size: <decimal>           payload length in bytes
 *   BEGIN PACKAGE SIGNATURE
 *   <RSA PKCS#1 v1.5 SHA-256 signature, hex, wrapped at 64 chars/line>
 *   END PACKAGE SIGNATURE
 *   <blank line>
 *   <payload: uncompressed ustar tar archive>
 *
 * The signature covers the bytes from offset 0 up to (not including) the
 * "BEGIN PACKAGE SIGNATURE" line, i.e. the magic line + metadata + the
 * payload's SHA256 and Size. The payload itself is protected by the embedded
 * SHA256. At install time bulum re-verifies both against /etc/bulum/keys.
 * Entries with Format: bpk in the repository index are fetched and verified
 * the same way as .deb archives (signed Packages index + SHA256/Size fields).
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bulum_crypto.h"

#define DPKG_STATUS "/var/lib/dpkg/status"
#define DPKG_INFO   "/var/lib/dpkg/info"
#define LISTS_DIR   "/var/lib/bulum/lists"
#define CACHE_DIR   "/var/cache/bpk"
#define STAGE_DIR   "/var/lib/bulum/stage"
#define BACKUP_DIR  "/var/lib/bulum/backup"
#define TX_DIR      "/var/lib/bulum/tx"
#define TX_STEPS    "/var/lib/bulum/tx/steps"
#define TX_META     "/var/lib/bulum/tx/meta"
#define TX_COMMIT   "/var/lib/bulum/tx/committed"
#define REPOS_CONF  "/etc/bulum/repos.conf"
#define KEYS_DIR    "/etc/bulum/keys"
#define BULUM_LOG   "/var/log/bulum/bulum.log"

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_YELLOW "\033[33m"
#define C_CYAN   "\033[36m"
#define C_GRAY   "\033[90m"

#define VERSION "1.3"
#define MAX_REPOS 16

static int json_mode = 0;

/* ================= package records ================= */

typedef struct {
    char name[128];
    char status[80];
    char version[96];
    char arch[32];
    char depends[1024];
    char conflicts[1024];
    char provides[1024];
    char desc[256];
    char priority[32];
    char section[64];
    char maintainer[256];
    char license[128];
    char keywords[256];
    char origin[64];
    long size; /* Installed-Size (KB) */
} pkg_t;

typedef struct {
    pkg_t *a;
    int n, cap;
} db_t;

/* an entry from a repository index */
typedef struct {
    pkg_t p;
    char repo[64];
    char filename[512];
    long asize;
    char sha256[65];
    char format[8]; /* "deb" (default) or "bpk" */
} avail_t;

static int pkg_installed(const pkg_t *p)
{
    return strncmp(p->status, "install ok installed", 20) == 0;
}

static int db_find(const db_t *db, const char *name)
{
    for (int i = 0; i < db->n; i++)
        if (!strcmp(db->a[i].name, name))
            return i;
    return -1;
}

static void db_grow(db_t *db)
{
    if (db->n >= db->cap) {
        db->cap = db->cap ? db->cap * 2 : 64;
        db->a = realloc(db->a, db->cap * sizeof(pkg_t));
    }
}

/* ================= small utilities ================= */

static const char *now_stamp(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void ensure_dirs(void)
{
    mkdir_p(DPKG_INFO);
    mkdir_p(LISTS_DIR);
    mkdir_p(CACHE_DIR);
    mkdir_p(STAGE_DIR);
    mkdir_p(BACKUP_DIR);
    mkdir_p(TX_STEPS);
    mkdir_p("/var/log/bulum");
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }

static void bulum_log(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open(BULUM_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf(fd, "[%s] %s\n", now_stamp(), buf);
        close(fd);
    }
}

static char *read_file_alloc(const char *path, long *out_len)
{
    if (out_len)
        *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, n, f);
    buf[got] = 0;
    if (out_len)
        *out_len = got;
    fclose(f);
    return buf;
}

static int run_cmd(const char *fmt, ...)
{
    va_list ap;
    char buf[2048];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int rc = system(buf);
    if (rc == -1)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

/* ================= JSON helpers ================= */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    if (s) {
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                fputc('\\', f);
                fputc((int)c, f);
            } else if (c == '\n') {
                fputs("\\n", f);
            } else if (c == '\r') {
                fputs("\\r", f);
            } else if (c == '\t') {
                fputs("\\t", f);
            } else if (c < 0x20) {
                fprintf(f, "\\u%04x", c);
            } else {
                fputc((int)c, f);
            }
        }
    }
    fputc('"', f);
}

static void json_field(FILE *f, const char *name, const char *val)
{
    fprintf(f, "\n    ");
    json_str(f, name);
    fputs(": ", f);
    json_str(f, val);
}

/* ================= dpkg status db ================= */

/* parse "Field: value" lines in [start,end) into p */
static void parse_record(pkg_t *p, char *start, char *end)
{
    memset(p, 0, sizeof(*p));
    char *line = start;
    while (line < end) {
        char *nl = strchr(line, '\n');
        if (!nl)
            nl = end;
        char *fn = line;
        while (*fn == ' ' || *fn == '\t')
            fn++;
        char *colon = strchr(fn, ':');
        if (colon && colon < nl) {
            int fnamelen = (int)(colon - fn);
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            char *e2 = nl;
            if (e2 > v && e2[-1] == '\r')
                e2--;
            char tmp[1024];
            size_t vl = e2 - v;
            if (vl >= sizeof(tmp))
                vl = sizeof(tmp) - 1;
            memcpy(tmp, v, vl);
            tmp[vl] = 0;
            if (fnamelen == 7 && !strncmp(fn, "Package", 7))
                snprintf(p->name, sizeof(p->name), "%s", tmp);
            else if (fnamelen == 6 && !strncmp(fn, "Status", 6))
                snprintf(p->status, sizeof(p->status), "%s", tmp);
            else if (fnamelen == 7 && !strncmp(fn, "Version", 7))
                snprintf(p->version, sizeof(p->version), "%s", tmp);
            else if (fnamelen == 12 && !strncmp(fn, "Architecture", 12))
                snprintf(p->arch, sizeof(p->arch), "%s", tmp);
            else if (fnamelen == 7 && !strncmp(fn, "Depends", 7))
                snprintf(p->depends, sizeof(p->depends), "%s", tmp);
            else if (fnamelen == 9 && !strncmp(fn, "Conflicts", 9))
                snprintf(p->conflicts, sizeof(p->conflicts), "%s", tmp);
            else if (fnamelen == 8 && !strncmp(fn, "Provides", 8))
                snprintf(p->provides, sizeof(p->provides), "%s", tmp);
            else if (fnamelen == 11 && !strncmp(fn, "Description", 11))
                snprintf(p->desc, sizeof(p->desc), "%s", tmp);
            else if (fnamelen == 8 && !strncmp(fn, "Priority", 8))
                snprintf(p->priority, sizeof(p->priority), "%s", tmp);
            else if (fnamelen == 7 && !strncmp(fn, "Section", 7))
                snprintf(p->section, sizeof(p->section), "%s", tmp);
            else if (fnamelen == 10 && !strncmp(fn, "Maintainer", 10))
                snprintf(p->maintainer, sizeof(p->maintainer), "%s", tmp);
            else if (fnamelen == 14 && !strncmp(fn, "Installed-Size", 14))
                p->size = atol(tmp);
            else if (fnamelen == 7 && !strncmp(fn, "License", 7))
                snprintf(p->license, sizeof(p->license), "%s", tmp);
            else if (fnamelen == 8 && !strncmp(fn, "Keywords", 8))
                snprintf(p->keywords, sizeof(p->keywords), "%s", tmp);
            else if (fnamelen == 6 && !strncmp(fn, "Origin", 6))
                snprintf(p->origin, sizeof(p->origin), "%s", tmp);
        }
        line = nl + 1;
    }
    p->desc[strcspn(p->desc, "\n")] = 0;
    p->desc[strcspn(p->desc, "\r")] = 0;
}

static void db_load(db_t *db)
{
    db->n = 0;
    long n = 0;
    char *buf = read_file_alloc(DPKG_STATUS, &n);
    if (!buf)
        return;
    char *ps = buf;
    while (ps < buf + n) {
        while (ps < buf + n && (*ps == '\n' || *ps == '\r'))
            ps++;
        if (ps >= buf + n)
            break;
        char *pe = strstr(ps, "\n\n");
        char *end = pe ? pe + 1 : buf + n;
        char *lastnl = end;
        while (lastnl > ps && (lastnl[-1] == '\n' || lastnl[-1] == '\r'))
            lastnl--;
        if (lastnl > ps) {
            db_grow(db);
            parse_record(&db->a[db->n], ps, lastnl);
            db->n++;
        }
        if (!pe)
            break;
        ps = pe + 2;
    }
    free(buf);
}

static void db_write(const db_t *db)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", DPKG_STATUS);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < db->n; i++) {
        const pkg_t *p = &db->a[i];
        fprintf(f, "Package: %s\n", p->name);
        fprintf(f, "Status: %s\n",
                p->status[0] ? p->status : "install ok installed");
        if (p->priority[0])
            fprintf(f, "Priority: %s\n", p->priority);
        if (p->section[0])
            fprintf(f, "Section: %s\n", p->section);
        if (p->maintainer[0])
            fprintf(f, "Maintainer: %s\n", p->maintainer);
        if (p->size)
            fprintf(f, "Installed-Size: %ld\n", p->size);
        fprintf(f, "Architecture: %s\n", p->arch[0] ? p->arch : "unknown");
        fprintf(f, "Version: %s\n", p->version);
        if (p->depends[0])
            fprintf(f, "Depends: %s\n", p->depends);
        if (p->conflicts[0])
            fprintf(f, "Conflicts: %s\n", p->conflicts);
        if (p->provides[0])
            fprintf(f, "Provides: %s\n", p->provides);
        if (p->desc[0])
            fprintf(f, "Description: %s\n", p->desc);
        if (p->license[0])
            fprintf(f, "License: %s\n", p->license);
        if (p->keywords[0])
            fprintf(f, "Keywords: %s\n", p->keywords);
        if (p->origin[0])
            fprintf(f, "Origin: %s\n", p->origin);
        fprintf(f, "\n");
    }
    fclose(f);
    rename(tmp, DPKG_STATUS);
}

static void db_add_or_update(db_t *db, const pkg_t *p)
{
    int i = db_find(db, p->name);
    if (i < 0) {
        db_grow(db);
        i = db->n;
        db->n++;
        memset(&db->a[i], 0, sizeof(pkg_t));
    }
    db->a[i] = *p;
}

static void db_remove_pkg(db_t *db, const char *name)
{
    int i = db_find(db, name);
    if (i < 0)
        return;
    memmove(&db->a[i], &db->a[i + 1], (db->n - i - 1) * sizeof(pkg_t));
    db->n--;
}

/* ================= version comparison (dpkg algorithm) ================= */

static int corder(int c)
{
    if (c == '~')
        return -1;
    if (!c)
        return 0;
    if (c >= '0' && c <= '9')
        return 0x100 + c;
    if (c >= 'a' && c <= 'z')
        return c;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return 0;
}

static int verrevcmp(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    while (*a || *b) {
        int first_diff = 0;
        while ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
            int va = corder((unsigned char)*a);
            int vb = corder((unsigned char)*b);
            if (va != vb)
                return va - vb;
            a++;
            b++;
        }
        while (*a == '0')
            a++;
        while (*b == '0')
            b++;
        while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            if (!first_diff)
                first_diff = (unsigned char)*a - (unsigned char)*b;
            a++;
            b++;
        }
        if (isdigit((unsigned char)*a))
            return 1;
        if (isdigit((unsigned char)*b))
            return -1;
        if (first_diff)
            return first_diff;
    }
    return 0;
}

static int vercmp(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    long ea = 0, eb = 0;
    const char *ca = strchr(a, ':');
    const char *cb = strchr(b, ':');
    if (ca) {
        ea = atol(a);
        a = ca + 1;
    }
    if (cb) {
        eb = atol(b);
        b = cb + 1;
    }
    if (ea != eb)
        return ea < eb ? -1 : 1;
    char *ua = strdup(a), *ub = strdup(b);
    char *da = strrchr(ua, '-'), *db2 = strrchr(ub, '-');
    char *ra = da ? da + 1 : NULL, *rb = db2 ? db2 + 1 : NULL;
    if (da)
        *da = 0;
    if (db2)
        *db2 = 0;
    int r = verrevcmp(ua, ub);
    if (!r)
        r = verrevcmp(ra, rb);
    free(ua);
    free(ub);
    return r;
}

/* ================= dependency handling ================= */

typedef struct {
    char name[64];   /* virtual package name */
    char provider[128]; /* installed package providing it */
} prov_t;

typedef struct { prov_t a[512]; int n; } provs_t;

/* collect (virtual -> provider) pairs from installed packages */
static void build_provides(const db_t *db, provs_t *out)
{
    out->n = 0;
    for (int i = 0; i < db->n; i++) {
        const pkg_t *p = &db->a[i];
        if (!pkg_installed(p) || !p->provides[0])
            continue;
        char *cpy = strdup(p->provides);
        char *tok = strtok(cpy, " \t,");
        while (tok && out->n < 512) {
            if (tok[0] != '(' && strcmp(tok, "=") != 0 && strcmp(tok, ">=") != 0 &&
                strcmp(tok, "<=") != 0 && strcmp(tok, ">>") != 0 && strcmp(tok, "<<") != 0) {
                snprintf(out->a[out->n].name, sizeof(out->a[0].name), "%s", tok);
                snprintf(out->a[out->n].provider, sizeof(out->a[0].provider), "%s", p->name);
                out->n++;
            }
            tok = strtok(NULL, " \t,");
        }
        free(cpy);
    }
}

static int rel_ok(int cmp, const char *op)
{
    if (!strcmp(op, ">="))
        return cmp >= 0;
    if (!strcmp(op, ">") || !strcmp(op, ">>"))
        return cmp > 0;
    if (!strcmp(op, "<="))
        return cmp <= 0;
    if (!strcmp(op, "<") || !strcmp(op, "<<"))
        return cmp < 0;
    if (!strcmp(op, "="))
        return cmp == 0;
    return 1;
}

/* is dependency alternative "name (op ver)" satisfied? */
static int alt_satisfied(const db_t *db, const provs_t *pv, const char *name,
                         const char *op, const char *ver)
{
    int i = db_find(db, name);
    if (i >= 0 && pkg_installed(&db->a[i])) {
        if (!op)
            return 1;
        return rel_ok(vercmp(db->a[i].version, ver), op);
    }
    for (int j = 0; j < pv->n; j++)
        if (!strcmp(pv->a[j].name, name))
            return 1;
    return 0;
}

/* parse "name (op ver)" from an alternative token (no spaces allowed) */
static void parse_constraint(const char *tok, size_t tlen, char *name, size_t nsz,
                             char *op, size_t osz, char *ver, size_t vsz)
{
    name[0] = op[0] = ver[0] = 0;
    size_t i = 0;
    while (i < tlen && tok[i] != '(' && tok[i] != ' ' && tok[i] != '\t')
        i++;
    size_t nl = i;
    while (i < tlen && (tok[i] == ' ' || tok[i] == '\t'))
        i++;
    if (i < tlen && tok[i] == '(') {
        i++;
        while (i < tlen && (tok[i] == ' ' || tok[i] == '\t'))
            i++;
        size_t ol = 0;
        while (i < tlen && tok[i] != ' ' && tok[i] != ')' && ol < osz - 1)
            op[ol++] = tok[i++];
        op[ol] = 0;
        while (i < tlen && (tok[i] == ' ' || tok[i] == '\t'))
            i++;
        size_t vl = 0;
        while (i < tlen && tok[i] != ')' && vl < vsz - 1)
            ver[vl++] = tok[i++];
        ver[vl] = 0;
    }
    if (nl >= nsz)
        nl = nsz - 1;
    memcpy(name, tok, nl);
    name[nl] = 0;
}

/* check that every Depends clause is satisfied; -1 + reason if not */
static int deps_ok(const db_t *db, const provs_t *pv, const char *depends,
                   char *reason, size_t rsz)
{
    if (!depends || !*depends)
        return 0;
    char *cpy = strdup(depends);
    char *clause = strtok(cpy, ",");
    while (clause) {
        while (*clause == ' ' || *clause == '\t')
            clause++;
        int satisfied = 0;
        char *alt = strtok(clause, "|");
        while (alt) {
            while (*alt == ' ' || *alt == '\t')
                alt++;
            size_t tlen = strlen(alt);
            while (tlen && isspace((unsigned char)alt[tlen - 1]))
                tlen--;
            char nm[128], op[8], ver[96];
            parse_constraint(alt, tlen, nm, sizeof(nm), op, sizeof(op), ver, sizeof(ver));
            if (alt_satisfied(db, pv, nm, op[0] ? op : NULL, ver[0] ? ver : NULL)) {
                satisfied = 1;
                break;
            }
            alt = strtok(NULL, "|");
        }
        if (!satisfied) {
            snprintf(reason, rsz, "%s", clause);
            free(cpy);
            return -1;
        }
        clause = strtok(NULL, ",");
    }
    free(cpy);
    return 0;
}

/* would removing 'name' (and its provides) break an installed package?
 * returns the dependent package name, or NULL. */
static const char *reverse_dep_break(const db_t *db, const provs_t *pv,
                                     const char *name)
{
    for (int i = 0; i < db->n; i++) {
        const pkg_t *p = &db->a[i];
        if (!pkg_installed(p) || !strcmp(p->name, name) || !p->depends[0])
            continue;
        char *cpy = strdup(p->depends);
        char *clause = strtok(cpy, ",");
        while (clause) {
            int all_dead = 1;
            char *alt = strtok(clause, "|");
            while (alt) {
                while (*alt == ' ' || *alt == '\t')
                    alt++;
                size_t tlen = strlen(alt);
                while (tlen && isspace((unsigned char)alt[tlen - 1]))
                    tlen--;
                char nm[128], op[8], ver[96];
                parse_constraint(alt, tlen, nm, sizeof(nm), op, sizeof(op), ver, sizeof(ver));
                /* alternative dies if it names the removed package, or a
                 * virtual only it provides */
                int dead = !strcmp(nm, name);
                if (!dead)
                    for (int j = 0; j < pv->n; j++)
                        if (!strcmp(pv->a[j].name, nm) &&
                            !strcmp(pv->a[j].provider, name)) {
                            dead = 1;
                            break;
                        }
                if (!dead) {
                    all_dead = 0;
                    break;
                }
                alt = strtok(NULL, "|");
            }
            if (all_dead) {
                free(cpy);
                return p->name;
            }
            clause = strtok(NULL, ",");
        }
        free(cpy);
    }
    return NULL;
}

/* ================= ownership map ================= */

typedef struct {
    char path[512];
    char owner[128];
    int is_dir;
} own_t;

static own_t *own_map;
static int nowner, owncap;

static void own_add(const char *path, const char *owner, int is_dir)
{
    if (!path[0] || !strcmp(path, "/"))
        return;
    if (nowner >= owncap) {
        owncap = owncap ? owncap * 2 : 1024;
        own_map = realloc(own_map, owncap * sizeof(own_t));
    }
    snprintf(own_map[nowner].path, sizeof(own_map[0].path), "%s", path);
    snprintf(own_map[nowner].owner, sizeof(own_map[0].owner), "%s", owner);
    own_map[nowner].is_dir = is_dir;
    nowner++;
}

static const char *owner_of(const char *path)
{
    for (int i = 0; i < nowner; i++)
        if (!strcmp(own_map[i].path, path))
            return own_map[i].owner;
    return NULL;
}

/* how many other packages also own this path? */
static int other_owners(const char *path, const char *self)
{
    int c = 0;
    for (int i = 0; i < nowner; i++)
        if (!strcmp(own_map[i].path, path) && strcmp(own_map[i].owner, self))
            c++;
    return c;
}

static void build_own_map(const db_t *db)
{
    nowner = 0;
    for (int i = 0; i < db->n; i++) {
        const pkg_t *p = &db->a[i];
        if (!pkg_installed(p))
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.list", DPKG_INFO, p->name);
        long n = 0;
        char *buf = read_file_alloc(path, &n);
        if (!buf)
            continue;
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = 0;
            if (line[0]) {
                size_t tl = strlen(line);
                int is_dir = (tl > 1 && line[tl - 1] == '/');
                own_add(line, p->name, is_dir);
            }
            line = nl ? nl + 1 : NULL;
        }
        free(buf);
    }
}

/* ================= staged file walk ================= */

typedef struct {
    char path[512];
    int is_dir;
    off_t size;
    mode_t mode;
} ent_t;

static ent_t *ents;
static int nent, entcap;

static void ent_add(const char *rel, int is_dir, off_t size, mode_t mode)
{
    if (nent >= entcap) {
        entcap = entcap ? entcap * 2 : 1024;
        ents = realloc(ents, entcap * sizeof(ent_t));
    }
    snprintf(ents[nent].path, sizeof(ents[0].path), "%s", rel);
    ents[nent].is_dir = is_dir;
    ents[nent].size = size;
    ents[nent].mode = mode;
    nent++;
}

static void walk_dir(const char *fsbase, const char *rel)
{
    char fspath[512];
    if (rel[0])
        snprintf(fspath, sizeof(fspath), "%s/%s", fsbase, rel);
    else
        snprintf(fspath, sizeof(fspath), "%s", fsbase);
    DIR *d = opendir(fspath);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char relp[512];
        if (rel[0])
            snprintf(relp, sizeof(relp), "%s/%s", rel, e->d_name);
        else
            snprintf(relp, sizeof(relp), "%s", e->d_name);
        char fsp[512];
        snprintf(fsp, sizeof(fsp), "%s/%s", fspath, e->d_name);
        struct stat st;
        if (lstat(fsp, &st) != 0)
            continue;
        if (S_ISREG(st.st_mode))
            ent_add(relp, 0, st.st_size, st.st_mode & 07777);
        else if (S_ISDIR(st.st_mode)) {
            ent_add(relp, 1, 0, st.st_mode & 07777);
            walk_dir(fsbase, relp);
        } else if (S_ISLNK(st.st_mode))
            ent_add(relp, 0, 0, S_IFLNK | 0777);
    }
    closedir(d);
}

static void stage_walk(const char *stage)
{
    nent = 0;
    walk_dir(stage, "");
}

static void rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
                rmdir_recursive(p);
            else
                unlink(p);
        }
        closedir(d);
    }
    rmdir(path);
}

/* ================= file copy ================= */

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[32768];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, n);
        if (w != n) {
            rc = -1;
            break;
        }
    }
    if (n < 0)
        rc = -1;
    fsync(out);
    close(out);
    close(in);
    if (rc == 0)
        chmod(dst, mode & 07777);
    return rc;
}

static int copy_link(const char *src, const char *dst)
{
    char tgt[512];
    ssize_t n = readlink(src, tgt, sizeof(tgt) - 1);
    if (n < 0)
        return -1;
    tgt[n] = 0;
    unlink(dst);
    return symlink(tgt, dst);
}

/* ================= transactions ================= */

static int tx_seq;

static void tx_begin(const char *type, const char *pkg, const char *ver)
{
    ensure_dirs();
    rmdir_recursive(TX_DIR);
    mkdir_p(TX_STEPS);
    int fd = open(TX_META, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dprintf(fd, "TYPE %s\nPKG %s\nVER %s\n", type, pkg, ver);
        close(fd);
    }
    tx_seq = 0;
}

/* record a completed step (tab separated, lines are atomically renamed in) */
static void tx_write(const char *fmt, ...)
{
    va_list ap;
    char line[1200];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    unsigned int rnd = (unsigned int)(getpid() ^ (int)time(NULL));
    char tmp[600], fin[600];
    tx_seq++;
    snprintf(tmp, sizeof(tmp), "%s/.t%u%u", TX_STEPS, rnd, tx_seq);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
    fsync(fd);
    close(fd);
    snprintf(fin, sizeof(fin), "%s/%06d", TX_STEPS, tx_seq);
    rename(tmp, fin);
}

/* commit marker: all file ops done, database update allowed to proceed */
static void tx_commit_marker(void)
{
    int fd = open(TX_COMMIT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}

static void tx_clear(void)
{
    rmdir_recursive(TX_DIR);
}

/* roll back any interrupted transaction. Returns 1 if something was undone */
static int recover_tx(void)
{
    if (!file_exists(TX_STEPS))
        return 0;
    if (file_exists(TX_COMMIT)) {
        printf(C_YELLOW "  bulum: leftover committed transaction cleaned\n" C_RESET);
        tx_clear();
        return 0;
    }
    char names[4096][64];
    int nn = 0;
    DIR *d = opendir(TX_STEPS);
    if (!d) {
        tx_clear();
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || nn >= 4096)
            continue;
        snprintf(names[nn], sizeof(names[0]), "%s", e->d_name);
        nn++;
    }
    closedir(d);
    /* descending zero-padded order = newest step first */
    for (int i = 1; i < nn; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) < 0; j--) {
            char t[64];
            snprintf(t, sizeof(t), "%s", names[j - 1]);
            snprintf(names[j - 1], sizeof(names[0]), "%s", names[j]);
            snprintf(names[j], sizeof(names[0]), "%s", t);
        }
    int rolled = 0;
    for (int i = 0; i < nn; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", TX_STEPS, names[i]);
        long n = 0;
        char *buf = read_file_alloc(p, &n);
        if (!buf)
            continue;
        char *line = buf;
        if (!strncmp(line, "W\t", 2)) {
            char dst[512], src[512], bak[512];
            if (sscanf(line + 2, "%511[^\t]\t%511[^\t]\t%511[^\n]", dst, src, bak) == 3) {
                if (strcmp(bak, "-") != 0 && bak[0]) {
                    copy_file(bak, dst, 0644);
                    printf(C_YELLOW "    undo: restored %s\n" C_RESET, dst);
                } else {
                    unlink(dst);
                    printf(C_YELLOW "    undo: removed %s\n" C_RESET, dst);
                }
                rolled = 1;
            }
        } else if (!strncmp(line, "M\t", 2)) {
            char dir[512];
            if (sscanf(line + 2, "%511[^\n]", dir) == 1) {
                rmdir(dir);
                rolled = 1;
            }
        } else if (!strncmp(line, "R\t", 2)) {
            char dst[512], bak[512];
            if (sscanf(line + 2, "%511[^\t]\t%511[^\n]", dst, bak) == 2) {
                if (strcmp(bak, "-") != 0 && bak[0]) {
                    copy_file(bak, dst, 0644);
                    printf(C_YELLOW "    undo: restored %s\n" C_RESET, dst);
                }
                rolled = 1;
            }
        } else if (!strncmp(line, "D\t", 2)) {
            char dir[512];
            if (sscanf(line + 2, "%511[^\n]", dir) == 1) {
                mkdir_p(dir);
                rolled = 1;
            }
        }
        free(buf);
    }
    if (rolled)
        printf(C_YELLOW "  bulum: rolled back interrupted operation (%d steps)\n" C_RESET,
               nn);
    bulum_log("recovered interrupted transaction (%d steps)", nn);
    tx_clear();
    return rolled;
}

/* ================= repositories ================= */

typedef struct {
    char name[64];
    char url[256];
    char arch[32];
    char key[256];
    int insecure;
    int enabled;
} repo_t;

static repo_t repos[MAX_REPOS];
static int nrepos;

static void repo_load(void)
{
    nrepos = 0;
    FILE *f = fopen(REPOS_CONF, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && nrepos < MAX_REPOS) {
        char *c = strchr(line, '#');
        if (c)
            *c = 0;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] == 0)
            continue;
        char name[64], url[256], arch[32], key[256];
        char flags[256] = "";
        int n = sscanf(line, "%63s %255s %31s %255s %255[^\n]",
                       name, url, arch, key, flags);
        if (n < 2)
            continue;
        snprintf(repos[nrepos].name, sizeof(repos[0].name), "%s", name);
        snprintf(repos[nrepos].url, sizeof(repos[0].url), "%s", url);
        snprintf(repos[nrepos].arch, sizeof(repos[0].arch), "%s", arch);
        if (n >= 4 && strcmp(key, "-") != 0)
            snprintf(repos[nrepos].key, sizeof(repos[0].key), "%s", key);
        else
            repos[nrepos].key[0] = 0;
        repos[nrepos].insecure = (n >= 5 && strstr(flags, "insecure")) ? 1 : 0;
        repos[nrepos].enabled = (n >= 5 && strstr(flags, "disabled")) ? 0 : 1;
        nrepos++;
    }
    fclose(f);
}

static const repo_t *repo_find(const char *name)
{
    for (int i = 0; i < nrepos; i++)
        if (!strcmp(repos[i].name, name))
            return &repos[i];
    return NULL;
}

static int wget(const char *url, const char *out, const char *extra)
{
    char cmd[2048];
    if (extra && extra[0])
        snprintf(cmd, sizeof(cmd), "wget -q -T 45 %s -O \"%s\" \"%s\"", extra, out, url);
    else
        snprintf(cmd, sizeof(cmd), "wget -q -T 45 -O \"%s\" \"%s\"", out, url);
    return run_cmd("%s", cmd);
}

/* ================= available packages (indexes) ================= */

static avail_t *avail;
static int navail, avcap;

static void avail_grow(void)
{
    if (navail >= avcap) {
        avcap = avcap ? avcap * 2 : 256;
        avail = realloc(avail, avcap * sizeof(avail_t));
    }
}

static void avail_clear(void)
{
    navail = 0;
}

/* parse a whole Packages file into the avail list */
static void parse_packages(const char *path, const char *repo)
{
    long n = 0;
    char *buf = read_file_alloc(path, &n);
    if (!buf)
        return;
    char *ps = buf;
    while (ps < buf + n) {
        while (ps < buf + n && (*ps == '\n' || *ps == '\r'))
            ps++;
        if (ps >= buf + n)
            break;
        char *pe = strstr(ps, "\n\n");
        char *end = pe ? pe + 1 : buf + n;
        char *lastnl = end;
        while (lastnl > ps && (lastnl[-1] == '\n' || lastnl[-1] == '\r'))
            lastnl--;
        if (lastnl > ps) {
            int idx = navail;
            avail_grow();
            memset(&avail[idx], 0, sizeof(avail_t));
            parse_record(&avail[idx].p, ps, lastnl);
            if (avail[idx].p.name[0]) {
                snprintf(avail[idx].repo, sizeof(avail[0].repo), "%s", repo);
                navail++;
            }
            /* archive-specific fields (Filename/Size/SHA256/Format) come from
             * the same paragraph so their mapping can never drift */
            char *line = ps;
            while (line < lastnl) {
                char *nl = strchr(line, '\n');
                if (!nl)
                    nl = lastnl;
                char *f = line;
                while (*f == ' ' || *f == '\t')
                    f++;
                char *v = NULL;
                if (!strncmp(f, "Filename:", 9))
                    v = f + 9;
                else if (!strncmp(f, "Size:", 5))
                    v = f + 5;
                else if (!strncmp(f, "SHA256:", 7))
                    v = f + 7;
                else if (!strncmp(f, "Format:", 7))
                    v = f + 7;
                if (v) {
                    while (*v == ' ' || *v == '\t')
                        v++;
                    if (v < nl) {
                        char tmp[640];
                        size_t vl = nl - v;
                        if (vl >= sizeof(tmp))
                            vl = sizeof(tmp) - 1;
                        memcpy(tmp, v, vl);
                        tmp[vl] = 0;
                        if (!strncmp(f, "Filename:", 9))
                            sscanf(tmp, "%511s", avail[idx].filename);
                        else if (!strncmp(f, "SHA256:", 7))
                            sscanf(tmp, "%64s", avail[idx].sha256);
                        else if (!strncmp(f, "Size:", 5))
                            avail[idx].asize = atol(tmp);
                        else if (!strncmp(f, "Format:", 7))
                            sscanf(tmp, "%7s", avail[idx].format);
                    }
                }
                line = nl + 1;
            }
            if (!avail[idx].format[0])
                snprintf(avail[idx].format, sizeof(avail[0].format), "%s", "deb");
        }
        if (!pe)
            break;
        ps = pe + 2;
    }
    free(buf);
}

static void avail_load_all(void)
{
    avail_clear();
    repo_load();
    DIR *d = opendir(LISTS_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char *dot = strstr(e->d_name, ".Packages");
        if (!dot)
            continue;
        char name[64];
        size_t nl = (size_t)(dot - e->d_name);
        if (nl >= sizeof(name))
            nl = sizeof(name) - 1;
        memcpy(name, e->d_name, nl);
        name[nl] = 0;
        const repo_t *r = repo_find(name);
        if (r && !r->enabled)
            continue; /* skip indexes from disabled repositories */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", LISTS_DIR, e->d_name);
        parse_packages(path, name);
    }
    closedir(d);
}

/* ================= architecture detection ================= */

static const char *sys_arch(void)
{
    const char *env = getenv("BULUM_ARCH");
    if (env && env[0])
        return env;
    static char m[32];
    struct utsname u;
    if (uname(&u) == 0) {
        if (!strcmp(u.machine, "aarch64") || !strcmp(u.machine, "arm64"))
            return "aarch64";
        if (!strcmp(u.machine, "x86_64") || !strcmp(u.machine, "amd64"))
            return "x86_64";
        snprintf(m, sizeof(m), "%s", u.machine);
        return m;
    }
    return "unknown";
}

static int arch_compat(const char *pkg_arch)
{
    if (!pkg_arch || !pkg_arch[0])
        return 0;
    if (!strcmp(pkg_arch, "all") || !strcmp(pkg_arch, "any"))
        return 1;
    const char *sys = sys_arch();
    if (!strcmp(sys, "aarch64"))
        return !strcmp(pkg_arch, "aarch64") || !strcmp(pkg_arch, "arm64");
    if (!strcmp(sys, "x86_64"))
        return !strcmp(pkg_arch, "x86_64") || !strcmp(pkg_arch, "amd64");
    return !strcmp(pkg_arch, sys);
}

static avail_t *avail_find_best(const char *name, int require_arch)
{
    avail_t *best = NULL;
    for (int i = 0; i < navail; i++) {
        if (strcmp(avail[i].p.name, name))
            continue;
        if (require_arch && !arch_compat(avail[i].p.arch))
            continue;
        if (!best || vercmp(avail[i].p.version, best->p.version) > 0)
            best = &avail[i];
    }
    return best;
}

/* find the best available candidate satisfying "name [op ver]" */
static avail_t *avail_find_matching(const char *name, const char *op, const char *ver)
{
    avail_t *best = NULL;
    for (int i = 0; i < navail; i++) {
        if (strcmp(avail[i].p.name, name))
            continue;
        if (!arch_compat(avail[i].p.arch))
            continue;
        if (op && op[0] && ver && ver[0] &&
            !rel_ok(vercmp(avail[i].p.version, ver), op))
            continue;
        if (!best || vercmp(avail[i].p.version, best->p.version) > 0)
            best = &avail[i];
    }
    return best;
}

/* ================= dependency resolution planner ================= */

#define PLAN_MAX 64
#define DEP_DEPTH 32

static avail_t *plan_arr[PLAN_MAX];
static char plan_names[PLAN_MAX][128];
static char plan_provides[PLAN_MAX][1024];
static int plan_n;
static int plan_visiting[PLAN_MAX];

static void plan_clear(void)
{
    plan_n = 0;
    memset(plan_names, 0, sizeof(plan_names));
    memset(plan_provides, 0, sizeof(plan_provides));
    memset(plan_visiting, 0, sizeof(plan_visiting));
}

static int plan_has_name(const char *name)
{
    for (int i = 0; i < plan_n; i++)
        if (!strcmp(plan_names[i], name))
            return 1;
    return 0;
}

static void plan_append(avail_t *a)
{
    if (plan_n >= PLAN_MAX)
        return;
    plan_arr[plan_n] = a;
    snprintf(plan_names[plan_n], sizeof(plan_names[0]), "%s", a->p.name);
    snprintf(plan_provides[plan_n], sizeof(plan_provides[0]), "%s",
             a->p.provides[0] ? a->p.provides : "");
    plan_n++;
}

/* is this dependency alternative satisfied by an installed package/provider
 * or by a package already scheduled in the plan? */
static int plan_alt_satisfied(const db_t *db, const provs_t *pv,
                              const char *name, const char *op, const char *ver)
{
    if (alt_satisfied(db, pv, name, op, ver))
        return 1;
    for (int i = 0; i < plan_n; i++) {
        if (!strcmp(plan_names[i], name)) {
            if (!op || !ver)
                return 1;
            int c = vercmp(plan_arr[i]->p.version, ver);
            if (rel_ok(c, op))
                return 1;
        }
        if (plan_provides[i][0]) {
            char *cpy = strdup(plan_provides[i]);
            char *tok = strtok(cpy, " \t,");
            while (tok) {
                if (tok[0] != '(' && strcmp(tok, "=") && strcmp(tok, ">=") &&
                    strcmp(tok, "<=") && strcmp(tok, ">>") && strcmp(tok, "<<"))
                    if (!strcmp(tok, name)) {
                        free(cpy);
                        return 1;
                    }
                tok = strtok(NULL, " \t,");
            }
            free(cpy);
        }
    }
    return 0;
}

/* depth-first, post-order: dependencies are scheduled before their parent.
 * returns 0 on success (plan may or may not contain 'cur'), -1 unsatisfiable. */
static int resolve_pkg(avail_t *cur, const db_t *db, const provs_t *pv, int depth)
{
    if (depth > DEP_DEPTH) {
        printf(C_RED "  bulum: dependency depth limit (%d) exceeded\n" C_RESET,
               DEP_DEPTH);
        return -1;
    }
    if (plan_has_name(cur->p.name))
        return 0;
    int vix = plan_n; /* visiting index; may move */
    plan_visiting[vix] = 1;

    if (cur->p.depends[0]) {
        char *cpy = strdup(cur->p.depends);
        char *clause = strtok(cpy, ",");
        while (clause) {
            while (*clause == ' ' || *clause == '\t')
                clause++;
            int satisfied = 0;
            avail_t *picked = NULL;
            char *alt = strtok(clause, "|");
            while (alt) {
                while (*alt == ' ' || *alt == '\t')
                    alt++;
                size_t tlen = strlen(alt);
                while (tlen && isspace((unsigned char)alt[tlen - 1]))
                    tlen--;
                char nm[128], op[8], ver[96];
                parse_constraint(alt, tlen, nm, sizeof(nm), op, sizeof(op),
                                 ver, sizeof(ver));
                if (plan_alt_satisfied(db, pv, nm, op[0] ? op : NULL,
                                       ver[0] ? ver : NULL)) {
                    satisfied = 1;
                    break;
                }
                if (!picked)
                    picked = avail_find_matching(nm, op[0] ? op : NULL,
                                                 ver[0] ? ver : NULL);
                alt = strtok(NULL, "|");
            }
            if (!satisfied && picked) {
                if (resolve_pkg(picked, db, pv, depth + 1) != 0) {
                    free(cpy);
                    return -1;
                }
                satisfied = 1;
            }
            if (!satisfied) {
                printf(C_RED "  bulum: ERROR: package '%s' requires '%s'\n" C_RESET,
                       cur->p.name, clause);
                printf(C_YELLOW "         no available candidate satisfies it\n" C_RESET);
                free(cpy);
                return -1;
            }
            clause = strtok(NULL, ",");
        }
        free(cpy);
    }

    plan_append(cur); /* post-order: deps already appended */
    return 0;
}

/* build an install plan rooted at 'cand' (post-order, deps first).
 * Returns the number of packages to install, or -1 on unsatisfiable deps. */
static int plan_resolve(avail_t *cand)
{
    db_t db = { 0 };
    db_load(&db);
    provs_t pv;
    build_provides(&db, &pv);
    plan_clear();
    if (resolve_pkg(cand, &db, &pv, 0) != 0)
        return -1;
    return plan_n;
}

/* ================= archive fetch ================= */

static int fetch_pkg(avail_t *a, char *out, size_t outsz)
{
    const char *ext = !strcmp(a->format, "bpk") ? "bpk" : "deb";
    snprintf(out, outsz, "%s/%s_%s_%s.%s", CACHE_DIR, a->p.name, a->p.version,
             a->p.arch, ext);
    if (file_exists(out)) {
        unsigned char d[32];
        char hex[65];
        sha256_file(out, d);
        sha256_hex(d, hex);
        if (!a->sha256[0] || !strcmp(hex, a->sha256))
            return 0;
        unlink(out);
    }
    const repo_t *r = repo_find(a->repo);
    if (!r) {
        printf(C_RED "  bulum: unknown repository '%s'\n" C_RESET, a->repo);
        return -1;
    }
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s", r->url, a->filename);
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.part", out);
    printf(C_CYAN "  download %s\n" C_RESET, a->filename);
    if (wget(url, tmp, r->insecure ? "--no-check-certificate" : NULL) != 0) {
        printf(C_RED "  bulum: download failed: %s\n" C_RESET, url);
        unlink(tmp);
        return -1;
    }
    if (a->asize > 0) {
        struct stat st;
        if (stat(tmp, &st) == 0 && st.st_size != a->asize) {
            printf(C_RED "  bulum: size mismatch (%lld != %ld)\n" C_RESET,
                   (long long)st.st_size, a->asize);
            unlink(tmp);
            return -1;
        }
    }
    if (a->sha256[0]) {
        unsigned char d[32];
        char hex[65];
        sha256_file(tmp, d);
        sha256_hex(d, hex);
        if (strcmp(hex, a->sha256)) {
            printf(C_RED "  bulum: sha256 mismatch (%s != %s)\n" C_RESET, hex, a->sha256);
            unlink(tmp);
            return -1;
        }
    }
    rename(tmp, out);
    return 0;
}

/* ================= install ================= */

static char stage_path[512];

static int extract_deb(const char *deb, const char *stage)
{
    rmdir_recursive(stage);
    mkdir_p(stage);
    if (run_cmd("dpkg-deb -x \"%s\" \"%s\"", deb, stage) != 0) {
        printf(C_RED "  bulum: cannot extract %s\n" C_RESET, deb);
        return -1;
    }
    return 0;
}

/* control from a .deb: busybox dpkg-deb has no -I (info); -f prints the
 * whole control paragraph on both busybox and GNU dpkg-deb. */
static int control_from_deb(const char *deb, pkg_t *p)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "dpkg-deb -f \"%s\"", deb);
    FILE *f = popen(cmd, "r");
    if (!f)
        return -1;
    char buf[16384];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = 0;
    int rc = pclose(f);
    if (rc != 0)
        return -1;
    parse_record(p, buf, buf + got);
    if (!p->name[0])
        return -1;
    return 0;
}

/* shared pre-flight: already-installed check + deps + conflicts.
 * returns 0 = proceed, 1 = abort, 2 = already installed (no-op) */
static int preinstall_checks(const pkg_t *ctrl, int force)
{
    db_t db = { 0 };
    db_load(&db);
    provs_t pv;
    build_provides(&db, &pv);

    if (ctrl->arch[0] && !arch_compat(ctrl->arch)) {
        printf(C_RED "  bulum: ERROR: package '%s' is built for %s;\n"
               "         current system architecture is %s\n" C_RESET,
               ctrl->name, ctrl->arch, sys_arch());
        return 1;
    }

    int installed_idx = db_find(&db, ctrl->name);
    pkg_t *inst = installed_idx >= 0 ? &db.a[installed_idx] : NULL;
    if (inst && pkg_installed(inst) && !force &&
        vercmp(inst->version, ctrl->version) >= 0) {
        printf(C_YELLOW "  bulum: %s %s already installed\n" C_RESET,
               ctrl->name, inst->version);
        return 2;
    }

    char reason[256] = "";
    if (deps_ok(&db, &pv, ctrl->depends, reason, sizeof(reason)) != 0) {
        printf(C_RED "  bulum: unsatisfied dependency: %s requires %s\n" C_RESET,
               ctrl->name, reason);
        if (!force)
            return 1;
        printf(C_YELLOW "  bulum: --force: proceeding anyway\n" C_RESET);
    }

    if (ctrl->conflicts[0] && !force) {
        char *cpy = strdup(ctrl->conflicts);
        char *tok = strtok(cpy, " \t,");
        while (tok) {
            if (tok[0] != '(' && strcmp(tok, "=") && strcmp(tok, ">=") &&
                strcmp(tok, "<=") && strcmp(tok, ">>") && strcmp(tok, "<<")) {
                int i = db_find(&db, tok);
                if (i >= 0 && pkg_installed(&db.a[i])) {
                    printf(C_RED "  bulum: %s conflicts with installed %s\n" C_RESET,
                           ctrl->name, tok);
                    free(cpy);
                    return 1;
                }
            }
            tok = strtok(NULL, " \t,");
        }
        free(cpy);
    }
    return 0;
}

/* shared post-extraction install: plan + journal + db + file lists.
 * The caller must have materialized the archive contents under stage_path
 * (dpkg-deb -x for .deb, native ustar extraction for .bpk). */
static int install_staged(const pkg_t *ctrl, int force)
{
    stage_walk(stage_path);
    db_t db = { 0 };
    db_load(&db);
    build_own_map(&db);

    /* plan: refuse to clobber anything unmanaged or owned by another pkg */
    long total_size = 0;
    for (int i = 0; i < nent; i++) {
        char dst[512];
        snprintf(dst, sizeof(dst), "/%s", ents[i].path);
        if (ents[i].is_dir)
            continue;
        total_size += ents[i].size;
        const char *ow = owner_of(dst);
        if (ow && strcmp(ow, ctrl->name)) {
            printf(C_RED "  bulum: %s is owned by %s, would be overwritten\n" C_RESET,
                   dst, ow);
            return 1;
        }
        if (!ow && file_exists(dst) && !force) {
            printf(C_RED "  bulum: %s exists and is not managed by bulum\n" C_RESET, dst);
            printf(C_YELLOW "         use --force to overwrite it\n" C_RESET);
            return 1;
        }
    }
    if (nent == 0) {
        printf(C_RED "  bulum: package contains no files\n" C_RESET);
        return 1;
    }

    tx_begin("install", ctrl->name, ctrl->version);
    printf(C_BOLD "  [ ... ] %s %s (%d files)\n" C_RESET,
           ctrl->name, ctrl->version, nent);

    int failed = 0;
    int pid = getpid();
    for (int i = 0; i < nent && !failed; i++) {
        char dst[512], src[512];
        snprintf(dst, sizeof(dst), "/%s", ents[i].path);
        snprintf(src, sizeof(src), "%s/%s", stage_path, ents[i].path);
        if (ents[i].is_dir) {
            if (!file_exists(dst)) {
                if (mkdir(dst, ents[i].mode & 0777) == 0)
                    tx_write("M\t%s", dst);
                else
                    failed = 1;
            }
            continue;
        }
        char bak[600];
        snprintf(bak, sizeof(bak), "%s/%s-%d-%d", BACKUP_DIR, ctrl->name, pid, i);
        struct stat st;
        if (lstat(src, &st) == 0 && S_ISLNK(st.st_mode)) {
            if (file_exists(dst)) {
                if (copy_file(dst, bak, 0644) == 0 && copy_link(src, dst) == 0)
                    tx_write("W\t%s\t%s\t%s", dst, src, bak);
                else
                    failed = 1;
            } else {
                if (copy_link(src, dst) == 0)
                    tx_write("W\t%s\t%s\t-", dst, src);
                else
                    failed = 1;
            }
            continue;
        }
        if (file_exists(dst)) {
            if (copy_file(dst, bak, 0644) == 0 && copy_file(src, dst, ents[i].mode) == 0)
                tx_write("W\t%s\t%s\t%s", dst, src, bak);
            else
                failed = 1;
        } else {
            if (copy_file(src, dst, ents[i].mode) == 0)
                tx_write("W\t%s\t%s\t-", dst, src);
            else
                failed = 1;
        }
    }
    if (failed) {
        printf(C_RED "  bulum: install failed, rolling back\n" C_RESET);
        recover_tx();
        printf(C_RED "  bulum: %s is NOT installed\n" C_RESET, ctrl->name);
        bulum_log("install %s %s FAILED (rolled back)", ctrl->name, ctrl->version);
        return 1;
    }

    tx_commit_marker();

    /* update the package database */
    pkg_t np = *ctrl;
    snprintf(np.status, sizeof(np.status), "install ok installed");
    np.size = total_size / 1024 + 1;
    db_add_or_update(&db, &np);
    db_write(&db);

    /* write file lists + md5sums */
    char listp[512], md5p[512];
    snprintf(listp, sizeof(listp), "%s/%s.list", DPKG_INFO, ctrl->name);
    snprintf(md5p, sizeof(md5p), "%s/%s.md5sums", DPKG_INFO, ctrl->name);
    FILE *lf = fopen(listp, "w");
    FILE *mf = fopen(md5p, "w");
    if (lf && mf) {
        for (int i = 0; i < nent; i++) {
            char dst[512];
            snprintf(dst, sizeof(dst), "/%s", ents[i].path);
            if (ents[i].is_dir) {
                fprintf(lf, "%s/\n", dst);
            } else {
                fprintf(lf, "%s\n", dst);
                unsigned char d[16];
                char src[512];
                snprintf(src, sizeof(src), "%s/%s", stage_path, ents[i].path);
                md5_file(src, d);
                for (int b = 0; b < 16; b++)
                    fprintf(mf, "%02x", d[b]);
                fprintf(mf, "  %s\n", dst + 1);
            }
        }
    }
    if (lf)
        fclose(lf);
    if (mf)
        fclose(mf);

    tx_clear();
    rmdir_recursive(stage_path);

    /* drop backups of the old version's overwritten files */
    DIR *bd = opendir(BACKUP_DIR);
    if (bd) {
        char prefix[128];
        snprintf(prefix, sizeof(prefix), "%s-%d-", ctrl->name, pid);
        struct dirent *e;
        while ((e = readdir(bd))) {
            if (!strncmp(e->d_name, prefix, strlen(prefix))) {
                char p[600];
                snprintf(p, sizeof(p), "%s/%s", BACKUP_DIR, e->d_name);
                unlink(p);
            }
        }
        closedir(bd);
    }

    printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
           "installed %s %s\n", ctrl->name, ctrl->version);
    bulum_log("installed %s %s", ctrl->name, ctrl->version);
    return 0;
}

static int install_deb(const char *deb, const pkg_t *ctrl, int force)
{
    int rc = preinstall_checks(ctrl, force);
    if (rc == 2)
        return 0;
    if (rc == 1)
        return 1;
    snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGE_DIR, ctrl->name);
    if (extract_deb(deb, stage_path) != 0)
        return 1;
    return install_staged(ctrl, force);
}

/* ================= BPK packages (native format) ================= */

#define BPK_MAX (256u * 1024 * 1024)

static int path_ok(const char *rel)
{
    if (!rel[0] || rel[0] == '/' || strstr(rel, "//"))
        return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    for (char *p = strtok(tmp, "/"); p; p = strtok(NULL, "/"))
        if (!strcmp(p, "..") || !strcmp(p, ".") || !strcmp(p, ""))
            return 0;
    return 1;
}

static int tar_oct(const char *f, int len, uint64_t *out)
{
    if ((unsigned char)f[0] & 0x80) /* GNU base-256: not generated */
        return -1;
    char tmp[24];
    int k = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
    memcpy(tmp, f, k);
    tmp[k] = 0;
    *out = strtoull(tmp, NULL, 8);
    return 0;
}

static int dup_meta_fields(const char *start, const char *end)
{
    static const char *names[] = {
        "Package", "Version", "Architecture", "Maintainer", "Depends",
        "Conflicts", "Provides", "Description", "Priority", "Section",
        "Status", "Installed-Size", "SHA256", "Size",
        "License", "Keywords"
    };
    unsigned seen[sizeof(names) / sizeof(names[0])] = { 0 };
    const char *p = start;
    while (p < end) {
        const char *nl = strchr(p, '\n');
        if (!nl)
            nl = end;
        const char *colon = strchr(p, ':');
        if (colon && colon < nl) {
            int fl = (int)(colon - p);
            for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
                int nlen = (int)strlen(names[k]);
                if (fl == nlen && !strncmp(p, names[k], (size_t)nlen)) {
                    if (seen[k])
                        return 1;
                    seen[k] = 1;
                    break;
                }
            }
        }
        p = nl + 1;
    }
    return 0;
}

/* walk an uncompressed ustar payload (as embedded in .bpk files) and
 * materialize it under <stage>. returns 0 on success. */
static int tar_extract(const unsigned char *data, long len, const char *stage)
{
    long off = 0;
    char longname[1024] = "";
    char longlink[1024] = "";
    while (off + 512 <= len) {
        const unsigned char *h = data + off;
        int allz = 1;
        for (int i = 0; i < 512; i++)
            if (h[i]) {
                allz = 0;
                break;
            }
        if (allz)
            break; /* end-of-archive (zero block) */
        unsigned sum = 0;
        for (int i = 0; i < 512; i++)
            sum += (i >= 148 && i < 156) ? ' ' : h[i];
        char ck[9] = { 0 };
        memcpy(ck, h + 148, 7);
        unsigned expect = (unsigned)strtoul(ck, NULL, 8);
        if (expect != (sum & 0777777))
            return -1;

        uint64_t size = 0;
        if (tar_oct((const char *)h + 124, 12, &size) != 0)
            return -1;
        unsigned char type = h[156];
        uint64_t padsz = (size + 511) & ~511ull;
        if (off + 512 + padsz > (uint64_t)len ||
            off + 512 + size > (uint64_t)len)
            return -1;

        char name[101] = { 0 }, prefix[156] = { 0 }, link[101] = { 0 };
        memcpy(name, h, 100);
        memcpy(prefix, h + 345, 155);
        memcpy(link, h + 157, 100);

        if (type == 'L') { /* GNU long name in payload */
            if (size >= sizeof(longname))
                return -1;
            memcpy(longname, data + off + 512, size);
            longname[size] = 0;
            off += 512 + padsz;
            continue;
        }
        if (type == 'K') { /* GNU long linkname in payload */
            if (size >= sizeof(longlink))
                return -1;
            memcpy(longlink, data + off + 512, size);
            longlink[size] = 0;
            off += 512 + padsz;
            continue;
        }

        char rel[700];
        if (longname[0]) {
            snprintf(rel, sizeof(rel), "%s", longname);
            longname[0] = 0;
        } else if (prefix[0]) {
            snprintf(rel, sizeof(rel), "%s/%s", prefix, name);
        } else {
            snprintf(rel, sizeof(rel), "%s", name);
        }
        if (!path_ok(rel))
            return -1;
        char full[768];
        snprintf(full, sizeof(full), "%s/%s", stage, rel);

        if (type == '5') { /* directory */
            mkdir_p(full);
            char md[9] = { 0 };
            memcpy(md, h + 100, 8);
            chmod(full, strtoull(md, NULL, 8) & 07777);
        } else if (type == '2' || type == '1') { /* symlink / hardlink */
            char target[1024];
            if (longlink[0]) {
                snprintf(target, sizeof(target), "%s", longlink);
                longlink[0] = 0;
            } else {
                snprintf(target, sizeof(target), "%s", link);
            }
            char *sl = strrchr(full, '/');
            if (sl) {
                *sl = 0;
                mkdir_p(full);
                *sl = '/';
            }
            unlink(full);
            symlink(target, full);
        } else if (type == '0' || type == 0) { /* regular file */
            char *sl = strrchr(full, '/');
            if (sl) {
                *sl = 0;
                mkdir_p(full);
                *sl = '/';
            }
            int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0)
                return -1;
            const unsigned char *p = data + off + 512;
            size_t left = (size_t)size;
            while (left) {
                ssize_t w = write(fd, p, left);
                if (w <= 0) {
                    close(fd);
                    return -1;
                }
                p += w;
                left -= (size_t)w;
            }
            close(fd);
            char md[9] = { 0 };
            memcpy(md, h + 100, 8);
            chmod(full, strtoull(md, NULL, 8) & 07777);
        } else {
            return -1; /* unsupported entry type */
        }
        off += 512 + padsz;
    }
    return 0;
}

/* read, verify (payload sha256 + RSA signature) and extract a BPK archive
 * into <stage>. control fields are returned in *ctrl.
 * key name refers to KEYS_DIR/<name>.pub; NULL -> "main". */
static int bpk_extract(const char *path, const char *stage, const char *key,
                       pkg_t *ctrl)
{
    long n = 0;
    char *buf = read_file_alloc(path, &n);
    if (!buf)
        return -1;
    if (n < 64 || (unsigned long)n > BPK_MAX || strncmp(buf, "BPK1\n", 5)) {
        free(buf);
        return -1;
    }
    char *sigb = strstr(buf, "BEGIN PACKAGE SIGNATURE\n");
    char *sige = sigb ? strstr(sigb, "\nEND PACKAGE SIGNATURE\n") : NULL;
    if (!sigb || !sige) {
        printf(C_RED "  bulum: %s is not a valid .bpk package\n" C_RESET, path);
        free(buf);
        return -1;
    }
    long signed_len = (long)(sigb - buf);
    char *payload = sige + strlen("\nEND PACKAGE SIGNATURE\n");
    while (payload < buf + n && (*payload == '\n' || *payload == '\r'))
        payload++;
    long paylen = (long)(buf + n - payload);
    if (paylen <= 0) {
        printf(C_RED "  bulum: %s has no payload\n" C_RESET, path);
        free(buf);
        return -1;
    }

    /* Verify the signature over the untouched signed region first */
    const char *mark = "BEGIN PACKAGE SIGNATURE\n";
    size_t siglen = (size_t)(sige - (sigb + strlen(mark)));
    char *sigcopy = malloc(siglen + 1);
    if (!sigcopy) {
        free(buf);
        return -1;
    }
    memcpy(sigcopy, sigb + strlen(mark), siglen);
    sigcopy[siglen] = 0;
    char keypath[512];
    snprintf(keypath, sizeof(keypath), "%s/%s.pub", KEYS_DIR,
             (key && key[0]) ? key : "main");
    if (rsa_verify_sig(keypath, (unsigned char *)buf, signed_len, sigcopy) != 0) {
        printf(C_RED "  bulum: bad signature on %s\n" C_RESET, path);
        free(sigcopy);
        free(buf);
        return -1;
    }
    free(sigcopy);

    if (dup_meta_fields(buf, sigb)) {
        printf(C_RED "  bulum: duplicate metadata field in %s\n" C_RESET, path);
        free(buf);
        return -1;
    }

    parse_record(ctrl, buf, sigb);

    /* SHA256 + Size of the payload come from the signed region */
    char expect_sha[65] = "";
    long expect_size = -1;
    char *ls = buf;
    char *sav = NULL;
    for (char *tok = strtok_r(ls, "\n", &sav); tok && tok < sigb;
         tok = strtok_r(NULL, "\n", &sav)) {
        if (!strncmp(tok, "SHA256: ", 8))
            snprintf(expect_sha, sizeof(expect_sha), "%s", tok + 8);
        else if (!strncmp(tok, "Size: ", 6))
            expect_size = atol(tok + 6);
    }
    if (!expect_sha[0]) {
        printf(C_RED "  bulum: %s has no SHA256 in header\n" C_RESET, path);
        free(buf);
        return -1;
    }
    unsigned char d[32];
    char hex[65];
    sha256_buf((unsigned char *)payload, (size_t)paylen, d);
    sha256_hex(d, hex);
    if (strcmp(hex, expect_sha)) {
        printf(C_RED "  bulum: sha256 mismatch in %s\n" C_RESET, path);
        free(buf);
        return -1;
    }
    if (expect_size >= 0 && paylen != expect_size) {
        printf(C_RED "  bulum: size mismatch in %s\n" C_RESET, path);
        free(buf);
        return -1;
    }

    rmdir_recursive(stage);
    mkdir_p(stage);
    if (tar_extract((const unsigned char *)payload, paylen, stage) != 0) {
        printf(C_RED "  bulum: corrupt payload in %s\n" C_RESET, path);
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

static int install_bpk(const char *path, const pkg_t *ctrl, int force,
                       const char *key)
{
    int rc = preinstall_checks(ctrl, force);
    if (rc == 2)
        return 0;
    if (rc == 1)
        return 1;
    snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGE_DIR, ctrl->name);
    pkg_t got;
    if (bpk_extract(path, stage_path, key, &got) != 0) {
        rmdir_recursive(stage_path);
        return 1;
    }
    return install_staged(ctrl, force);
}

/* local .bpk file install: control comes from the archive itself */
static int install_bpk_file(const char *path, const char *key, int force)
{
    pkg_t ctrl;
    snprintf(stage_path, sizeof(stage_path), "%s/local", STAGE_DIR);
    if (bpk_extract(path, stage_path, key, &ctrl) != 0) {
        rmdir_recursive(stage_path);
        return 1;
    }
    int rc = preinstall_checks(&ctrl, force);
    if (rc == 2 || rc == 1) {
        rmdir_recursive(stage_path);
        return rc == 2 ? 0 : 1;
    }
    return install_staged(&ctrl, force);
}

static int install_avail(avail_t *a, int force)
{
    char deb[1024];
    if (fetch_pkg(a, deb, sizeof(deb)) != 0)
        return 1;
    snprintf(a->p.origin, sizeof(a->p.origin), "%s", a->repo);
    if (!strcmp(a->format, "bpk")) {
        const repo_t *r = repo_find(a->repo);
        const char *key = (r && r->key[0]) ? r->key : NULL;
        return install_bpk(deb, &a->p, force, key);
    }
    return install_deb(deb, &a->p, force);
}

/* ================= remove ================= */

static int remove_pkg(const char *name, int force)
{
    db_t db = { 0 };
    db_load(&db);
    int i = db_find(&db, name);
    if (i < 0 || !pkg_installed(&db.a[i])) {
        printf(C_RED "  bulum: %s is not installed\n" C_RESET, name);
        return 1;
    }
    pkg_t *p = &db.a[i];
    provs_t pv;
    build_provides(&db, &pv);

    const char *dep = reverse_dep_break(&db, &pv, name);
    if (dep && !force) {
        printf(C_RED "  bulum: cannot remove %s: %s still depends on it\n" C_RESET,
               name, dep);
        return 1;
    }
    if (dep)
        printf(C_YELLOW "  bulum: --force: removing despite %s\n" C_RESET, dep);

    char listp[512];
    snprintf(listp, sizeof(listp), "%s/%s.list", DPKG_INFO, name);
    long n = 0;
    char *buf = read_file_alloc(listp, &n);
    if (!buf) {
        printf(C_RED "  bulum: broken database: no file list for %s\n" C_RESET, name);
        return 1;
    }

    build_own_map(&db);
    tx_begin("remove", name, p->version);
    int pid = getpid();
    int seq = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0]) {
            size_t tl = strlen(line);
            int is_dir = (tl > 1 && line[tl - 1] == '/');
            if (!is_dir) {
                if (other_owners(line, name) == 0 && file_exists(line)) {
                    char bak[600];
                    snprintf(bak, sizeof(bak), "%s/%s-rm-%d-%d", BACKUP_DIR, name,
                             pid, seq);
                    if (copy_file(line, bak, 0644) == 0 && unlink(line) == 0)
                        tx_write("R\t%s\t%s", line, bak);
                }
            }
            seq++;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);

    /* dirs: walk the list again, deepest first (parents were listed after
     * their children during install), remove only what becomes empty */
    char dirs[4096][512];
    int ndir = 0;
    FILE *df = fopen(listp, "r");
    if (df) {
        char l[512];
        while (fgets(l, sizeof(l), df) && ndir < 4096) {
            size_t tl = strlen(l);
            if (tl > 1 && l[tl - 1] == '\n')
                l[--tl] = 0;
            if (tl > 1 && l[tl - 1] == '/')
                l[--tl] = 0;
            if (l[0])
                snprintf(dirs[ndir++], sizeof(dirs[0]), "%s", l);
        }
        fclose(df);
    }
    /* dirs array is in list order; the deepest dirs come first, but we want
     * longest path first regardless of order -> selection sort on length */
    for (int a = 0; a < ndir; a++) {
        int best = a;
        for (int b = a + 1; b < ndir; b++)
            if (strlen(dirs[b]) > strlen(dirs[best]))
                best = b;
        char t[512];
        snprintf(t, sizeof(t), "%s", dirs[a]);
        snprintf(dirs[a], sizeof(dirs[0]), "%s", dirs[best]);
        snprintf(dirs[best], sizeof(dirs[0]), "%s", t);
    }
    for (int a = 0; a < ndir; a++) {
        if (rmdir(dirs[a]) == 0)
            tx_write("D\t%s", dirs[a]);
    }

    tx_commit_marker();
    db_remove_pkg(&db, name);
    db_write(&db);
    char pth[512];
    snprintf(pth, sizeof(pth), "%s/%s.list", DPKG_INFO, name);
    unlink(pth);
    snprintf(pth, sizeof(pth), "%s/%s.md5sums", DPKG_INFO, name);
    unlink(pth);
    tx_clear();

    printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
           "removed %s %s\n", name, p->version);
    bulum_log("removed %s %s", name, p->version);
    return 0;
}

/* ================= commands ================= */

static int cmd_update(void)
{
    repo_load();
    if (nrepos == 0) {
        printf(C_YELLOW "  bulum: no repositories configured (%s)\n" C_RESET, REPOS_CONF);
        return 1;
    }
    int ok = 0, fail = 0, skipped = 0;
    for (int ri = 0; ri < nrepos; ri++) {
        const repo_t *r = &repos[ri];
        if (!r->enabled) {
            printf(C_GRAY "  [ -- ] %-12s skipped (disabled)\n" C_RESET, r->name);
            skipped++;
            continue;
        }
        char rel_tmp[512], sig_tmp[512], pkg_tmp[512];
        snprintf(rel_tmp, sizeof(rel_tmp), "%s/.tmp-%s-Release", LISTS_DIR, r->name);
        snprintf(sig_tmp, sizeof(sig_tmp), "%s/.tmp-%s-Release.sig", LISTS_DIR, r->name);
        snprintf(pkg_tmp, sizeof(pkg_tmp), "%s/.tmp-%s-Packages", LISTS_DIR, r->name);
        unlink(rel_tmp);
        unlink(sig_tmp);
        unlink(pkg_tmp);

        char url[1024];
        snprintf(url, sizeof(url), "%s/Release", r->url);
        if (wget(url, rel_tmp, r->insecure ? "--no-check-certificate" : NULL) != 0) {
            printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                   "%-12s cannot fetch Release\n", r->name);
            fail++;
            continue;
        }
        long relen = 0;
        char *rel = read_file_alloc(rel_tmp, &relen);

        int verified = 0;
        if (r->key[0]) {
            snprintf(url, sizeof(url), "%s/Release.sig", r->url);
            int has_sig = wget(url, sig_tmp, r->insecure ? "--no-check-certificate" : NULL) == 0;
            if (!has_sig) {
                printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                       "%-12s signed repo but no Release.sig\n", r->name);
                fail++;
                free(rel);
                continue;
            }
            long siglen = 0;
            char *sig = read_file_alloc(sig_tmp, &siglen);
            if (rsa_verify_sig(r->key, (unsigned char *)rel, relen, sig) == 0)
                verified = 1;
            free(sig);
            if (!verified) {
                printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                       "%-12s bad signature on Release\n", r->name);
                fail++;
                free(rel);
                continue;
            }
        }

        /* parse Release -> SHA256 of Packages */
        char expect_sha[65] = "";
        long expect_size = -1;
        if (rel) {
            int expect = 0;
            char *ls = rel;
            char *sav = NULL;
            for (char *tok = strtok_r(ls, "\n", &sav); tok; tok = strtok_r(NULL, "\n", &sav)) {
                if (expect) {
                    char h[65], nm[128];
                    long sz;
                    if (sscanf(tok, "%64s %ld %127s", h, &sz, nm) == 3 &&
                        !strcmp(nm, "Packages")) {
                        snprintf(expect_sha, sizeof(expect_sha), "%s", h);
                        expect_size = sz;
                        break;
                    }
                    expect = 0;
                }
                if (!strcmp(tok, "SHA256:"))
                    expect = 1;
            }
        }
        free(rel);
        if (!expect_sha[0]) {
            printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                   "%-12s no SHA256 for Packages in Release\n", r->name);
            fail++;
            continue;
        }

        snprintf(url, sizeof(url), "%s/Packages", r->url);
        if (wget(url, pkg_tmp, r->insecure ? "--no-check-certificate" : NULL) != 0) {
            printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                   "%-12s cannot fetch Packages\n", r->name);
            fail++;
            continue;
        }

        /* verify checksum against the signed Release */
        unsigned char d[32];
        char hex[65];
        sha256_file(pkg_tmp, d);
        sha256_hex(d, hex);
        if (strcmp(hex, expect_sha)) {
            printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                   "%-12s Packages checksum mismatch\n", r->name);
            fail++;
            continue;
        }
        if (expect_size > 0) {
            struct stat st;
            if (stat(pkg_tmp, &st) == 0 && st.st_size != expect_size) {
                printf("  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET
                       "%-12s Packages size mismatch\n", r->name);
                fail++;
                continue;
            }
        }

        /* count packages */
        int npk = 0;
        char *pk = read_file_alloc(pkg_tmp, NULL);
        if (pk) {
            if (!strncmp(pk, "Package:", 8))
                npk++;
            char *pp = pk;
            while ((pp = strstr(pp, "\nPackage:"))) {
                npk++;
                pp += 9;
            }
            free(pk);
        }
        char fin[512];
        snprintf(fin, sizeof(fin), "%s/%s.Packages", LISTS_DIR, r->name);
        rename(pkg_tmp, fin);
        unlink(rel_tmp);
        unlink(sig_tmp);
        printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
               "%-12s %d packages%s\n", r->name, npk,
               verified ? " (signature verified)" : " (unsigned)");
        ok++;
    }
    printf(C_BOLD "  [ %s ] " C_RESET "update complete: %d/%d repos ready\n",
           fail ? C_RED "!!" : C_GREEN "OK", ok, ok + fail);
    bulum_log("update: %d/%d repos ready (%d skipped)", ok, ok + fail, skipped);
    return fail ? 1 : 0;
}

static int cmd_search(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bulum search <term>\n");
        return 2;
    }
    avail_load_all();
    if (navail == 0) {
        if (json_mode)
            printf("{\"results\":[]}");
        else
            printf(C_YELLOW "  bulum: no package indexes; run 'bulum update' first\n" C_RESET);
        return 1;
    }
    int found = 0;
    if (json_mode) {
        printf("{\"query\":");
        json_str(stdout, argv[0]);
        printf(",\"results\":[");
    }
    for (int i = 0; i < navail; i++) {
        int hit = strstr(avail[i].p.name, argv[0]) != NULL ||
                  strstr(avail[i].p.desc, argv[0]) != NULL;
        if (!hit)
            continue;
        if (json_mode) {
            if (found)
                printf(",");
            printf("{\"name\":");
            json_str(stdout, avail[i].p.name);
            printf(",\"version\":");
            json_str(stdout, avail[i].p.version);
            printf(",\"description\":");
            json_str(stdout, avail[i].p.desc);
            printf(",\"repo\":");
            json_str(stdout, avail[i].repo);
            printf(",\"arch\":");
            json_str(stdout, avail[i].p.arch);
            printf("}");
        } else {
            printf("  %-16s %-12s %s [%s]\n", avail[i].p.name, avail[i].p.version,
                   avail[i].p.desc[0] ? avail[i].p.desc : "(no description)",
                   avail[i].repo);
        }
        found++;
    }
    if (json_mode) {
        printf("],\"count\":%d}", found);
        return found ? 0 : 1;
    }
    printf(C_GRAY "  %d match%s\n" C_RESET, found, found == 1 ? "" : "es");
    return found ? 0 : 1;
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bulum info <package>\n");
        return 2;
    }
    db_t db = { 0 };
    db_load(&db);
    avail_load_all();
    const char *nm = argv[0];
    int inst = db_find(&db, nm);
    avail_t *best = avail_find_best(nm, 0);
    if (inst < 0 && !best) {
        if (json_mode)
            printf("{\"error\":\"not found\"}");
        else
            printf(C_RED "  bulum: %s not found (not installed, not in indexes)\n" C_RESET, nm);
        return 1;
    }
    if (json_mode) {
        printf("{");
        if (inst >= 0) {
            pkg_t *p = &db.a[inst];
            printf("\"name\":");
            json_str(stdout, p->name);
            printf(",\"installed\":true,\"version\":");
            json_str(stdout, p->version);
            printf(",\"arch\":");
            json_str(stdout, p->arch);
            json_field(stdout, "depends", p->depends);
            json_field(stdout, "conflicts", p->conflicts);
            json_field(stdout, "provides", p->provides);
            json_field(stdout, "description", p->desc);
            json_field(stdout, "license", p->license);
            json_field(stdout, "maintainer", p->maintainer);
            json_field(stdout, "origin", p->origin);
            printf("\n    ,\"size\":%ld", p->size);
        } else {
            printf("\"installed\":false");
        }
        if (best && (inst < 0 || vercmp(best->p.version, db.a[inst].version) > 0)) {
            printf(",\"available\":true,\"repo\":");
            json_str(stdout, best->repo);
            printf(",\"candidate_version\":");
            json_str(stdout, best->p.version);
            json_field(stdout, "candidate_depends", best->p.depends);
        }
        printf("\n}");
        return 0;
    }
    if (inst >= 0) {
        pkg_t *p = &db.a[inst];
        printf(C_BOLD "  %s %s\n" C_RESET, p->name, p->version);
        printf("    Status:     %s\n", pkg_installed(p) ? "installed" : p->status);
        printf("    Architecture: %s\n", p->arch[0] ? p->arch : "unknown");
        if (p->size)
            printf("    Installed-Size: %ld KB\n", p->size);
        if (p->depends[0])
            printf("    Depends:    %s\n", p->depends);
        if (p->conflicts[0])
            printf("    Conflicts:  %s\n", p->conflicts);
        if (p->provides[0])
            printf("    Provides:   %s\n", p->provides);
        if (p->desc[0])
            printf("    Description: %s\n", p->desc);
        if (p->license[0])
            printf("    License:    %s\n", p->license);
        if (p->maintainer[0])
            printf("    Maintainer: %s\n", p->maintainer);
        if (p->origin[0])
            printf("    Origin:     %s\n", p->origin);
    }
    if (best && (inst < 0 || vercmp(best->p.version, db.a[inst].version) > 0)) {
        printf(C_CYAN "  %s %s (available)\n" C_RESET, best->p.name, best->p.version);
        printf("    Repository: %s\n", best->repo);
        printf("    Architecture: %s\n", best->p.arch);
        if (best->p.depends[0])
            printf("    Depends:    %s\n", best->p.depends);
        if (best->p.license[0])
            printf("    License:    %s\n", best->p.license);
        if (best->p.desc[0])
            printf("    Description: %s\n", best->p.desc);
    }
    return 0;
}

static int cmd_list(void)
{
    db_t db = { 0 };
    db_load(&db);
    int n = 0;
    if (!json_mode)
        printf(C_BOLD "  %-16s %-14s %-10s %s\n" C_RESET, "PACKAGE", "VERSION", "SIZE", "ARCH");
    if (json_mode)
        printf("{\"installed\":[");
    for (int i = 0; i < db.n; i++) {
        if (!pkg_installed(&db.a[i]))
            continue;
        if (json_mode) {
            if (n)
                printf(",");
            printf("{\"name\":");
            json_str(stdout, db.a[i].name);
            printf(",\"version\":");
            json_str(stdout, db.a[i].version);
            printf(",\"arch\":");
            json_str(stdout, db.a[i].arch);
            printf(",\"size\":%ld}", db.a[i].size);
        } else {
            printf("  %-16s %-14s %-9ldKB %s\n", db.a[i].name, db.a[i].version,
                   db.a[i].size, db.a[i].arch[0] ? db.a[i].arch : "unknown");
        }
        n++;
    }
    if (json_mode)
        printf("],\"count\":%d}", n);
    else
        printf(C_GRAY "  %d package%s installed\n" C_RESET, n, n == 1 ? "" : "s");
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bulum install <package|file.deb|file.bpk> [--force]\n");
        return 2;
    }
    int force = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f"))
            force = 1;
    const char *arg = argv[0];
    int is_file = strchr(arg, '/') != NULL || strstr(arg, ".deb") != NULL ||
                  strstr(arg, ".bpk") != NULL;
    if (is_file && file_exists(arg)) {
        char magic[5] = { 0 };
        FILE *f = fopen(arg, "rb");
        if (f) {
            fread(magic, 1, 4, f);
            fclose(f);
        }
        if (!strncmp(magic, "BPK1", 4))
            return install_bpk_file(arg, NULL, force);
        pkg_t ctrl;
        if (control_from_deb(arg, &ctrl) != 0) {
            printf(C_RED "  bulum: %s is not a valid package file\n" C_RESET, arg);
            return 1;
        }
        return install_deb(arg, &ctrl, force);
    }
    repo_load();
    avail_load_all();
    avail_t *cand = avail_find_best(arg, 1);
    if (!cand) {
        printf(C_RED "  bulum: %s not found in repositories\n" C_RESET, arg);
        printf(C_YELLOW "         run 'bulum update' to refresh indexes\n" C_RESET);
        return 1;
    }
    /* local .bpk/.deb installs also resolve deps from indexes when possible */
    {
        int np = plan_resolve(cand);
        if (np < 0)
            return 1;
        if (np == 0) {
            printf(C_YELLOW "  bulum: %s is already installed\n" C_RESET, arg);
            return 0;
        }
        printf(C_CYAN "  The following packages will be installed:\n" C_RESET);
        for (int i = 0; i < np; i++)
            printf("    %-16s %s%s\n", plan_arr[i]->p.name,
                   plan_arr[i]->p.version,
                   strcmp(plan_arr[i]->p.name, arg) ? "" : "  <requested>");
        int rcf = 0;
        for (int i = 0; i < np; i++)
            if (install_avail(plan_arr[i], force) != 0) {
                printf(C_RED "  bulum: install of '%s' failed; "
                       "remaining packages aborted\n" C_RESET,
                       plan_arr[i]->p.name);
                rcf = 1;
                break;
            }
        return rcf;
    }
}

static int cmd_remove(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bulum remove <package> [--force]\n");
        return 2;
    }
    int force = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f"))
            force = 1;
    return remove_pkg(argv[0], force);
}

static int cmd_upgrade(void)
{
    db_t db = { 0 };
    db_load(&db);
    repo_load();
    avail_load_all();
    int pending = 0;
    for (int i = 0; i < db.n; i++) {
        if (!pkg_installed(&db.a[i]))
            continue;
        avail_t *cand = avail_find_best(db.a[i].name, 1);
        if (!cand || vercmp(cand->p.version, db.a[i].version) <= 0)
            continue;
        if (!pending)
            printf(C_CYAN "  Upgrading\n" C_RESET);
        printf("    %s %s -> %s\n", db.a[i].name, db.a[i].version, cand->p.version);
        pending++;
        int np = plan_resolve(cand);
        if (np < 0) {
            printf(C_RED "  bulum: upgrade of %s aborted (deps unsatisfiable)\n" C_RESET,
                   db.a[i].name);
            return 1;
        }
        for (int j = 0; j < np; j++) {
            const repo_t *r = repo_find(plan_arr[j]->repo);
            const char *key = (r && r->key[0]) ? r->key : NULL;
            char arch[1024];
            if (fetch_pkg(plan_arr[j], arch, sizeof(arch)) != 0)
                return 1;
            int rc;
            if (!strcmp(plan_arr[j]->format, "bpk")) {
                snprintf(plan_arr[j]->p.origin, sizeof(plan_arr[j]->p.origin), "%s",
                         plan_arr[j]->repo);
                rc = install_bpk(arch, &plan_arr[j]->p, 0, key);
            } else {
                snprintf(plan_arr[j]->p.origin, sizeof(plan_arr[j]->p.origin), "%s",
                         plan_arr[j]->repo);
                rc = install_deb(arch, &plan_arr[j]->p, 0);
            }
            if (rc != 0)
                return rc;
        }
    }
    if (!pending)
        printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
               "all packages are up to date\n");
    return 0;
}

static int cmd_clean(void)
{
    long freed = 0;
    DIR *d = opendir(CACHE_DIR);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", CACHE_DIR, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0) {
                freed += st.st_size;
                unlink(p);
                n++;
            }
        }
        closedir(d);
    }
    printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
           "removed %d cached archive%s (%ld KB)\n", n, n == 1 ? "" : "s",
           freed / 1024);
    bulum_log("clean: removed %d archives (%ld KB)", n, freed / 1024);
    return 0;
}

static int cmd_doctor(void)
{
    int problems = 0;
    recover_tx();

    db_t db = { 0 };
    db_load(&db);
    int ninst = 0;
    for (int i = 0; i < db.n; i++) {
        pkg_t *p = &db.a[i];
        if (!pkg_installed(p))
            continue;
        ninst++;
        char listp[512];
        snprintf(listp, sizeof(listp), "%s/%s.list", DPKG_INFO, p->name);
        FILE *lf = fopen(listp, "r");
        if (!lf) {
            printf(C_RED "  MISSING %s.list\n" C_RESET, p->name);
            problems++;
            continue;
        }
        char path[512];
        while (fgets(path, sizeof(path), lf)) {
            size_t tl = strlen(path);
            if (tl && path[tl - 1] == '\n')
                path[--tl] = 0;
            if (!tl)
                continue;
            int is_dir = path[tl - 1] == '/';
            if (is_dir)
                path[--tl] = 0;
            if (!file_exists(path)) {
                if (!is_dir) {
                    printf(C_RED "  MISSING %s (owned by %s)\n" C_RESET, path, p->name);
                    problems++;
                }
                continue;
            }
        }
        fclose(lf);

        /* verify md5sums */
        char md5p[512];
        snprintf(md5p, sizeof(md5p), "%s/%s.md5sums", DPKG_INFO, p->name);
        FILE *mf = fopen(md5p, "r");
        if (!mf) {
            printf(C_RED "  MISSING %s.md5sums\n" C_RESET, p->name);
            problems++;
        } else {
            char sum[40], rel[512];
            while (fscanf(mf, "%39s %511s", sum, rel) == 2) {
                char full[600];
                snprintf(full, sizeof(full), "/%s", rel);
                if (!file_exists(full))
                    continue;
                unsigned char d[16];
                md5_file(full, d);
                char hex[33];
                for (int b = 0; b < 16; b++)
                    snprintf(hex + b * 2, 3, "%02x", d[b]);
                hex[32] = 0;
                if (strcmp(hex, sum)) {
                    printf(C_RED "  MODIFIED %s (owned by %s)\n" C_RESET, full, p->name);
                    problems++;
                }
            }
            fclose(mf);
        }
    }

    /* dependency integrity */
    provs_t pv;
    build_provides(&db, &pv);
    for (int i = 0; i < db.n; i++) {
        if (!pkg_installed(&db.a[i]))
            continue;
        char reason[256] = "";
        if (deps_ok(&db, &pv, db.a[i].depends, reason, sizeof(reason)) != 0) {
            printf(C_RED "  BROKEN-DEP %s needs %s\n" C_RESET, db.a[i].name, reason);
            problems++;
        }
    }

    /* duplicate (non-dir) file ownership */
    build_own_map(&db);
    for (int i = 0; i < nowner; i++) {
        if (own_map[i].is_dir)
            continue;
        int c = 0;
        for (int j = 0; j < nowner; j++)
            if (!strcmp(own_map[j].path, own_map[i].path))
                c++;
        if (c > 1) {
            printf(C_RED "  SHARED-FILE %s owned by multiple packages\n" C_RESET,
                   own_map[i].path);
            problems++;
            /* avoid noise: skip remaining duplicates of this path */
            for (int k = i + 1; k < nowner; k++)
                if (!strcmp(own_map[k].path, own_map[i].path))
                    own_map[k].path[0] = 0;
        }
    }

    printf(C_BOLD "  [ %s ] " C_RESET "doctor: %d package%s, %d problem%s\n",
           problems ? C_RED "!!" : C_GREEN "OK", ninst, ninst == 1 ? "" : "s",
           problems, problems == 1 ? "" : "s");
    return problems ? 1 : 0;
}

static void usage(const char *p)
{
    printf("usage: %s <command> [args]   (bpk is a symlink to this binary)\n"
           "  update                fetch repository indexes\n"
           "  install <pkg|deb>     install a package (resolves dependencies)\n"
           "  remove <pkg>          remove a package\n"
           "  search <term>         search available packages\n"
           "  info <pkg>            show package details\n"
           "  list                  list installed packages\n"
           "  upgrade               upgrade installed packages\n"
           "  clean                 remove cached archives\n"
           "  doctor                verify database, recover interrupted ops\n"
           "  owns <file>           show which package owns a file\n"
           "  verify [pkg]          verify package file integrity\n"
           "  repo list             list configured repositories\n"
           "  repo add <name> <url> [arch [key [flags]]]\n"
           "  repo remove <name>    remove a repository\n"
           "  repo enable <name>    enable a repository\n"
           "  repo disable <name>   disable a repository\n"
           "  arch                  print system architecture\n"
           "  version               print version\n"
           "  help                  this text\n"
           "  --json <cmd> [args]   machine-readable JSON output\n", p);
}

/* ================= owns / verify / arch / repo commands ================= */

/* does a package have a file list entry? (1 = yes) */
static int package_has_list(const char *name)
{
    char listp[512];
    snprintf(listp, sizeof(listp), "%s/%s.list", DPKG_INFO, name);
    return file_exists(listp);
}

static int verify_one_pkg(const char *name)
{
    db_t db = { 0 };
    db_load(&db);
    int idx = db_find(&db, name);
    if (idx < 0 || !pkg_installed(&db.a[idx])) {
        if (json_mode) {
            printf("{\"name\":");
            json_str(stdout, name);
            printf(",\"verified\":0,\"modified\":0,\"missing\":0,\"status\":\"not installed\"}");
        } else {
            printf(C_RED "  bulum: %s is not installed\n" C_RESET, name);
        }
        return 1;
    }
    char listp[512], md5p[512];
    snprintf(listp, sizeof(listp), "%s/%s.list", DPKG_INFO, name);
    snprintf(md5p, sizeof(md5p), "%s/%s.md5sums", DPKG_INFO, name);

    int verified = 0, modified = 0, missing = 0;

    /* load expected md5 map: rel -> hex */
    FILE *mf = fopen(md5p, "r");
    char **rels = NULL;
    char (*sums)[33] = NULL;
    int nsums = 0, sumcap = 0;
    if (mf) {
        char sum[40], rel[512];
        while (fscanf(mf, "%39s %511s", sum, rel) == 2) {
            if (nsums >= sumcap) {
                int nc = sumcap ? sumcap * 2 : 256;
                rels = realloc(rels, nc * sizeof(char *));
                sums = realloc(sums, nc * sizeof(*sums));
                sumcap = nc;
            }
            rels[nsums] = strdup(rel);
            snprintf(sums[nsums], 33, "%s", sum);
            nsums++;
        }
        fclose(mf);
    }
    char *sums_local = (char *)sums;

    FILE *lf = fopen(listp, "r");
    if (!lf) {
        if (!json_mode) {
            printf(C_RED "  bulum: broken database: no file list for %s\n" C_RESET, name);
            printf("    verified: 0 files\n");
            printf("    modified: 0\n");
            printf("    missing: 0\n");
        } else {
            printf("{\"name\":");
            json_str(stdout, name);
            printf(",\"verified\":0,\"modified\":0,\"missing\":0,\"status\":\"broken db\"}");
        }
        for (int k = 0; k < nsums; k++)
            free(rels[k]);
        free(rels);
        free(sums_local);
        return 1;
    }
    char path[512];
    while (fgets(path, sizeof(path), lf)) {
        size_t tl = strlen(path);
        if (tl && path[tl - 1] == '\n')
            path[--tl] = 0;
        if (!tl)
            continue;
        int is_dir = path[tl - 1] == '/';
        if (is_dir)
            path[--tl] = 0;
        if (!file_exists(path)) {
            if (!is_dir)
                missing++;
            continue;
        }
        if (is_dir) {
            verified++;
            continue;
        }
        /* find expected md5 (rel path without leading slash) */
        const char *rel = path + 1;
        int found_sum = 0;
        for (int k = 0; k < nsums; k++) {
            if (rels[k] && !strcmp(rels[k], rel)) {
                unsigned char d[16];
                md5_file(path, d);
                char hex[33];
                for (int b = 0; b < 16; b++)
                    snprintf(hex + b * 2, 3, "%02x", d[b]);
                hex[32] = 0;
                if (strcmp(hex, sums[k])) {
                    modified++;
                    if (!json_mode)
                        printf(C_RED "  MODIFIED %s\n" C_RESET, path);
                } else {
                    verified++;
                }
                found_sum = 1;
                break;
            }
        }
        if (!found_sum)
            verified++; /* no checksum recorded: existence is enough */
    }
    fclose(lf);

    if (!json_mode) {
        printf("  %s\n", name);
        printf("    verified: %d files\n", verified);
        printf("    modified: %d\n", modified);
        printf("    missing: %d\n", missing);
    } else {
        printf("{\"name\":");
        json_str(stdout, name);
        printf(",\"verified\":%d,\"modified\":%d,\"missing\":%d", verified,
               modified, missing);
        printf(",\"status\":\"%s\"}",
               (modified || missing) ? "problems" : "ok");
    }

    for (int k = 0; k < nsums; k++)
        free(rels[k]);
    free(rels);
    free(sums_local);
    return (modified || missing) ? 1 : 0;
}

static int cmd_verify(int argc, char **argv)
{
    if (argc < 1) {
        /* verify all installed packages */
        db_t db = { 0 };
        db_load(&db);
        int tot_v = 0, tot_m = 0, tot_x = 0, npk = 0;
        if (json_mode)
            printf("{\"packages\":[");
        for (int i = 0; i < db.n; i++) {
            if (!pkg_installed(&db.a[i]))
                continue;
            if (json_mode && npk)
                printf(",");
            npk++;
            /* do it inline to accumulate totals */
            char listp[512], md5p[512];
            snprintf(listp, sizeof(listp), "%s/%s.list", DPKG_INFO, db.a[i].name);
            snprintf(md5p, sizeof(md5p), "%s/%s.md5sums", DPKG_INFO, db.a[i].name);
            FILE *lf = fopen(listp, "r");
            if (!lf) {
                tot_x++;
                continue;
            }
            int v = 0, m = 0, x = 0;
            FILE *mf = fopen(md5p, "r");
            char **rels = NULL;
            char (*sums)[33] = NULL;
            int nsums = 0, sumcap = 0;
            if (mf) {
                char sum[40], rel[512];
                while (fscanf(mf, "%39s %511s", sum, rel) == 2) {
                    if (nsums >= sumcap) {
                        int nc = sumcap ? sumcap * 2 : 128;
                        rels = realloc(rels, nc * sizeof(char *));
                        sums = realloc(sums, nc * sizeof(*sums));
                        sumcap = nc;
                    }
                    rels[nsums] = strdup(rel);
                    snprintf(sums[nsums], 33, "%s", sum);
                    nsums++;
                }
                fclose(mf);
            }
            char path[512];
            while (fgets(path, sizeof(path), lf)) {
                size_t tl = strlen(path);
                if (tl && path[tl - 1] == '\n')
                    path[--tl] = 0;
                if (!tl)
                    continue;
                int is_dir = path[tl - 1] == '/';
                if (is_dir)
                    path[--tl] = 0;
                if (is_dir) {
                    v++;
                    continue;
                }
                if (!file_exists(path)) {
                    x++;
                    if (!json_mode)
                        printf(C_RED "  MISSING %s\n" C_RESET, path);
                    continue;
                }
                const char *rel = path + 1;
                int got = 0;
                for (int k = 0; k < nsums; k++) {
                    if (rels[k] && !strcmp(rels[k], rel)) {
                        unsigned char d[16];
                        md5_file(path, d);
                        char hex[33];
                        for (int b = 0; b < 16; b++)
                            snprintf(hex + b * 2, 3, "%02x", d[b]);
                        hex[32] = 0;
                        if (strcmp(hex, sums[k])) {
                            m++;
                            if (!json_mode)
                                printf(C_RED "  MODIFIED %s\n" C_RESET, path);
                        } else {
                            v++;
                        }
                        got = 1;
                        break;
                    }
                }
                if (!got)
                    v++;
            }
            fclose(lf);
            for (int k = 0; k < nsums; k++)
                free(rels[k]);
            free(rels);
            free(sums);
            if (json_mode) {
                printf("{\"name\":");
                json_str(stdout, db.a[i].name);
                printf(",\"verified\":%d,\"modified\":%d,\"missing\":%d}", v, m, x);
            } else {
                printf("  %-16s verified: %d files, modified: %d, missing: %d\n",
                       db.a[i].name, v, m, x);
            }
            tot_v += v;
            tot_m += m;
            tot_x += x;
        }
        if (json_mode) {
            printf("],\"count\":%d,\"verified\":%d,\"modified\":%d,\"missing\":%d}",
                   npk, tot_v, tot_m, tot_x);
        } else {
            printf(C_BOLD "  [ %s ] " C_RESET
                   "verify: %d package%s, %d files, %d modified, %d missing\n",
                   (tot_m || tot_x) ? C_RED "!!" : C_GREEN "OK",
                   npk, npk == 1 ? "" : "s", tot_v, tot_m, tot_x);
        }
        return (tot_m || tot_x) ? 1 : 0;
    }
    return verify_one_pkg(argv[0]);
}

static int cmd_owns(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bulum owns <file>\n");
        return 2;
    }
    const char *path = argv[0];
    db_t db = { 0 };
    db_load(&db);
    build_own_map(&db);
    const char *owner = owner_of(path);
    if (!owner) {
        /* try with/without trailing slash */
        size_t l = strlen(path);
        char t[512];
        if (l > 1 && path[l - 1] == '/')
            snprintf(t, sizeof(t), "%s", path), t[l - 1] = 0;
        else
            snprintf(t, sizeof(t), "%s/", path);
        owner = owner_of(t);
    }
    if (json_mode) {
        if (!owner) {
            printf("{\"path\":");
            json_str(stdout, path);
            printf(",\"owner\":null}");
        } else {
            printf("{\"path\":");
            json_str(stdout, path);
            printf(",\"owner\":");
            json_str(stdout, owner);
            int i = db_find(&db, owner);
            printf(",\"version\":");
            json_str(stdout, i >= 0 ? db.a[i].version : "");
            printf("}");
        }
    } else {
        printf("  %s\n", path);
        if (!owner) {
            printf("    not owned by any package\n");
        } else {
            int i = db_find(&db, owner);
            printf("    owned by: %s %s\n", owner,
                   i >= 0 ? db.a[i].version : "?");
        }
    }
    return owner ? 0 : 1;
}

static int cmd_arch(void)
{
    if (json_mode) {
        printf("{\"arch\":");
        json_str(stdout, sys_arch());
        printf("}");
    } else {
        printf("%s\n", sys_arch());
    }
    return 0;
}

/* rewrite /etc/bulum/repos.conf preserving comments, applying an action
 * to the single matching repo line. For disable: ensure a "disabled" flag;
 * for enable: strip the "disabled" flag. */
static int repos_conf_rewrite(const char *target, int want_enable)
{
    FILE *f = fopen(REPOS_CONF, "r");
    if (!f)
        return -1;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", REPOS_CONF);
    FILE *o = fopen(tmp, "w");
    if (!o) {
        fclose(f);
        return -1;
    }
    int found = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", line);
        char *nl = strchr(copy, '\n');
        if (nl)
            *nl = 0;
        char *hash = strchr(copy, '#');
        if (hash)
            *hash = 0;
        char name[64];
        int have_name = sscanf(copy, "%63s", name) == 1;
        if (!(have_name && !strcmp(name, target))) {
            fputs(line, o);
            continue;
        }
        found = 1;
        /* rebuild the line from its whitespace-separated tokens, dropping
         * the "disabled" token (enable) or appending it (disable).
         * Layout: name url arch key [flags...] */
        size_t ln = strlen(line);
        while (ln && (line[ln - 1] == '\n' || line[ln - 1] == '\r'))
            line[--ln] = 0;
        char tok_buf[1024];
        snprintf(tok_buf, sizeof(tok_buf), "%s", line);
        char *toks[32];
        int nt = 0;
        char *sav = NULL;
        for (char *tok = strtok_r(tok_buf, " \t", &sav); tok && nt < 32;
             tok = strtok_r(NULL, " \t", &sav))
            toks[nt++] = tok;
        if (nt < 4) {
            fputs(line, o);
            fputc('\n', o);
            continue;
        }
        char out[1200];
        out[0] = 0;
        for (int i = 0; i < nt; i++) {
            if (!strcmp(toks[i], "disabled"))
                continue; /* strip */
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s",
                     out[0] ? " " : "", toks[i]);
        }
        if (!want_enable)
            snprintf(out + strlen(out), sizeof(out) - strlen(out),
                     "%sdisabled", out[0] ? " " : "");
        fprintf(o, "%s\n", out);
    }
    fclose(f);
    fclose(o);
    if (!found) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, REPOS_CONF) != 0)
        return -1;
    return 0;
}

static int cmd_repo(int argc, char **argv)
{
    if (argc < 1) {
        printf("usage: bulum repo <list|add|remove|enable|disable> ...\n");
        return 2;
    }
    const char *sub = argv[0];
    if (!strcmp(sub, "list")) {
        repo_load();
        if (json_mode) {
            printf("{\"repos\":[");
            for (int i = 0; i < nrepos; i++) {
                if (i)
                    printf(",");
                printf("{\"name\":");
                json_str(stdout, repos[i].name);
                printf(",\"url\":");
                json_str(stdout, repos[i].url);
                printf(",\"arch\":");
                json_str(stdout, repos[i].arch);
                printf(",\"key\":");
                json_str(stdout, repos[i].key);
                printf(",\"enabled\":%s}", repos[i].enabled ? "true" : "false");
            }
            printf("],\"count\":%d}", nrepos);
            return 0;
        }
        if (nrepos == 0) {
            printf(C_GRAY "  no repositories configured\n" C_RESET);
            return 0;
        }
        printf(C_BOLD "  %-14s %-38s %-8s %s\n" C_RESET, "NAME", "URL", "ARCH", "");
        for (int i = 0; i < nrepos; i++)
            printf("  %-14s %-38s %-8s %s\n", repos[i].name, repos[i].url,
                   repos[i].arch, repos[i].enabled ? "enabled" : "DISABLED");
        return 0;
    }
    if (!strcmp(sub, "add")) {
        if (argc < 3) {
            printf("usage: bulum repo add <name> <url> [arch [key [flags]]]\n");
            return 2;
        }
        const char *name = argv[1];
        const char *url = argv[2];
        if (strchr(name, ' ') || strchr(name, '\t') || name[0] == '#') {
            printf(C_RED "  bulum: invalid repository name\n" C_RESET);
            return 1;
        }
        if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
            printf(C_RED "  bulum: url must start with http:// or https://\n" C_RESET);
            return 1;
        }
        repo_load();
        if (repo_find(name)) {
            printf(C_RED "  bulum: repository '%s' already exists\n" C_RESET, name);
            return 1;
        }
        FILE *f = fopen(REPOS_CONF, "a");
        if (!f) {
            printf(C_RED "  bulum: cannot write %s\n" C_RESET, REPOS_CONF);
            return 1;
        }
        const char *arch = (argc >= 4) ? argv[3] : "all";
        const char *key = (argc >= 5) ? argv[4] : "-";
        const char *flags = (argc >= 6) ? argv[5] : "";
        fprintf(f, "%s %s %s %s%s%s\n", name, url, arch, key,
                flags[0] ? " " : "", flags);
        fclose(f);
        if (json_mode) {
            printf("{\"added\":");
            json_str(stdout, name);
            printf("}");
        } else {
            printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
                   "added repository %s\n", name);
        }
        return 0;
    }
    if (!strcmp(sub, "remove")) {
        if (argc < 2) {
            printf("usage: bulum repo remove <name>\n");
            return 2;
        }
        FILE *f = fopen(REPOS_CONF, "r");
        if (!f) {
            printf(C_RED "  bulum: no repository configuration\n" C_RESET);
            return 1;
        }
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s.tmp", REPOS_CONF);
        FILE *o = fopen(tmp, "w");
        int found = 0;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char copy[1024];
            snprintf(copy, sizeof(copy), "%s", line);
            char *nl = strchr(copy, '\n');
            if (nl)
                *nl = 0;
            char *hash = strchr(copy, '#');
            if (hash)
                *hash = 0;
            char name[64];
            if (sscanf(copy, "%63s", name) == 1 && !strcmp(name, argv[1])) {
                found = 1;
                continue; /* drop this line */
            }
            fputs(line, o);
        }
        fclose(f);
        fclose(o);
        if (!found) {
            unlink(tmp);
            printf(C_RED "  bulum: repository '%s' not found\n" C_RESET, argv[1]);
            return 1;
        }
        rename(tmp, REPOS_CONF);
        if (json_mode) {
            printf("{\"removed\":");
            json_str(stdout, argv[1]);
            printf("}");
        } else {
            printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
                   "removed repository %s\n", argv[1]);
        }
        return 0;
    }
    if (!strcmp(sub, "enable") || !strcmp(sub, "disable")) {
        if (argc < 2) {
            printf("usage: bulum repo %s <name>\n", sub);
            return 2;
        }
        int want_enable = !strcmp(sub, "enable");
        repo_load();
        if (!repo_find(argv[1])) {
            printf(C_RED "  bulum: repository '%s' not found\n" C_RESET, argv[1]);
            return 1;
        }
        int rc = repos_conf_rewrite(argv[1], want_enable);
        if (rc != 0) {
            printf(C_RED "  bulum: could not update %s\n" C_RESET, REPOS_CONF);
            return 1;
        }
        if (json_mode)
            printf("{\"name\":\"%s\",\"enabled\":%s}", argv[1],
                   want_enable ? "true" : "false");
        else
            printf("  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
                   "repository %s %s\n", argv[1], want_enable ? "enabled" : "disabled");
        return 0;
    }
    printf("usage: bulum repo <list|add|remove|enable|disable> ...\n");
    return 2;
}

int main(int argc, char **argv)
{
    ensure_dirs();
    recover_tx();
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--json") || !strcmp(argv[1], "-j")) {
        json_mode = 1;
        argc--;
        for (int i = 1; i < argc; i++)
            argv[i] = argv[i + 1];
    }
    const char *cmd = argv[1];
    int rc;
    if (!strcmp(cmd, "update"))
        rc = cmd_update();
    else if (!strcmp(cmd, "install"))
        rc = cmd_install(argc - 2, argv + 2);
    else if (!strcmp(cmd, "remove"))
        rc = cmd_remove(argc - 2, argv + 2);
    else if (!strcmp(cmd, "search"))
        rc = cmd_search(argc - 2, argv + 2);
    else if (!strcmp(cmd, "info"))
        rc = cmd_info(argc - 2, argv + 2);
    else if (!strcmp(cmd, "list"))
        rc = cmd_list();
    else if (!strcmp(cmd, "upgrade"))
        rc = cmd_upgrade();
    else if (!strcmp(cmd, "clean"))
        rc = cmd_clean();
    else if (!strcmp(cmd, "doctor"))
        rc = cmd_doctor();
    else if (!strcmp(cmd, "owns"))
        rc = cmd_owns(argc - 2, argv + 2);
    else if (!strcmp(cmd, "verify"))
        rc = cmd_verify(argc - 2, argv + 2);
    else if (!strcmp(cmd, "arch"))
        rc = cmd_arch();
    else if (!strcmp(cmd, "repo"))
        rc = cmd_repo(argc - 2, argv + 2);
    else if (!strcmp(cmd, "version") || !strcmp(cmd, "--version"))
        rc = (printf("bulum/bpk %s (Phase 12)\n", VERSION), 0);
    else if (!strcmp(cmd, "help") || !strcmp(cmd, "-h"))
        rc = (usage(argv[0]), 0);
    else {
        usage(argv[0]);
        rc = 2;
    }
    (void)package_has_list;
    return rc;
}