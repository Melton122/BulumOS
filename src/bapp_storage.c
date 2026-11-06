#include "bapps.h"
#include "bappui.h"
#include "bstorage.h"
#include "bstorage_cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_TEXT_MAX 8192
enum { STORAGE_BODY = 1, STORAGE_REFRESH };

typedef struct { char text[STORAGE_TEXT_MAX]; } storage_state_t;

static void append(storage_state_t *state, const char *format, ...)
{
    size_t used = strlen(state->text);
    if (used >= sizeof(state->text) - 1)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(state->text + used, sizeof(state->text) - used, format, args);
    va_end(args);
}

static void refresh(appui_t *ui)
{
    storage_state_t *state = ui->extra;
    if (!state)
        return;
    state->text[0] = '\0';
    bstorage_scan();
    append(state, "Bulum Storage\n\n%llu disk(s), %llu partition(s), "
           "%llu mounted filesystem(s)\n\n",
           (unsigned long long)bstorage_disk_count(),
           (unsigned long long)bstorage_partition_count(),
           (unsigned long long)bstorage_mount_count());
    for (size_t i = 0; i < bstorage_disk_count(); i++) {
        BStorageDevice disk;
        char size[32];
        if (bstorage_get_device(i, &disk) != 0 || disk.is_partition)
            continue;
        bstorage_human_bytes(disk.size_bytes, size, sizeof(size));
        append(state, "%s  %s%s%s\n", disk.path, size,
               disk.removable ? "  removable" : "",
               disk.read_only ? "  read-only" : "");
        for (size_t p = 0; p < bstorage_device_count(); p++) {
            BStorageDevice part;
            if (bstorage_get_device(p, &part) != 0 || !part.is_partition ||
                strcmp(part.parent, disk.name) != 0)
                continue;
            bstorage_human_bytes(part.size_bytes, size, sizeof(size));
            append(state, "  |- %s  %s  %s%s\n", part.path,
                   part.filesystem[0] ? part.filesystem : "unknown", size,
                   part.mountpoint[0] ? "" : "  not mounted");
            if (part.mountpoint[0]) {
                BStorageUsage usage;
                BStorageMount mnt;
                if (bstorage_usage(part.mountpoint, &usage) == 0) {
                    char used[32], available[32];
                    bstorage_human_bytes(usage.used_bytes, used, sizeof(used));
                    bstorage_human_bytes(usage.available_bytes, available,
                                         sizeof(available));
                    bool ro = (bstorage_find_mount(part.mountpoint, &mnt) == 0) ? mnt.read_only : false;
                    append(state, "      mounted at %s; %s used, %s available%s\n",
                           part.mountpoint, used, available,
                           ro ? "; read-only" : "");
                } else {
                    bool ro = (bstorage_find_mount(part.mountpoint, &mnt) == 0) ? mnt.read_only : false;
                    append(state, "      mounted at %s%s\n", part.mountpoint,
                           ro ? "; read-only" : "");
                }
            }
        }
        append(state, "\n");
    }
    if (!bstorage_disk_count())
        append(state, "No block disks detected.\n");
    appui_set_text(ui, STORAGE_BODY, state->text);
    appui_reparse_line_count(ui, STORAGE_BODY, state->text);
}

static void on_event(appui_t *ui, int event)
{
    (void)event;
    if (ui->last_widget == STORAGE_REFRESH)
        refresh(ui);
}

int bapp_storage_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Bulum Storage", 140, 88, 600, 440,
                         440, 300, 960, 680) != 0)
        return -1;
    storage_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        appui_win_close(&ui);
        return -1;
    }
    ui.extra = state;
    ui.on_event = on_event;
    appui_add(&ui, appui_textarea(STORAGE_BODY, 12, 12, 576, 360));
    appui_add(&ui, appui_button(STORAGE_REFRESH, 500, 390, 88, "Refresh"));
    refresh(&ui);
    appui_set_active(&ui);
    return ui.win;
}