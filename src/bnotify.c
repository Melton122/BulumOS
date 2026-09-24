/*
 * bnotify.c - BulumOS notifications (Phase 9.16)
 */

#include "bnotify.h"

#include <stdio.h>
#include <string.h>

static bnotify_t g_ring[BNOTIFY_CAP];
static int g_cap = BNOTIFY_CAP;
static unsigned long g_head;              /* insertion index (mod cap) */
static uint32_t g_next_id = 1;
static uint32_t g_now = 0;

void bnotify_init(uint32_t *monotonic_ms, int cap)
{
    memset(g_ring, 0, sizeof(g_ring));
    g_cap = (cap > 0 && cap <= BNOTIFY_CAP) ? cap : BNOTIFY_CAP;
    g_head = 0;
    g_next_id = 1;
    g_now = monotonic_ms ? *monotonic_ms : 0;
}

uint32_t bnotify_send(const char *title, const char *msg,
                      const char *icon, int priority, int lifetime_ms)
{
    if (g_cap <= 0)
        return 0;
    bnotify_t *n = &g_ring[g_head % (unsigned)g_cap];
    g_head++;
    n->id = g_next_id++;
    n->priority = (priority < 0) ? 0 : (priority > 3 ? 3 : priority);
    n->lifetime_ms = (lifetime_ms < 0) ? 0 : lifetime_ms;
    n->created_ms = (int)g_now;
    n->visible = 1;
    snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
    snprintf(n->msg,   sizeof(n->msg),   "%s", msg ? msg : "");
    snprintf(n->icon,  sizeof(n->icon),  "%s", icon ? icon : "");
    return n->id;
}

void bnotify_clear(uint32_t id)
{
    for (int i = 0; i < g_cap; i++) {
        if (g_ring[i].id && (id == 0 || g_ring[i].id == id))
            g_ring[i].id = 0;
    }
}

int bnotify_list(bnotify_t *out, int max)
{
    int count = 0;
    if (!out || max < 0)
        max = 0;
    for (int i = 0; i < g_cap && count < max; i++)
        if (g_ring[i].id) {
            out[count++] = g_ring[i];
        }
    return count;
}

void bnotify_tick(int now_ms)
{
    g_now = (uint32_t)now_ms;
    for (int i = 0; i < g_cap; i++) {
        bnotify_t *n = &g_ring[i];
        if (n->id && n->lifetime_ms > 0 &&
            (int)g_now - n->created_ms >= n->lifetime_ms)
            n->id = 0;
    }
}