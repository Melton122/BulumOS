/*
 * binputtest - BulumOS input device tester (Phase 8)
 *
 * Prints keyboard/mouse events as they arrive.  Exits cleanly on
 * Ctrl+C (SIGINT) or after --timeout seconds.
 *
 *   binputtest              monitor all devices until Ctrl+C
 *   binputtest --list       list detected devices and exit
 *   binputtest --timeout N  monitor for N seconds then exit
 *   binputtest --help       usage
 *   binputtest --version    version
 */

#include "binput.h"

#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_quit = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void list_devices(void)
{
    printf("Input devices: %d\n", binput_count());
    for (int i = 0; i < binput_count(); i++) {
        printf("  %s %s (%s)\n", binput_path(i),
               binput_kind(i) == BINPUT_KBD ? "keyboard" :
               binput_kind(i) == BINPUT_MOUSE ? "mouse" : "other",
               binput_name(i));
    }
}

static void handle_event(const binput_event_t *ev)
{
    const char *devtype = ev->kind == BINPUT_KBD ? "Keyboard" :
                          ev->kind == BINPUT_MOUSE ? "Mouse" : "Device";
    if (ev->type == EV_KEY) {
        const char *state = ev->value == 1 ? "pressed" :
                            ev->value == 0 ? "released" : "repeat";
        printf("%s:\n%s %s\n", devtype, binput_code_name(ev->type, ev->code), state);
    } else if (ev->type == EV_REL) {
        if (ev->code == REL_X || ev->code == REL_Y)
            printf("%s:\n%s %+d\n", devtype, binput_code_name(ev->type, ev->code), ev->value);
        else
            printf("%s:\n%s %d\n", devtype, binput_code_name(ev->type, ev->code), ev->value);
    } else if (ev->type == EV_ABS) {
        printf("%s:\n%s %d\n", devtype, binput_code_name(ev->type, ev->code), ev->value);
    }
    /* EV_SYN is silently ignored */
}

static void usage(const char *p)
{
    printf("usage: %s [--list] [--timeout N] [--help] [--version]\n", p);
}

int main(int argc, char **argv)
{
    int timeout = -1; /* -1 = forever */
    int do_list = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("binputtest %s (BulumOS Phase 8)\n", BINPUT_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--list") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout = atoi(argv[++i]);
            if (timeout < 0)
                timeout = 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    int n = binput_init();
    if (n == 0) {
        printf("Input devices: none\n");
        return 1;
    }

    if (do_list) {
        list_devices();
        binput_shutdown();
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("Input devices: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("  %s %s (%s)\n", binput_path(i),
               binput_kind(i) == BINPUT_KBD ? "keyboard" :
               binput_kind(i) == BINPUT_MOUSE ? "mouse" : "other",
               binput_name(i));

    while (!g_quit) {
        int r = binput_poll(timeout * 1000);
        if (r < 0) {
            printf("input error\n");
            break;
        }
        if (r == 0)
            break; /* timeout expired */
        binput_event_t ev;
        while (binput_read(&ev))
            handle_event(&ev);
        fflush(stdout);
    }

    binput_shutdown();
    printf("exit\n");
    return 0;
}