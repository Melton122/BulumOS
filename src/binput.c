/*
 * binput.c - BulumOS input subsystem (Phase 8)
 *
 * evdev front-end with device classification (keyboard/mouse),
 * nonblocking reads and a bounded FIFO.  All parsing is length- and
 * type-checked; a malformed device cannot overflow anything.
 *
 * Device classification:
 *   keyboard  - EV_KEY with a letter/digit key (KEY_A..KEY_Z / KEY_0..KEY_9)
 *   mouse     - EV_REL (relative pointer)
 *   else      - BINPUT_UNKNOWN (still readable, e.g. absolute touchscreen)
 */

#define _GNU_SOURCE

#include "binput.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define SYS_INPUT "/sys/class/input"

typedef struct {
    int fd;
    int kind;
    char path[64];
    char name[128];
} binput_dev_t;

static binput_dev_t g_devs[BINPUT_MAX_DEV];
static int g_ndev;
static char g_dir[64] = INPUT_DIR;
static char g_sysfs[96] = SYS_INPUT;

/* bounded FIFO of parsed events */
static binput_event_t g_q[BINPUT_QUEUE_SIZE];
static int g_q_head, g_q_tail, g_q_len;

/* ---------- sysfs helpers ---------- */

static char *read_sysfs(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    if (!fgets(buf, (int)bufsz, f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

/* inputX for a given eventX, e.g. /sys/class/input/event0/device -> input0 */
static void event_to_input(const char *event, char *out, size_t outsz)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/device", g_sysfs, event);
    /* the "device" attribute is a symlink to the inputN directory;
     * readlink() resolves it, fopen() cannot (target is a directory) */
    ssize_t n = readlink(path, out, outsz - 1);
    if (n > 0) {
        out[n] = 0;
        const char *slash = strrchr(out, '/');
        if (slash) {
            char *dst = out;
            for (const char *p = slash + 1; *p && dst < out + outsz - 1; p++)
                *dst++ = *p;
            *dst = 0;
        }
        return;
    }
    snprintf(out, outsz, "%s", event);
}

/* parse a sysfs bitmap like "0 0 0 3f 0 0 ..." (hex words, MSB first):
 * word 0 holds the HIGHEST bits, so bit N lives in word
 * (nwords-1-N/32) at offset N%32. */
static int sysfs_bit_test(const char *bits, int bit)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", bits);
    char *words[64];
    int nwords = 0;
    char *tok = strtok(tmp, " \t");
    while (tok && nwords < 64) {
        words[nwords++] = tok;
        tok = strtok(NULL, " \t");
    }
    if (nwords == 0)
        return 0;
    int widx = nwords - 1 - bit / 32;
    if (widx < 0 || widx >= nwords)
        return 0;
    unsigned long v = strtoul(words[widx], NULL, 16);
    return (v >> (bit % 32)) & 1;
}

/* ---------- classification ---------- */

static int classify(int idx)
{
    char path[256], buf[512];
    char input[64];
    const char *base = strrchr(g_devs[idx].path, '/');
    event_to_input(base ? base + 1 : g_devs[idx].path, input, sizeof(input));

    /* capabilities */
    snprintf(path, sizeof(path), "%s/%s/capabilities/ev", g_sysfs, input);
    if (!read_sysfs(path, buf, sizeof(buf)))
        return BINPUT_UNKNOWN;
    if (!sysfs_bit_test(buf, EV_KEY))
        return BINPUT_UNKNOWN;
    snprintf(path, sizeof(path), "%s/%s/capabilities/key", g_sysfs, input);
    if (!read_sysfs(path, buf, sizeof(buf)))
        return BINPUT_UNKNOWN;

    int has_letter = 0;
    for (int c = KEY_A; c <= KEY_Z; c++)
        if (sysfs_bit_test(buf, c)) { has_letter = 1; break; }
    for (int c = KEY_1; c <= KEY_0 + 10; c++) {
        if (c >= KEY_1 && c <= KEY_9)
            if (sysfs_bit_test(buf, c)) { has_letter = 1; break; }
    }
    if (has_letter)
        return BINPUT_KBD;

    snprintf(path, sizeof(path), "%s/%s/capabilities/rel", g_sysfs, input);
    if (read_sysfs(path, buf, sizeof(buf))) {
        if (sysfs_bit_test(buf, REL_X) || sysfs_bit_test(buf, REL_Y))
            return BINPUT_MOUSE;
    }
    return BINPUT_UNKNOWN;
}

/* ---------- lifecycle ---------- */

int binput_init(void)
{
    /* test hook: BINPUT_DIR overrides the device directory */
    const char *dir = getenv("BINPUT_DIR");
    if (dir && dir[0])
        snprintf(g_dir, sizeof(g_dir), "%s", dir);
    const char *sysfs = getenv("BINPUT_SYSFS");
    if (sysfs && sysfs[0])
        snprintf(g_sysfs, sizeof(g_sysfs), "%s", sysfs);
    DIR *d = opendir(g_dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_ndev < BINPUT_MAX_DEV) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        binput_dev_t *dev = &g_devs[g_ndev];
        memset(dev, 0, sizeof(*dev));
        snprintf(dev->path, sizeof(dev->path), "%s/", g_dir);
        size_t used = strlen(dev->path);
        if (used + 1 < sizeof(dev->path))
            snprintf(dev->path + used, sizeof(dev->path) - used, "%s", e->d_name);
        dev->fd = open(dev->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (dev->fd < 0)
            continue;
        /* device name */
        char input[64], path[256];
        event_to_input(e->d_name, input, sizeof(input));
        snprintf(path, sizeof(path), "%s/%s/name", g_sysfs, input);
        read_sysfs(path, dev->name, sizeof(dev->name));
        if (!dev->name[0])
            snprintf(dev->name, sizeof(dev->name), "%.*s",
                     (int)sizeof(dev->name) - 1, e->d_name);
        dev->kind = classify(g_ndev);
        g_ndev++;
    }
    closedir(d);
    return g_ndev;
}

void binput_shutdown(void)
{
    for (int i = 0; i < g_ndev; i++) {
        if (g_devs[i].fd >= 0) {
            close(g_devs[i].fd);
            g_devs[i].fd = -1;
        }
    }
    g_ndev = 0;
    g_q_head = g_q_tail = g_q_len = 0;
}

int binput_count(void)      { return g_ndev; }
int binput_kind(int dev)
{
    if (dev < 0 || dev >= g_ndev)
        return BINPUT_UNKNOWN;
    return g_devs[dev].kind;
}
const char *binput_name(int dev)
{
    if (dev < 0 || dev >= g_ndev)
        return "";
    return g_devs[dev].name;
}
const char *binput_path(int dev)
{
    if (dev < 0 || dev >= g_ndev)
        return "";
    return g_devs[dev].path;
}

/* ---------- events ---------- */

int binput_poll(int timeout_ms)
{
    if (g_ndev == 0)
        return -1;
    struct pollfd fds[BINPUT_MAX_DEV];
    int n = 0;
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i].fd >= 0) {
            fds[n].fd = g_devs[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
    if (n == 0)
        return -1;
    int r = poll(fds, (nfds_t)n, timeout_ms);
    if (r <= 0)
        return r; /* 0 = timeout, -1 = error */

    for (int i = 0; i < n; i++) {
        if (!(fds[i].revents & POLLIN))
            continue;
        /* drain this fd into the bounded queue */
        struct input_event ev;
        for (;;) {
            ssize_t rd = read(fds[i].fd, &ev, sizeof(ev));
            if (rd < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    break;
                break; /* device error: drop it */
            }
            if (rd != sizeof(ev))
                break; /* partial read: kernel guarantees full frames; drop */
            if (g_q_len >= BINPUT_QUEUE_SIZE)
                break; /* queue full: drop oldest input, stay bounded */
            binput_event_t *qe = &g_q[g_q_tail];
            qe->device = i;
            qe->kind = g_devs[i].kind;
            qe->type = ev.type;
            qe->code = ev.code;
            qe->value = ev.value;
            g_q_tail = (g_q_tail + 1) % BINPUT_QUEUE_SIZE;
            g_q_len++;
        }
    }
    return g_q_len > 0 ? 1 : 0;
}

int binput_read(binput_event_t *ev)
{
    if (g_q_len == 0)
        return 0;
    *ev = g_q[g_q_head];
    g_q_head = (g_q_head + 1) % BINPUT_QUEUE_SIZE;
    g_q_len--;
    return 1;
}

/* ---------- code names ---------- */

static const struct {
    uint16_t type, code;
    const char *name;
} code_table[] = {
    { EV_KEY, KEY_ESC,       "KEY_ESC" },
    { EV_KEY, KEY_1,         "KEY_1" },   { EV_KEY, KEY_2, "KEY_2" },
    { EV_KEY, KEY_3,         "KEY_3" },   { EV_KEY, KEY_4, "KEY_4" },
    { EV_KEY, KEY_5,         "KEY_5" },   { EV_KEY, KEY_6, "KEY_6" },
    { EV_KEY, KEY_7,         "KEY_7" },   { EV_KEY, KEY_8, "KEY_8" },
    { EV_KEY, KEY_9,         "KEY_9" },   { EV_KEY, KEY_0, "KEY_0" },
    { EV_KEY, KEY_MINUS,     "KEY_MINUS" }, { EV_KEY, KEY_EQUAL, "KEY_EQUAL" },
    { EV_KEY, KEY_BACKSPACE, "KEY_BACKSPACE" }, { EV_KEY, KEY_TAB, "KEY_TAB" },
    { EV_KEY, KEY_Q, "KEY_Q" }, { EV_KEY, KEY_W, "KEY_W" }, { EV_KEY, KEY_E, "KEY_E" },
    { EV_KEY, KEY_R, "KEY_R" }, { EV_KEY, KEY_T, "KEY_T" }, { EV_KEY, KEY_Y, "KEY_Y" },
    { EV_KEY, KEY_U, "KEY_U" }, { EV_KEY, KEY_I, "KEY_I" }, { EV_KEY, KEY_O, "KEY_O" },
    { EV_KEY, KEY_P, "KEY_P" }, { EV_KEY, KEY_LEFTBRACE, "KEY_LEFTBRACE" },
    { EV_KEY, KEY_RIGHTBRACE, "KEY_RIGHTBRACE" }, { EV_KEY, KEY_ENTER, "KEY_ENTER" },
    { EV_KEY, KEY_A, "KEY_A" }, { EV_KEY, KEY_S, "KEY_S" }, { EV_KEY, KEY_D, "KEY_D" },
    { EV_KEY, KEY_F, "KEY_F" }, { EV_KEY, KEY_G, "KEY_G" }, { EV_KEY, KEY_H, "KEY_H" },
    { EV_KEY, KEY_J, "KEY_J" }, { EV_KEY, KEY_K, "KEY_K" }, { EV_KEY, KEY_L, "KEY_L" },
    { EV_KEY, KEY_SEMICOLON, "KEY_SEMICOLON" }, { EV_KEY, KEY_APOSTROPHE, "KEY_APOSTROPHE" },
    { EV_KEY, KEY_GRAVE, "KEY_GRAVE" }, { EV_KEY, KEY_LEFTSHIFT, "KEY_LEFTSHIFT" },
    { EV_KEY, KEY_BACKSLASH, "KEY_BACKSLASH" }, { EV_KEY, KEY_Z, "KEY_Z" },
    { EV_KEY, KEY_X, "KEY_X" }, { EV_KEY, KEY_C, "KEY_C" }, { EV_KEY, KEY_V, "KEY_V" },
    { EV_KEY, KEY_B, "KEY_B" }, { EV_KEY, KEY_N, "KEY_N" }, { EV_KEY, KEY_M, "KEY_M" },
    { EV_KEY, KEY_COMMA, "KEY_COMMA" }, { EV_KEY, KEY_DOT, "KEY_DOT" },
    { EV_KEY, KEY_SLASH, "KEY_SLASH" }, { EV_KEY, KEY_RIGHTSHIFT, "KEY_RIGHTSHIFT" },
    { EV_KEY, KEY_KPASTERISK, "KEY_KPASTERISK" }, { EV_KEY, KEY_LEFTALT, "KEY_LEFTALT" },
    { EV_KEY, KEY_SPACE, "KEY_SPACE" }, { EV_KEY, KEY_CAPSLOCK, "KEY_CAPSLOCK" },
    { EV_KEY, KEY_F1, "KEY_F1" }, { EV_KEY, KEY_F2, "KEY_F2" }, { EV_KEY, KEY_F3, "KEY_F3" },
    { EV_KEY, KEY_F4, "KEY_F4" }, { EV_KEY, KEY_F5, "KEY_F5" }, { EV_KEY, KEY_F6, "KEY_F6" },
    { EV_KEY, KEY_F7, "KEY_F7" }, { EV_KEY, KEY_F8, "KEY_F8" }, { EV_KEY, KEY_F9, "KEY_F9" },
    { EV_KEY, KEY_F10, "KEY_F10" }, { EV_KEY, KEY_F11, "KEY_F11" },
    { EV_KEY, KEY_F12, "KEY_F12" },
    { EV_KEY, KEY_NUMLOCK, "KEY_NUMLOCK" }, { EV_KEY, KEY_SCROLLLOCK, "KEY_SCROLLLOCK" },
    { EV_KEY, KEY_LEFTCTRL, "KEY_LEFTCTRL" }, { EV_KEY, KEY_RIGHTCTRL, "KEY_RIGHTCTRL" },
    { EV_KEY, KEY_RIGHTALT, "KEY_RIGHTALT" }, { EV_KEY, KEY_HOME, "KEY_HOME" },
    { EV_KEY, KEY_UP, "KEY_UP" }, { EV_KEY, KEY_PAGEUP, "KEY_PAGEUP" },
    { EV_KEY, KEY_LEFT, "KEY_LEFT" }, { EV_KEY, KEY_RIGHT, "KEY_RIGHT" },
    { EV_KEY, KEY_END, "KEY_END" }, { EV_KEY, KEY_DOWN, "KEY_DOWN" },
    { EV_KEY, KEY_PAGEDOWN, "KEY_PAGEDOWN" }, { EV_KEY, KEY_INSERT, "KEY_INSERT" },
    { EV_KEY, KEY_DELETE, "KEY_DELETE" },
    { EV_KEY, BTN_LEFT,   "LEFT" },
    { EV_KEY, BTN_RIGHT,  "RIGHT" },
    { EV_KEY, BTN_MIDDLE, "MIDDLE" },
    { EV_REL, REL_X, "X" },
    { EV_REL, REL_Y, "Y" },
    { EV_REL, REL_WHEEL, "WHEEL" },
    { EV_ABS, ABS_X, "ABS_X" },
    { EV_ABS, ABS_Y, "ABS_Y" },
};

const char *binput_code_name(uint16_t type, uint16_t code)
{
    for (size_t i = 0; i < sizeof(code_table) / sizeof(code_table[0]); i++) {
        if (code_table[i].type == type && code_table[i].code == code)
            return code_table[i].name;
    }
    static char tmp[32];
    snprintf(tmp, sizeof(tmp), "%s_%u", type == EV_KEY ? "KEY" :
             type == EV_REL ? "REL" : type == EV_ABS ? "ABS" : "EV", code);
    return tmp;
}