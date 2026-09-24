/*
 * bdesktop - BulumOS desktop shell launcher (Phase 9)
 *
 * Initialises the background services (graphics + input), loads the theme,
 * registers the application set, creates the compositor (bwm) and desktop
 * shell (bshell), opens a welcome window, and runs the single-threaded event
 * loop that feeds bgfx/binput events into the window manager and composes
 * each frame.
 *
 *   bdesktop --backend X     auto|framebuffer|drm (default: from config)
 *   bdesktop --theme NAME    dark|light (default: from config)
 *   bdesktop --frames N      run N frames then exit (self-test)
 *   bdesktop --help          usage
 *   bdesktop --version       version
 *
 * On a machine without graphics the desktop prints "Graphics: unavailable"
 * and exits 1.
 */

#include "bgfx.h"
#include "binput.h"
#include "btheme.h"
#include "bapp.h"
#include "bwm.h"
#include "bshell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>

#define BDESKTOP_VERSION "1.0.0"
#define GRAPHICS_CONF "/etc/bulum/graphics.conf"

static unsigned long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000UL);
}

static int read_conf_backend(char *out, size_t n)
{
    FILE *f = fopen(GRAPHICS_CONF, "r");
    if (!f)
        return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "backend=%31s", out) == 1) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static void usage(const char *p)
{
    printf("usage: %s [--backend X] [--theme NAME] [--frames N] [--help] [--version]\n", p);
}

int main(int argc, char **argv)
{
    const char *backend = NULL;
    const char *theme_name = NULL;
    long frames = -1;         /* -1 = run forever */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            printf("bdesktop %s (BulumOS Phase 10)\n", BDESKTOP_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            backend = argv[++i];
        } else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            theme_name = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = atol(argv[++i]);
            if (frames < 0)
                frames = 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    /* backend resolution: explicit > config > auto */
    char cfg[32] = "auto";
    if (!backend && read_conf_backend(cfg, sizeof(cfg)) == 0)
        backend = cfg;
    if (!backend)
        backend = "auto";

    if (bgfx_init(backend) < 0) {
        printf("Graphics: unavailable (%s)\n",
               bgfx_info() ? bgfx_info()->error : "init failed");
        return 1;
    }
    int nin = binput_init();
    printf("Graphics: %dx%d (%s), input: %d device(s)\n",
           bgfx_get_width(), bgfx_get_height(),
           bgfx_info()->format == BGFX_FMT_XRGB8888 ? "XRGB8888" :
           bgfx_info()->format == BGFX_FMT_BGRX8888 ? "BGRX8888" :
           bgfx_info()->format == BGFX_FMT_RGB888 ? "RGB888" :
           bgfx_info()->format == BGFX_FMT_RGB565 ? "RGB565" : "?",
           nin);
    fflush(stdout);

    /* theme:  --theme NAME > desktop.conf (theme=) + theme.conf overrides */
    btheme_t theme;
    if (theme_name && !strcasecmp(theme_name, "light")) {
        btheme_default_light(&theme);
    } else if (theme_name && !strcasecmp(theme_name, "dark")) {
        btheme_default_dark(&theme);
    } else if (theme_name != NULL) {
        usage(argv[0]);
        bgfx_shutdown();
        binput_shutdown();
        return 2;
    } else {
        /* btheme_load reads desktop.conf `theme=` (dark/light) then applies
         * any explicit colours from /etc/bulum/theme.conf */
        btheme_load(&theme, BTHEME_CONF_PATH);
    }

    /* applications: prefer .bapp files on disk; else built-in defaults */
    if (bapp_rescan(BAPP_APPS_DIR) == 0)
        bapp_register_defaults();

    /* compositor + shell */
    if (bwm_init(&theme, BSHELL_PANEL_H) < 0) {
        printf("Desktop: compositor init failed\n");
        bgfx_shutdown();
        binput_shutdown();
        return 1;
    }

    bshell_t shell;
    bshell_init(&shell, &theme);
    bshell_register(&shell);

    /* welcome "About BulumOS" window (built-in demo).  Closing it quits the
     * desktop gracefully - a clean shutdown path for interactive use. */
    static bshell_textapp_t about = {
        .title = "About BulumOS",
    };
    snprintf(about.lines[0], sizeof(about.lines[0]),
             "Welcome to BulumOS Phase 10.");
    snprintf(about.lines[1], sizeof(about.lines[1]),
             "This native desktop is a C compositor built on bgfx + binput.");
    snprintf(about.lines[2], sizeof(about.lines[2]),
             "Use the panel launcher (top left) to open apps.");
    snprintf(about.lines[3], sizeof(about.lines[3]),
             "Drag title bars to move; grip resizes; buttons close/shrink.");
    about.nlines = 4;
    int welcome_win = bwm_window_create("About BulumOS", 120, 90, 460, 200,
                                        220, 140, 900, 600,
                                        bshell_draw_textapp, &about);

    /* --- event loop --- */
    unsigned long last_clock = mono_ms();
    int rendered_once = 0;
    long frame_count = 0;
    int want_quit = 0;

    binput_event_t ev;
    while (!want_quit) {
        unsigned long now = mono_ms();
        if (now - last_clock >= 100) {
            bshell_tick(&shell, (int)now);
            bwm_frame();
            rendered_once = 1;
            frame_count++;
            last_clock = now;
            if (frames >= 0 && frame_count >= frames)
                break;
            /* the desktop stays alive while the welcome window is open */
            if (welcome_win > 0 && bwm_window_get(welcome_win) == NULL)
                want_quit = 1;
        }

        int r = binput_poll(30);
        if (r < 0)
            break;      /* input error: stop gracefully */
        if (r == 0)
            continue;

        while (binput_read(&ev)) {
            /* quit on Ctrl+Q (KEY_LEFTCTRL held while KEY_Q pressed) -
             * handled by bwm dispatch returning a dedicated code below.
             * Here we keep it simple: any key on the welcome window's
             * "Close" action is handled by the compositor. */
            int hit = bwm_handle_event(&ev);
            if (hit == BWM_HIT_PANEL || hit == BWM_HIT_LAUNCHER)
                bshell_pointer(&shell, &ev);
            if (ev.type == EV_KEY && shell.launcher_open)
                bshell_key(&shell, &ev);
        }
        /* always repaint after input so drags/menus track the pointer */
        if (rendered_once)
            bwm_frame();
    }

    bwm_shutdown();
    binput_shutdown();
    bgfx_shutdown();
    printf("BDESKTOP-DONE frames=%ld\n", frame_count);
    return 0;
}
