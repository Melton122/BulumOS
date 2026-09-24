/*
 * bapp_files.c - Bulum Files: a safe, in-process file manager app.
 *
 * Navigation and mutation use plain libc (opendir/readdir/stat,
 * open/mkdir/rename/remove) - never a shell.  Destructive ops (delete) are
 * confirmed through the shared dialog.  Entries are sorted with a type marker
 * and the selected entry's metadata is shown in the status line.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bapps.h"
#include "bappui.h"
#include "bsys.h"

#define BFS_PATH  512
#define BFS_MAX   256
#define BFS_NAME  128
#define BFS_INFO  200

enum { W_LIST = 1, W_PATH, W_NAME, W_STATUS,
       W_UP, W_HOME, W_NEWFILE, W_NEWDIR, W_RENAME, W_DELETE, W_INFO,
       W_REFRESH, W_COPY };

typedef struct {
    char   cwd[BFS_PATH];
    int    nentries;
    char   names[BFS_MAX][BFS_NAME];
    char   types[BFS_MAX];      /* 'd' dir, 'f' file, 'l' symlink, '?' */
    char   inf_msg[BFS_INFO];
    int    mode;                /* 0 none, 1 newfile, 2 newdir, 3 rename */
} bfs_state_t;

static appui_t *g_ui;

static void set_status(appui_t *ui, const char *s)
{
    bfs_state_t *st = (bfs_state_t *)ui->extra;
    if (!st) return;
    snprintf(st->inf_msg, sizeof(st->inf_msg), "%s", s);
    appui_set_text(ui, W_STATUS, s);
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void rescan(bfs_state_t *st)
{
    st->nentries = 0;
    st->names[0][0] = '\0';
    DIR *d = opendir(st->cwd);
    if (!d) {
        snprintf(st->inf_msg, sizeof(st->inf_msg), "Cannot open: %s", st->cwd);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && st->nentries < BFS_MAX) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        snprintf(st->names[st->nentries], BFS_NAME, "%s", nm);
        char full[BFS_PATH];
        snprintf(full, sizeof(full), "%s/%s", st->cwd, nm);
        struct stat sb;
        char ty = '?';
        if (lstat(full, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) ty = 'd';
            else if (S_ISLNK(sb.st_mode)) ty = 'l';
            else if (S_ISREG(sb.st_mode)) ty = 'f';
        }
        st->types[st->nentries] = ty;
        st->nentries++;
    }
    closedir(d);
    if (st->nentries > 1)
        qsort(st->names, (size_t)st->nentries, BFS_NAME, cmp_names);
}

static void entry_path(const bfs_state_t *st, int idx, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/%s", st->cwd, st->names[idx]);
}

static void reload(appui_t *ui, bfs_state_t *st)
{
    rescan(st);
    appui_set_text(ui, W_PATH, st->cwd);
    appui_set_total(ui, W_LIST, st->nentries);
    appui_scroll_top(ui, W_LIST);
}

/* ---- shared helpers ----------------------------------------------------- */
static int parent_of(const char *path, char *out, size_t outsz)
{
    const char *last = strrchr(path, '/');
    if (!last)
        return -1;
    if (last == path) {
        if (strcmp(path, "/") != 0) { snprintf(out, outsz, "/"); return 0; }
        return -1;
    }
    size_t n = (size_t)(last - path);
    if (n == 0) n = 1;
    if (n >= outsz) return -1;
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}

static int name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) >= BFS_NAME)
        return 0;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\')
            return 0;
    return 1;
}

/* ---- dialog callback for delete (runs after dialog window is closed) ---- */
static void delete_cb(void *user, int ok)
{
    (void)user;
    if (!ok || !g_ui)
        return;
    bfs_state_t *st = (bfs_state_t *)g_ui->extra;
    if (!st)
        return;
    appui_widget_t *w = appui_widget(g_ui, W_LIST);
    if (!w || st->nentries == 0)
        return;
    int idx = w->first + w->selected;
    if (idx < 0 || idx >= st->nentries)
        return;
    char full[BFS_PATH];
    entry_path(st, idx, full, sizeof(full));
    int rc = (st->types[idx] == 'd') ? rmdir(full) : remove(full);
    if (rc == 0) {
        reload(g_ui, st);
        set_status(g_ui, "Deleted.");
    } else {
        char e[BFS_INFO];
        snprintf(e, sizeof(e), "Delete failed: %s", strerror(errno));
        set_status(g_ui, e);
    }
    appui_set_active(g_ui);
}

/* ---- list row provider -------------------------------------------------- */
static int rowfn(appui_t *ui, int wid, int row)
{
    bfs_state_t *st = (bfs_state_t *)ui->extra;
    appui_widget_t *w = appui_widget(ui, wid);
    if (!st || !w)
        return -1;
    int idx = w->first + row;
    if (idx < 0 || idx >= st->nentries)
        return -1;
    char tmp[BAPPUI_MAX_TEXT];
    const char *tm = st->types[idx] == 'd' ? " [dir]"
                   : st->types[idx] == 'l' ? " [link]"
                   : st->types[idx] == 'f' ? ""
                   : " [?]";
    snprintf(tmp, sizeof(tmp), "%s%s", st->names[idx], tm);
    appui_set_rowtext(ui, wid, tmp);
    return 1;
}

/* ---- file mutation (create / rename) committed on the input field ------- */
static void commit_name(appui_t *ui, bfs_state_t *st)
{
    const char *name = appui_get_text(ui, W_NAME);
    if (!name_ok(name)) {
        st->mode = 0;
        set_status(ui, "Invalid name.");
        return;
    }
    char full[BFS_PATH];
    snprintf(full, sizeof(full), "%s/%s", st->cwd, name);
    int rc = 0;
    if (st->mode == 1) {
        int fd = open(full, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) close(fd); else rc = errno;
    } else if (st->mode == 2) {
        rc = mkdir(full, 0755);
    } else if (st->mode == 3) {
        appui_widget_t *w = appui_widget(ui, W_LIST);
        int oi = (w && st->nentries > 0) ? w->first + w->selected : -1;
        if (oi >= 0 && oi < st->nentries) {
            char old[BFS_PATH];
            entry_path(st, oi, old, sizeof(old));
            rc = rename(old, full);
        }
    }
    st->mode = 0;
    if (rc == 0) {
        reload(ui, st);
        set_status(ui, "Done.");
    } else {
        char e[BFS_INFO];
        snprintf(e, sizeof(e), "Operation failed: %s", strerror(rc));
        set_status(ui, e);
    }
}

/* ---- event dispatch ----------------------------------------------------- */
static void on_event(appui_t *ui, int evt)
{
    bfs_state_t *st = (bfs_state_t *)ui->extra;
    if (!st)
        return;
    int wid = ui->last_widget;

    /* INPUT Enter commits a new/rename name */
    if (wid == W_NAME && evt == APPUI_EVT_ACTIVATE && st->mode != 0) {
        commit_name(ui, st);
        return;
    }
    if (evt != APPUI_EVT_ACTIVATE)
        return;

    switch (wid) {
    case W_LIST: {
        appui_widget_t *w = appui_widget(ui, W_LIST);
        if (w && st->nentries > 0) {
            int idx = w->first + w->selected;
            if (idx >= 0 && idx < st->nentries) {
                if (st->types[idx] == 'd') {
                    char full[BFS_PATH];
                    entry_path(st, idx, full, sizeof(full));
                    snprintf(st->cwd, sizeof(st->cwd), "%s", full);
                    reload(ui, st);
                } else {
                    char full[BFS_PATH];
                    entry_path(st, idx, full, sizeof(full));
                    int aid = bapps_open_for_path(full);
                    if (aid > 0) {
                        char m[BFS_INFO];
                        snprintf(m, sizeof(m), "Opened with associated app.");
                        set_status(ui, m);
                    } else {
                        struct stat sb;
                        if (stat(full, &sb) == 0) {
                            char m[BFS_INFO];
                            snprintf(m, sizeof(m), "%s  size=%ld bytes (no association)",
                                     st->names[idx], (long)sb.st_size);
                            set_status(ui, m);
                        }
                    }
                }
            }
        }
        break;
    }
    case W_NEWFILE:
        st->mode = 1;
        appui_set_text(ui, W_NAME, "newfile.txt");
        set_status(ui, "Enter a new file name, then press Enter.");
        break;
    case W_NEWDIR:
        st->mode = 2;
        appui_set_text(ui, W_NAME, "newfolder");
        set_status(ui, "Enter a new folder name, then press Enter.");
        break;
    case W_RENAME: {
        appui_widget_t *w = appui_widget(ui, W_LIST);
        if (w && st->nentries > 0) {
            int idx = w->first + w->selected;
            if (idx >= 0 && idx < st->nentries) {
                st->mode = 3;
                appui_set_text(ui, W_NAME, st->names[idx]);
                set_status(ui, "Enter the new name, then press Enter.");
            }
        }
        break;
    }
    case W_DELETE:
        if (st->nentries == 0)
            break;
        {
            appui_widget_t *w = appui_widget(ui, W_LIST);
            int idx = (w && st->nentries > 0) ? w->first + w->selected : -1;
            if (idx >= 0 && idx < st->nentries) {
                char msg[BFS_INFO];
                snprintf(msg, sizeof(msg), "Delete %s '%s'?",
                         st->types[idx] == 'd' ? "folder" : "file",
                         st->names[idx]);
                g_ui = ui;
                bapp_dialog("Confirm Delete", msg, "Delete", "Cancel",
                            delete_cb, NULL);
            }
        }
        break;
    case W_UP: {
        char p[BFS_PATH];
        if (parent_of(st->cwd, p, sizeof(p)) == 0) {
            snprintf(st->cwd, sizeof(st->cwd), "%s", p);
            reload(ui, st);
        }
        st->mode = 0;
        break;
    }
    case W_HOME:
        snprintf(st->cwd, sizeof(st->cwd), "/");
        reload(ui, st);
        st->mode = 0;
        break;
    case W_REFRESH:
        reload(ui, st);
        set_status(ui, "Refreshed.");
        st->mode = 0;
        break;
    case W_INFO: {
        appui_widget_t *w = appui_widget(ui, W_LIST);
        if (w && st->nentries > 0) {
            int idx = w->first + w->selected;
            if (idx >= 0 && idx < st->nentries) {
                struct stat sb; char full[BFS_PATH];
                entry_path(st, idx, full, sizeof(full));
                if (stat(full, &sb) == 0) {
                    char m[BFS_INFO];
                    snprintf(m, sizeof(m), "%s  mode=%o  size=%ld bytes",
                             st->names[idx], (unsigned)sb.st_mode & 07777,
                             (long)sb.st_size);
                    set_status(ui, m);
                }
            }
        }
        break;
    }
    case W_COPY: {
        appui_widget_t *w = appui_widget(ui, W_LIST);
        if (w && st->nentries > 0) {
            int idx = w->first + w->selected;
            if (idx >= 0 && idx < st->nentries) {
                bsys_clip_set(st->names[idx], (int)strlen(st->names[idx]));
                char m[BFS_INFO];
                snprintf(m, sizeof(m), "Copied '%s' to clipboard.",
                         st->names[idx]);
                set_status(ui, m);
            }
        }
        break;
    }
    }
}

int bapp_files_open(void)
{
    static appui_t ui;
    if (appui_win_create(&ui, "Bulum Files", 120, 70, 560, 420, 360, 240,
                         900, 600))
        return -1;
    bfs_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        appui_win_close(&ui);
        return -1;
    }
    snprintf(st->cwd, sizeof(st->cwd), "/");
    rescan(st);
    ui.extra = st;
    ui.on_event = on_event;
    g_ui = &ui;

    int W = 560;
    appui_add(&ui, appui_label(W_PATH, 10, 8, W - 20, st->cwd));
    appui_add(&ui, appui_list(W_LIST, 10, 30, W - 20, 255, APPUI_ROW_H));
    appui_add(&ui, appui_input(W_NAME, 10, 293, W - 20, ""));
    appui_add(&ui, appui_label(W_STATUS, 10, 322, W - 20, ""));
    appui_add(&ui, appui_button(W_UP, 10, 350, 70, "Up"));
    appui_add(&ui, appui_button(W_HOME, 88, 350, 70, "Home"));
    appui_add(&ui, appui_button(W_NEWFILE, 166, 350, 80, "New File"));
    appui_add(&ui, appui_button(W_NEWDIR, 254, 350, 82, "New Folder"));
    appui_add(&ui, appui_button(W_RENAME, 344, 350, 74, "Rename"));
    appui_add(&ui, appui_button(W_DELETE, 426, 350, 62, "Delete"));
    appui_add(&ui, appui_button(W_REFRESH, 496, 350, 54, "Refresh"));
    appui_add(&ui, appui_button(W_INFO, 10, 380, 70, "Info"));
    appui_add(&ui, appui_button(W_COPY, 88, 380, 90, "Copy Name"));

    appui_widget_t *lst = appui_widget(&ui, W_LIST);
    if (lst) {
        lst->rowfn = rowfn;
        lst->total = st->nentries;
    }
    if (st->nentries == 0)
        set_status(&ui, "Empty directory.");
    appui_set_active(&ui);
    return ui.win;
}
