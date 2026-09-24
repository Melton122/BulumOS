/*
 * bapp_about.c - About BulumOS app (Phase 10).
 *
 * A read-only text view of version + system identity.  Uses bsys for
 * kernel/hostname/arch and static constants for the release line.  No shell
 * commands are invoked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define BULUMOS_VERSION "10.0"

enum { W_BODY = 1 };

static void rebuild(appui_t *ui)
{
    char k[64] = "unknown", hn[64] = "unknown";
    if (bsys_kernel(k, sizeof(k)) != 0) snprintf(k, sizeof(k), "unknown");
    if (bsys_hostname(hn, sizeof(hn)) != 0) snprintf(hn, sizeof(hn), "unknown");

    char buf[BAPPUI_MAX_TEXT];
    /* compose a bounded multi-line text into the textarea */
    snprintf(buf, sizeof(buf),
             "BulumOS %s\n"
             "\n"
             "Native desktop (Phase 10)\n"
             "Kernel  : %s\n"
             "Host    : %s\n"
             "Arch    : %s\n"
             "Apps    : appui framework, in-process\n"
             "\n"
             "BulumOS is a lightweight, native,\n"
             "dependency-free operating system.",
             BULUMOS_VERSION, k, hn, bsys_arch());
    appui_set_text(ui, W_BODY, buf);
    appui_reparse_line_count(ui, W_BODY, buf);
}

int bapp_about_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "About BulumOS", 320, 160, 460, 320, 360, 240,
                         800, 600))
        return -1;
    ui.extra = NULL;             /* nothing dynamic to free */
    ui.on_event = NULL;

    appui_add(&ui, appui_textarea(W_BODY, 14, 12, 460 - 28, 320 - 34));
    rebuild(&ui);
    appui_set_active(&ui);
    return ui.win;
}
