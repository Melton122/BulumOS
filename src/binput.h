/*
 * binput.h - BulumOS input subsystem (Phase 8)
 *
 * Thin wrapper over Linux evdev (/dev/input/event*).  Detects
 * keyboards and mice, provides a bounded event queue.
 *
 * Usage:
 *   binput_init()                 open all event devices (nonblocking)
 *   binput_poll(timeout_ms)       wait for events (-1 = forever)
 *   binput_read(&ev)              pop next event from the bounded queue
 *   binput_shutdown()             close everything
 */

#ifndef BINPUT_H
#define BINPUT_H

#include <stdint.h>

#define BINPUT_VERSION "1.0.0"

#define BINPUT_UNKNOWN 0
#define BINPUT_KBD     1
#define BINPUT_MOUSE   2

#define BINPUT_MAX_DEV    8
#define BINPUT_QUEUE_SIZE 64

typedef struct {
    int device;      /* index into the device table */
    int kind;        /* BINPUT_KBD / BINPUT_MOUSE */
    uint16_t type;   /* EV_KEY / EV_REL / EV_ABS / EV_SYN */
    uint16_t code;   /* KEY_*, REL_*, ABS_* */
    int32_t value;
} binput_event_t;

/* returns number of devices opened (0 = no input devices) */
int binput_init(void);
void binput_shutdown(void);

int binput_count(void);
int binput_kind(int dev);
const char *binput_name(int dev);
const char *binput_path(int dev);

/* wait for input; returns 1 when events are queued, 0 on timeout, -1 on error */
int binput_poll(int timeout_ms);

/* pop one queued event; returns 1, or 0 if the queue is empty */
int binput_read(binput_event_t *ev);

/* evdev code -> name, e.g. KEY_A, REL_X, BTN_LEFT */
const char *binput_code_name(uint16_t type, uint16_t code);

#endif /* BINPUT_H */