/*
 * bnotify.h - BulumOS notifications (Phase 9.16)
 *
 * A small, strictly bounded notification ring.  Memory never grows: the
 * buffer is fixed at compile time and `bnotify_send` overwrites the oldest
 * entry when full.  There is no way for callers to cause unbounded growth.
 */

#ifndef BNOTIFY_H
#define BNOTIFY_H

#include <stdint.h>

#define BNOTIFY_MAX_TITLE 64
#define BNOTIFY_MAX_MSG   160
#define BNOTIFY_MAX_ICON  24
#define BNOTIFY_CAP       12        /* bounded queue length */

#define BNOTIFY_PRIO_LOW    0
#define BNOTIFY_PRIO_NORMAL 1
#define BNOTIFY_PRIO_HIGH   2
#define BNOTIFY_PRIO_CRIT   3

typedef struct {
    uint32_t id;             /* monotonic id (nonzero once used)        */
    int      priority;
    int      lifetime_ms;    /* 0 = sticky (kept until cleared)      */
    int      created_ms;     /* monotonic ms at creation              */
    int      visible;        /* currently displayed */
    char     title[BNOTIFY_MAX_TITLE];
    char     msg[BNOTIFY_MAX_MSG];
    char     icon[BNOTIFY_MAX_ICON];
} bnotify_t;

/* Init the ring.  `cap` may be 0 to keep the compile-time cap. */
void   bnotify_init(uint32_t *monotonic_ms, int cap);

/* Send a notification.  Returns the new id, or -1 if the ring is disabled/
 * cap==0. Never blocks and never grows past the cap (oldest is dropped). */
uint32_t bnotify_send(const char *title, const char *msg,
                      const char *icon, int priority, int lifetime_ms);

/* Remove the notification with the given id (0 = clear all). */
void bnotify_clear(uint32_t id);

/* Number of live notifications currently in the ring. */
int  bnotify_list(bnotify_t *out, int max);

/* Advance time; expires notifications whose lifetime elapsed. */
void bnotify_tick(int now_ms);

#endif /* BNOTIFY_H */