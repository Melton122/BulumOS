/*
 * BulumOS user management tool.
 *
 *   buser list                 list users + groups
 *   buser add USER [shell]     add a user (prompts for password; default /bin/bsh)
 *   buser del USER             remove a user (keeps home dir)
 *   buser passwd [USER]        set/change password (defaults to current user)
 *   buser groups [USER]        show a user's groups
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <crypt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"

static const char *LOGIN_SHELL = "/bin/bsh";
static const char *PASSWD      = "/etc/passwd";
static const char *SHADOW      = "/etc/shadow";
static const char *GROUP       = "/etc/group";

/* Env overrides for testability (BUSER_PASSWD / BUSER_SHADOW / BUSER_GROUP /
 * BUSER_ROOT_OK=1 bypasses the privilege check). */
static const char *p_path(const char *env, const char *def)
{
    const char *v = getenv(env);
    return (v && *v) ? v : def;
}
static const char *pw_path(void) { return p_path("BUSER_PASSWD", PASSWD); }
static const char *sh_path(void) { return p_path("BUSER_SHADOW", SHADOW); }
static const char *gr_path(void) { return p_path("BUSER_GROUP",  GROUP);  }

static int is_root(void) {
    const char *o = getenv("BUSER_ROOT_OK");
    if (o && o[0] == '1') return 1;
    return geteuid() == 0;
}

/* Read a password. Uses /dev/tty to mask echo only when stdin is itself a
 * tty; otherwise (piped/scripted input) reads plainly from stdin so tests
 * and `echo pw | buser passwd` work without hanging on /dev/tty. */
static int prompt_password(char *buf, size_t bufsz, const char *prompt)
{
    int tty_in = isatty(STDIN_FILENO);
    FILE *t = tty_in ? fopen("/dev/tty", "w") : NULL;
    if (t) {
        fprintf(t, "%s", prompt);
        fflush(t);
    }
    if (tty_in) {
        struct termios old, new;
        tcgetattr(STDIN_FILENO, &old);
        new = old;
        new.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new);
        if (fgets(buf, bufsz, stdin) == NULL) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
            if (t) fclose(t);
            return -1;
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        if (t) {
            fprintf(t, "\n");
            fflush(t);
            fclose(t);
        }
    } else {
        if (fgets(buf, bufsz, stdin) == NULL)
            return -1;
    }
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

/* Compute a SHA-512 crypt hash using busybox mkpasswd (compiled into the
 * rootfs; avoids any external libcrypt dependency for the static cross build). */
static int compute_hash(const char *pw, char *out, size_t outsz)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/buser_pw.XXXXXX");
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    dprintf(fd, "%s", pw);
    close(fd);

    char rcmd[320];
    snprintf(rcmd, sizeof(rcmd), "mkpasswd -m 6 < %s 2>/dev/null", tmp);
    FILE *p = popen(rcmd, "r");
    if (!p) { unlink(tmp); return -1; }
    int rc = -1;
    if (fgets(out, (int)outsz, p) != NULL) {
        out[strcspn(out, "\n")] = 0;
        if (out[0] == '$') rc = 0;
    }
    pclose(p);
    unlink(tmp);
    return rc;
}

/* /home/USER/.profile buffer helper (static, single-use) */
static char *home_profile(const char *home)
{
    static char p[512];
    snprintf(p, sizeof(p), "%s/.profile", home);
    return p;
}

static int cmd_list(void)
{
    struct passwd *pw;
    printf(C_BOLD "users:" C_RESET "\n");
    setpwent();
    while ((pw = getpwent()) != NULL) {
        printf("  %-14s uid=%-5d gid=%-5d shell=%s\n",
               pw->pw_name, pw->pw_uid, pw->pw_gid,
               (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "(none)");
    }
    endpwent();

    printf(C_BOLD "groups:" C_RESET "\n");
    struct group *gr;
    setgrent();
    while ((gr = getgrent()) != NULL)
        printf("  %-14s gid=%-5d\n", gr->gr_name, gr->gr_gid);
    endgrent();
    return 0;
}

/* Drop every line in `path` whose colon-first-field equals `name`. */
static int rewrite_drop(const char *path, const char *name)
{
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); return 1; }
    char *tmp = NULL;
    asprintf(&tmp, "%s.bak", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); return 1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int len = strlen(name);
    while ((n = getline(&line, &cap, in)) != -1) {
        if (strncmp(line, name, len) == 0 && line[len] == ':')
            continue;
        fputs(line, out);
    }
    fclose(in); fclose(out);
    int rc = rename(tmp, path);
    free(tmp); free(line);
    return rc != 0;
}

/* Remove a user from /etc/group: drop the private group (field 1 == name)
 * and remove `name` from any member lists (field 4). */
static int rewrite_group_del(const char *path, const char *name)
{
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); return 1; }
    char *tmp = NULL;
    asprintf(&tmp, "%s.bak", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); return 1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int len = strlen(name);
    while ((n = getline(&line, &cap, in)) != -1) {
        if (strncmp(line, name, len) == 0 && line[len] == ':')
            continue; /* drop private group */
        /* group:x:gid:mem1,mem2,... */
        char *c1 = strchr(line, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        char *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
        char *c4 = c3 ? strchr(c3 + 1, ':') : NULL;
        if (c4 && c4[1] && c4[1] != '\n') {
            size_t mlen = strcspn(c4 + 1, "\n");
            char *mlist = strndup(c4 + 1, mlen);
            char *nov[64];
            int nv = 0;
            int removed = 0;
            char *sav = NULL;
            char *tok = strtok_r(mlist, ",", &sav);
            while (tok && nv < 63) {
                if (strcmp(tok, name) != 0)
                    nov[nv++] = tok;
                else
                    removed = 1;
                tok = strtok_r(NULL, ",", &sav);
            }
            if (removed) {
                fprintf(out, "%.*s", (int)(c4 - line + 1), line);
                for (int i = 0; i < nv; i++)
                    fprintf(out, "%s%s", i ? "," : "", nov[i]);
                fprintf(out, "\n");
                free(mlist);
                free(line); line = NULL; cap = 0;
                continue;
            }
            free(mlist);
        }
        fputs(line, out);
    }
    fclose(in); fclose(out);
    int rc = rename(tmp, path);
    free(tmp); free(line);
    return rc != 0;
}

static int cmd_add(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: buser add USER [shell]\n");
        return 2;
    }
    const char *name = argv[0];
    const char *shell = argc >= 2 ? argv[1] : LOGIN_SHELL;
    if (getpwnam(name)) {
        fprintf(stderr, C_RED "buser:" C_RESET " user '%s' already exists\n", name);
        return 1;
    }
    if (strpbrk(name, ":/ \t\n\\") || !*name) {
        fprintf(stderr, C_RED "buser:" C_RESET " invalid username '%s'\n", name);
        return 2;
    }
    if (!is_root() && getuid() != 0) {
        fprintf(stderr, C_RED "buser:" C_RESET " must be root\n");
        return 1;
    }

    uid_t uid = 1000;
    while (getpwuid(uid)) uid++;
    gid_t gid = uid;

    char pw1[128], pw2[128];
    if (prompt_password(pw1, sizeof(pw1), "New password: ") != 0) {
        fprintf(stderr, "buser: aborted\n");
        return 1;
    }
    if (prompt_password(pw2, sizeof(pw2), "Retype password: ") != 0) {
        fprintf(stderr, "buser: aborted\n");
        return 1;
    }
    if (strcmp(pw1, pw2) != 0) {
        fprintf(stderr, C_RED "buser:" C_RESET " passwords do not match\n");
        return 1;
    }
    if (!pw1[0]) {
        fprintf(stderr, C_RED "buser:" C_RESET " empty password not allowed\n");
        return 1;
    }
    char hash[256];
    if (compute_hash(pw1, hash, sizeof(hash)) != 0) {
        fprintf(stderr, "buser: could not hash password\n");
        return 1;
    }

    FILE *gf = fopen(gr_path(), "a");
    if (!gf) { perror("group"); return 1; }
    fprintf(gf, "%s:x:%u:\n", name, (unsigned)gid);
    fclose(gf);

    FILE *pf = fopen(pw_path(), "a");
    if (!pf) { perror("passwd"); return 1; }
    fprintf(pf, "%s:x:%u:%u::/home/%s:%s\n", name, (unsigned)uid, (unsigned)gid, name, shell);
    fclose(pf);

    FILE *sf = fopen(sh_path(), "a");
    if (!sf) { perror("shadow"); return 1; }
    fprintf(sf, "%s:%s:19786:0:99999:7:::\n", name, hash);
    fclose(sf);

    char home[256];
    snprintf(home, sizeof(home), "/home/%s", name);
    if (mkdir(home, 0700) == 0) {
        chown(home, uid, gid);
        FILE *prof = fopen(home_profile(home), "w");
        if (prof) { fprintf(prof, "# %s profile\n", name); fclose(prof); }
    }

    printf(C_GREEN "[ OK ]" C_RESET " user '%s' created (uid %u, home %s)\n",
           name, (unsigned)uid, home);
    return 0;
}

static int cmd_del(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: buser del USER\n");
        return 2;
    }
    const char *name = argv[0];
    if (!is_root() && getuid() != 0) {
        fprintf(stderr, C_RED "buser:" C_RESET " must be root\n");
        return 1;
    }
    if (!getpwnam(name)) {
        fprintf(stderr, C_RED "buser:" C_RESET " no such user '%s'\n", name);
        return 1;
    }
    if (strcmp(name, "root") == 0) {
        fprintf(stderr, C_RED "buser:" C_RESET " cannot delete root\n");
        return 1;
    }
    if (rewrite_drop(pw_path(), name) != 0) return 1;
    if (rewrite_drop(sh_path(), name) != 0) return 1;
    rewrite_group_del(gr_path(), name);
    printf(C_GREEN "[ OK ]" C_RESET " user '%s' removed\n", name);
    return 0;
}

static int cmd_passwd(int argc, char **argv)
{
    const char *name = argc >= 1 ? argv[0] : NULL;
    if (!name) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) name = pw->pw_name;
    }
    if (!name) {
        fprintf(stderr, "buser: cannot determine user\n");
        return 2;
    }
    if (!getpwnam(name)) {
        fprintf(stderr, C_RED "buser:" C_RESET " no such user '%s'\n", name);
        return 1;
    }
    char pw1[128], pw2[128];
    if (prompt_password(pw1, sizeof(pw1), "New password: ") != 0) return 1;
    if (prompt_password(pw2, sizeof(pw2), "Retype password: ") != 0) return 1;
    if (strcmp(pw1, pw2) != 0) {
        fprintf(stderr, C_RED "buser:" C_RESET " passwords do not match\n");
        return 1;
    }
    if (!pw1[0]) {
        fprintf(stderr, C_RED "buser:" C_RESET " empty password not allowed\n");
        return 1;
    }
    char hash[256];
    if (compute_hash(pw1, hash, sizeof(hash)) != 0) return 1;

    if (rewrite_drop(sh_path(), name) != 0) return 1;
    FILE *sf = fopen(sh_path(), "a");
    if (!sf) { perror("shadow"); return 1; }
    fprintf(sf, "%s:%s:19786:0:99999:7:::\n", name, hash);
    fclose(sf);
    printf(C_GREEN "[ OK ]" C_RESET " password updated for '%s'\n", name);
    return 0;
}

static int cmd_groups(int argc, char **argv)
{
    const char *name = argc >= 1 ? argv[0] : NULL;
    if (!name) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) name = pw->pw_name;
    }
    if (!name) return 2;
    struct passwd *pw = getpwnam(name);
    if (!pw) { fprintf(stderr, "buser: no such user\n"); return 1; }
    printf("primary: uid=%d gid=%d\n", pw->pw_uid, pw->pw_gid);
    printf("groups:  ");
    int first = 1;
    setgrent();
    struct group *gr;
    while ((gr = getgrent()) != NULL) {
        int in = (gr->gr_gid == pw->pw_gid);
        if (!in && gr->gr_mem) {
            for (int i = 0; gr->gr_mem[i]; i++) {
                if (strcmp(gr->gr_mem[i], name) == 0) { in = 1; break; }
            }
        }
        if (in) {
            printf("%s%s", first ? "" : " ", gr->gr_name);
            first = 0;
        }
    }
    endgrent();
    printf("\n");
    return 0;
}

static void usage(void)
{
    printf("usage: buser <cmd> [args]\n"
           "  list                 list users + groups\n"
           "  add USER [shell]     add user (prompts for password)\n"
           "  del USER             remove user\n"
           "  passwd [USER]        set password (defaults to current user)\n"
           "  groups [USER]        show user's groups\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "list";
    char **args = argv + 2;
    int n = argc - 2;

    if (strcmp(cmd, "list") == 0)   return cmd_list();
    if (strcmp(cmd, "add") == 0)    return cmd_add(n, args);
    if (strcmp(cmd, "del") == 0)    return cmd_del(n, args);
    if (strcmp(cmd, "passwd") == 0) return cmd_passwd(n, args);
    if (strcmp(cmd, "groups") == 0) return cmd_groups(n, args);
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage();
        return 0;
    }
    fprintf(stderr, "buser: unknown command '%s'\n", cmd);
    usage();
    return 2;
}
