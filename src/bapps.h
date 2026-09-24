/*
 * bapps.h - Phase 10 in-process application registry + dispatcher.
 *
 * Each app is a self-contained module exposing `int bapp_<name>_open(void)`
 * which creates and shows its appui window (given BAPPUI a bwm window with
 * draw + event callbacks).  The single entry point `bapps_open(exec)` maps an
 * `internal:<name>` token (from bapp_t.exec) to the right module and hands a
 * "not found" failure back up to the shell so it can notify the user.
 *
 * Apps never fork a compositor, shell or another app; they share the bdesktop
 * process and may spawn whitelisted CLI tools via bsys_exec (fork/execve).
 */

#ifndef BAPPS_H
#define BAPPS_H

/* Dispatcher: exec must be an "internal:name" token.  Returns the appui
 * window id (>0) on success, 0 if the app opened no window, <0 on error. */
int bapps_open(const char *exec);

/* File-association open: match the file path's extension against
 * /etc/bulum/associations.conf and open the bound in-process app.  Returns
 * the same codes as bapps_open, or <0 if no association exists. */
int bapps_open_for_path(const char *path);

/* individual module entry points (implemented by each bapp_*.c) */
int bapp_files_open(void);
int bapp_settings_open(void);
int bapp_packages_open(void);
int bapp_monitor_open(void);
int bapp_hardware_open(void);
int bapp_network_open(void);
int bapp_security_open(void);
int bapp_services_open(void);
int bapp_logs_open(void);
int bapp_users_open(void);
int bapp_terminal_open(void);
int bapp_about_open(void);

/* small shared UI helper: an error/confirm dialog built on appui.
 * Returns 1 on Enter/OK confirm, 0 otherwise.  Blocking is not used; the
 * dialog drives its own window and calls back. */
typedef void (*bdialog_cb)(void *user, int ok);
int bapp_dialog(const char *title, const char *message, const char *ok_label,
                const char *cancel_label, bdialog_cb cb, void *user);

#endif /* BAPPS_H */
