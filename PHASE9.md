# BulumOS Phase 9 — Desktop Shell & Compositor (final report)

## Summary

Phase 9 turns BulumOS's fbdev graphics + evdev input stack into a native,
dependency-free desktop.  A window manager / compositor (`bwm`) and a
desktop shell (`bshell`) are written in C on top of the existing `bgfx`
and `binput` libraries, driven by a single event-loop daemon (`bdesktop`)
that the service manager starts on graphical boots.  There is no X11, no
Wayland, no GTK/Qt and no large dependency stack: rendering is a software
compositor that double-buffers into an off-screen pixel buffer and presents
in one pass through the existing bgfx backend, so it works regardless of the
framebuffer pixel format.

## Deliverables

| Artifact | Location | Notes |
|---|---|---|
| Window manager / compositor | `src/bwm.c` + `src/bwm.h` | bounded window table, z-order, focus, title-bar chrome, move/resize/minimize/maximize/close, pointer hit-testing + dispatch, off-screen back buffer (double buffering) |
| Desktop shell | `src/bshell.c` + `src/bshell.h` (+ `src/bshell_internal.h`) | system panel, app launcher menu, desktop background, notification popups, clock/tray, built-in in-process apps |
| Desktop daemon | `src/bdesktop.c` | arg parsing, theme/config load, graphics+input init, single-threaded event loop |
| bgfx compositor hooks | `src/bgfx.c` / `src/bgfx.h` | `bgfx_get_bpp`, `bgfx_pack`, `bgfx_blit_from` (single-pass present) |
| Host functional test | `src/bdesk_hosttest.c` + `tools/test-bdesktop.sh` | runs the real compositor/shell headlessly against a software framebuffer stub (G01–G11) |
| Config files | `rootfs/etc/bulum/desktop.conf`, `rootfs/etc/bulum/theme.conf` | desktop + theme settings |
| Service | `tools/install-services.sh` | `desktop` service (deps=graphics, respawn) starting `/usr/bin/bdesktop` |
| Build | `rebuild-userspace.sh` | Phase 9 section compiles `bdesktop` + deps into rootfs |
| VM test | `test-bdesktop-vm.sh` | D1–D10 desktop verification in QEMU |
| Docs | `PHASE9.md`, `BULUMOS.md` | this report + index update |

## Architecture

```
 binput (evdev)        -> feed raw events
      |                       |
      v                       v
  bdesktop (event loop) -> bwm_handle_event()
                              |  hit-test / move / resize / focus / close
                              v
   bwm (compositor) <-------------- bshell (panel, launcher, bg, tray)
      |  compose: bg -> windows -> fg into back buffer
      v
   bgfx_blit_from(back) -> /dev/fb0   (single-pass present)
```

- **Double buffering**: the compositor allocates a back buffer in the exact
  device pixel format (via `bgfx_get_bpp`/`bgfx_get_pitch`/`bgfx_pack`),
  draws the whole desktop into it, then presents with one `bgfx_blit_from`.
  This decouples every drawing helper from the framebuffer format and gives
  the best available anti-tearing behaviour on an fbdev backend that has no
  native page flip.
- **Bounded everything**: window table, icon sizes, notification ring and
  launcher list are all compile-time bounded; geometry is clamped inside the
  work area (screen minus panel) by the `bwindow` model, and every back-buffer
  drawing helper clips to the screen bounds so no draw can write out of
  range.
- **No cross-process IPC**: windows are in-process this phase; the shell
  registers content renderers (`bwm_draw_fn`) and real `.bapp` apps are still
  launched via the existing safe `fork`/`execve` path in `bapp`.

## Configuration

`/etc/bulum/desktop.conf`:

```
theme=dark                  # dark | light
background=solid            # solid | gradient
background_color=#16161e    # used when background=solid
```

`/etc/bulum/theme.conf` (each `name=#rrggbb`, picked up by the existing
`btheme` engine over the chosen base set): `background`, `surface`, `panel`,
`window`, `titlebar`, `titlebar-active`, `text`, `muted`, `accent`,
`border`, `success`, `warning`, `error`.

## Controls

- **Panel (top)**: launcher button (left), taskbar of open windows, status
  tray (clock, notifications, battery/wifi indicators).
- **Launcher**: click the "Apps" button; hover to highlight, click an item to
  open it (built-ins open an in-process window, external `.bapp` apps are
  launched safely).
- **Windows**: drag the title bar to move, drag the bottom-right grip to
  resize, use the `–` / `[]` / `x` buttons to minimise / maximise / close.
- Closing the **About BulumOS** welcome window quits the desktop cleanly
  (an explicit `--frames N` mode is provided for automated self-tests).

## Verification

Host (no framebuffer required — desktop runs against a software stub):
`tools/test-bdesktop.sh` → **G01–G11 pass** (12/12): frame present into the
back buffer, window create, on-screen clamping, min/max resize, focus/z-order,
title-drag move, resize-grip resize, minimize/restore, close, launcher+panel
render, and out-of-bounds drawing safety.

Builds:
- Host: `tools/build-bdesktop-host.sh` → `bdesktop` runs, reports
  `Graphics: unavailable` cleanly without `/dev/fb0`.
- Target: `tools/build-bdesktop-target.sh` (aarch64 cross) → statically
  linked ELF.
- Regression: `tools/test-bgfx.sh` still **17/17 pass** after the `bgfx`
  additions; `rebuild-userspace.sh` Phase 9 section and the `desktop` service
  install verified.

In-VM (needs the full UEFI + disk build): `test-bdesktop-vm.sh` (D1–D10)
boots with virtio-gpu/keyboard/mouse and checks the `desktop` service starts,
`bdesktop --frames N` renders frames and prints `BDESKTOP-DONE`, and config
files load (including the light-theme path).

## Security / robustness notes

- No `system()`/`popen()`; external apps launch only through `bapp`'s
  validated `fork`+`execve`.
- All desktop buffers are bounded; draw helpers clip; a corrupt `theme.conf` /
  `desktop.conf` can never crash the desktop (unknown/malformed lines are
  ignored).
- The `desktop` service only starts when the `graphics` service reports the
  framebuffer is available; otherwise it defers gracefully (honest headless).

## Known limitations

- Windows are in-process; a cross-process windowing protocol is future work.
- No floating accel / touch input; pointer is relative (RE) only.
- The launcher closes on the toggle button or an item click; clicking an
  arbitrary window does not auto-dismiss it.
- Compositing is software (no GPU acceleration); sufficient for the QEMU
  resolution (1280x800) but not for high-refresh compositing.
