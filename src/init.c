/*
 * BulumOS init - PID 1
 *
 * Boot sequence:
 *   1. mount proc/sysfs/devtmpfs/devpts/tmpfs
 *   2. set hostname from /etc/hostname
 *   3. print splash banner
 *   4. run "bctl boot" (service manager) unless recovery mode
 *   5. spawn /bin/login on the console, respawn on exit
 *
 * Signals:
 *   SIGINT  -> reboot
 *   SIGTERM -> poweroff
 *   SIGUSR1 -> reboot (ctl-alt-del style)
 *
 * Kernel cmdline flags:
 *   single    -> drop to shell, no services
 *   recovery  -> drop to shell, no services
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/utsname.h>

#define CONSOLE "/dev/console"

static volatile sig_atomic_t shutdown_mode = 0; /* 0=none, 1=poweroff, 2=reboot */
static int recovery_mode = 0;

/* ANSI colors */
#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_GREEN "\033[32m"
#define C_YELLOW "\033[33m"
#define C_RED "\033[31m"
#define C_CYAN "\033[36m"
#define C_GRAY "\033[90m"

static void out(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void banner(void)
{
    struct utsname u;
    if (uname(&u) != 0)
        strcpy(u.machine, "unknown");
    out("\033[2J\033[H"); /* clear screen, home cursor */
    out(C_BOLD C_CYAN
        "\n"
        "   ____        _                     ____  _____\n"
        "  | __ ) _   _| |_ __ _ _ __ ___    / ___|| ____|\n"
        "  |  _ \\| | | | __/ _` | '_ ` _ \\  \\___ \\|  _|  \n"
        "  | |_) | |_| | || (_| | | | | | |  ___) | |___ \n"
        "  |____/ \\__,_|\\__\\__,_|_| |_| |_| |____/|_____|\n"
        C_RESET "\n");
    out(C_GRAY "  BulumOS 1.2 - Linux 7.1.5 (");
    out(u.machine);
    out(")\n" C_RESET);
    out(C_GRAY "  ----------------------------\n\n" C_RESET);
}

static void status_line(const char *what, int ok)
{
    out(ok ? C_BOLD "  [ " C_GREEN "OK" C_RESET C_BOLD " ] " C_RESET
           : C_BOLD "  [ " C_RED "!!" C_RESET C_BOLD " ] " C_RESET);
    out(what);
    out("\n");
}

static void set_console(void)
{
    int fd = open(CONSOLE, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fd = open("/dev/null", O_RDWR);
        if (fd < 0)
            return;
    }
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
        close(fd);
}

static void mount_pseudo_fs(void)
{
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/tmp", 01777);

    int ok = 1;
    char err[256] = "";
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { ok = 0; snprintf(err, sizeof(err), "proc: %s", strerror(errno)); }
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) != 0) { ok = 0; snprintf(err, sizeof(err), "sysfs: %s", strerror(errno)); }
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0) { ok = 0; snprintf(err, sizeof(err), "devtmpfs: %s", strerror(errno)); }
    mkdir("/dev/pts", 0755);
    mkdir("/dev/shm", 01777);
    if (mount("devpts", "/dev/pts", "devpts", 0, NULL) != 0) { ok = 0; snprintf(err, sizeof(err), "devpts: %s", strerror(errno)); }
    if (mount("tmpfs", "/tmp", "tmpfs", 0, NULL) != 0) { ok = 0; snprintf(err, sizeof(err), "tmpfs: %s", strerror(errno)); }

    status_line("Mounting filesystems", ok);
    if (err[0]) {
        out(C_RED "       ");
        out(err);
        out("\n" C_RESET);
    }
}

/* Parse /proc/cmdline for recovery/single flags */
static void parse_cmdline(void)
{
    char buf[4096];
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    char *tok = strtok(buf, " \n");
    while (tok) {
        if (strcmp(tok, "single") == 0 || strcmp(tok, "recovery") == 0 ||
            strcmp(tok, "bulum.recovery") == 0) {
            recovery_mode = 1;
            return;
        }
        tok = strtok(NULL, " \n");
    }
}

/* Set hostname from /etc/hostname (fall back to "bulumos") */
static void set_hostname(void)
{
    char buf[256];
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
buf[n] = '\0';
    char *eol = strchr(buf, '\n');
    if (eol)
        *eol = '\0';
    if (buf[0])
        sethostname(buf, strlen(buf));
}

/* Run a program, inherit stdio; return exit status */
static int run_prog(char *const args[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execv(args[0], args);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

static void bctl_boot(void)
{
    char *args[] = { "/sbin/bctl", "boot", NULL };
    run_prog(args);
}

/* VM/qemu diagnostic hook: if /etc/bulum/autotest.sh exists, run it with
 * stdio on the console (its output lands on the serial console) and wait
 * for it to finish.  Absent on normal images, so this is a no-op there. */
static void run_autotest(void)
{
    if (access("/etc/bulum/autotest.sh", R_OK) != 0)
        return;
    out("\n" C_YELLOW "  Running autotest...\n" C_RESET);
    char *args[] = { "/bin/sh", "/etc/bulum/autotest.sh", NULL };
    run_prog(args);
}

/* collect zombies so stopped services don't linger as defunct */
static void reap_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void bctl_shutdown(void)
{
    char *args[] = { "/sbin/bctl", "shutdown", NULL };
    run_prog(args);
}

static void spawn_console_prog(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        setsid();
        ioctl(0, TIOCSCTTY, 0);
        char *args[3];
        if (!recovery_mode && access("/bin/login", X_OK) == 0) {
            args[0] = "/bin/login";
            args[1] = NULL;
        } else {
            /* Bulum OS shell, fall back to busybox sh */
            args[0] = access("/bin/bsh", X_OK) == 0 ? "/bin/bsh" : "/bin/sh";
            args[1] = NULL;
        }
        execv(args[0], args);
        _exit(127);
    }
    /* wait for the shell; when it exits, respawn.
     * a shutdown signal interrupts waitpid with EINTR -> bail out */
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR && !shutdown_mode)
        ;
    (void)status;
}

static void do_shutdown(int mode)
{
out(C_YELLOW "\n  Shutting down...\n" C_RESET);
    bctl_shutdown();
    sync();
    sleep(1);
    if (mode == 2)
        out(C_GREEN "  Goodbye. Rebooting...\n" C_RESET);
    else
        out(C_GREEN "  Goodbye. Powering off...\n" C_RESET);
    if (mode == 2) {
        reboot(RB_AUTOBOOT);
    } else {
        reboot(RB_POWER_OFF);
    }
    /* fallback */
    for (;;)
        pause();
}

static void on_signal(int sig)
{
    if (sig == SIGINT || sig == SIGUSR1)
        shutdown_mode = 2; /* reboot */
    else if (sig == SIGTERM || sig == SIGUSR2)
        shutdown_mode = 1; /* poweroff */
}

static void install_signal(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* no SA_RESTART: so a signal can interrupt waitpid() with EINTR */
    sigaction(sig, &sa, NULL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    set_console();
    install_signal(SIGINT);
    install_signal(SIGTERM);
    install_signal(SIGUSR1);
    install_signal(SIGUSR2);

    banner();
    mount_pseudo_fs();
    parse_cmdline();
    set_hostname();
    status_line("Setting hostname", 1);

    if (recovery_mode) {
        out(C_YELLOW "  === RECOVERY MODE ===\n" C_RESET);
        out(C_GRAY "  Filesystems mounted, services not started.\n\n" C_RESET);
    } else {
        bctl_boot();
    }

    run_autotest();

    out("\n" C_CYAN "  BulumOS is ready. Type 'poweroff' or 'reboot'.\n\n" C_RESET);

    /* main loop: respawn login, handle shutdown signals */
    for (;;) {
        if (shutdown_mode)
            do_shutdown(shutdown_mode);
        reap_children();
        spawn_console_prog();
        if (shutdown_mode)
            do_shutdown(shutdown_mode);
    }
    return 0;
}
