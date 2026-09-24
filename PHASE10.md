# BulumOS — Phase 10: Application Ecosystem

Phase 10 turns the Phase 9 in-process native desktop (bwm compositor + bshell +
bdesktop daemon) into a real application ecosystem: native GUI apps, a
lightweight `bappui` widget framework, `.bapp` metadata, launcher
discovery/search, preferences, clipboard, file associations, notifications, and
error dialogs.  Phase 10 apps are **in-process modules** hosted by `bdesktop`,
extending the `internal:*` builtin pattern.  Cross-process windowing is
deferred to Phase 11.  The app layer never uses `system()`/`popen()`.

## Staging summary

| Stage | Work | Status |
|---|---|---|
| 1 | Audit codebase + all CLI tool interfaces (bctl, bulum/bpk, blog, bnet, bsecure, bstatus, buser, bfirewall) | done |
| 2 | `bappui` widget toolkit (`src/bappui.[ch]`) | done |
| 3 | `.bapp` metadata: `version`/`desc`; `bapp_rescan`; `register_one`; 11 default apps with categories | done |
| 4+ | `bsys` helpers: `bsys_exec`, ANSI strip, /proc readers, preferences, clipboard | done |
| 5 | `bapps` dispatcher + dialog + file associations | done |
| 6-16 | 11 app modules (`src/bapp_*.c`) all compile clean | done |
| 15/17 | Launcher keyboard search + filtering in `bshell` | done |
| 27 | Build integration: link bappui/bsys/bapps + apps; write `.bapp` + `preferences.conf` + `associations.conf` into rootfs | done |
| 28 | Host tests: `tools/test-apps.sh` (13/13), `tools/test-bdesktop.sh` (12/12), `tools/chk-target-bdesktop.sh` | done |
| 26 | Security audit: no system/popen, no unbounded strcpy/strcat/sprintf | done |
| 32 | Docs: this file + BULUMOS.md section | done |

## Framework: bappui

- `appui_win_create` memsets the `appui_t` and creates a `bwindow`; `appui_win_close`
  does `free(ui->extra)`.  Apps must heap-allocate their state in `ui->extra`
  and set `ui->on_event` **after** create.
- `appui_add(ui, appui_widget_t w)` takes the widget **by value**:
  `appui_add(&ui, appui_label(...))`.
- Lists: `widget->selected`/`rowfn` row args are **visible** rows; map to the
  session index via `widget->first + row`.  Keyboard: KEY_UP/DOWN fire SELECT
  (scrolling), KEY_ENTER fires ACTIVATE; input fires ACTIVATE on Enter.
- Widgets: label, button, list, input, check, textarea, separator.
  Accessors: `appui_widget(ui, id)`, `appui_set_text`, `appui_set_active`.

## Applications

| App | Module | Notes |
|---|---|---|
| Files | `bapp_files.c` | libc dir ops, sorted list, Up/Home/New/Rename/Delete/Info/Refresh/**Copy Name**, delete confirm, open-by-association, clipboard |
| Settings | `bapp_settings.c` | theme/clock/panel/icons/launcher-autoclose via `bpref_*` |
| Packages | `bapp_packages.c` | `bsys_exec` on `/usr/bin/bulum`, ANSI stripped (no internal system/popen path) |
| System Monitor | `bapp_monitor.c` | CPU/mem/uptime/process table |
| Network | `bapp_network.c` | `/proc/net/dev` + `/etc/resolv.conf` |
| Security | `bapp_security.c` | stat-based audit (never reads `/etc/shadow`/hashes) |
| Services | `bapp_services.c` | `bsys_exec` on `/usr/bin/bctl` |
| Logs | `bapp_logs.c` | bounded tails of `/var/log/bulum/*` |
| Users | `bapp_users.c` | `/etc/passwd` parse |
| About | `bapp_about.c` | version/kernel/host/arch textarea |
| Terminal | `bapp_terminal.c` | bounded command console (no PTY; Phase 11) |

## Helpers (bsys)

- `bsys_exec`: exe must start with `/` and be directly under a `ROOTS[]` root
  (`/usr/bin/`, `/bin/`, `/sbin/`), no subdirectory; bounded pipe read then
  `waitpid` (no deadlock/zombie).
- `bsys_strip_ansi`, `bsys_trim`, `/proc` readers, `bpref_*`, and the
  clipboard (`bsys_clip_set/get/has`, `BSYS_CLIP_MAX 4096`).

## Associations / launch

- `bapps_open("internal:name")` dispatches to a module.
- `bapps_open_for_path(path)` reads `/etc/bulum/associations.conf`
  (`.ext=app` or `.ext=internal:x`); the path is overridable via `BULUM_ASSOC`
  for host tests.
- `bshell_launch` dispatches builtins through `bapps_open`; launcher keyboard
  search filters by name/category (`bshell_key`).

## Build / deploy

`rebuild-userspace.sh`'s Phase 10 section links bdesktop with bappui/bsys/bapps
+ all 11 modules and writes `rootfs/usr/share/applications/*.bapp`,
`/etc/bulum/preferences.conf`, and `/etc/bulum/associations.conf` when absent.

## QEMU aarch64 VM verification (desktop-as-service)

Two service-manager bugs were found and fixed while booting the desktop as a
`bctl` service in the QEMU aarch64 VM (`tools/install-services.sh`):

- **Graphics gate used a clobbered state file.** The graphics service is
  ONESHOT, so `bctl` overwrites `/var/run/bulum/graphics.state` with
  `DONE rc=0` right after the service writes `available`; the desktop control's
  `grep '^available$'` therefore always failed and the desktop was permanently
  `deferred` (bdesktop never launched, yet `bctl start` returned 0 and spawned
  a respawn supervisor).  Fix: the desktop control probes graphics live with
  `/usr/bin/bgfx status` instead of trusting the state file.
- **Daemon never matched its service name.** Desktop `meta` had no `PIDFILE`,
  so `bctl`'s `find_pid` scanned `/proc/<pid>/comm` and compared it to the
  service name `desktop`; the daemon's comm is `bdesktop`, so the service was
  always reported `[STOPPED]` and the supervisor respawned bdesktops forever.
  Fix: `mk desktop ... /var/run/bulum/desktop.pid` (the control already writes
  that pidfile), mirroring the dropbear service.  Also replaced the stop
  control's `pkill -F` (unsupported on busybox v1.37.0) with
  `kill "$(cat $PIDFILE)"` + `killall bdesktop`.

Verified boot in QEMU (autotest hook in `src/init.c` runs `/etc/bulum/
autotest.sh` if present, printing to the serial console): graphics `[ OK ]`,
desktop `[ OK ]` at boot, `boot complete: 13/13 services`, a **single**
supervised `bdesktop` reporting `[RUNNING] yes` with `Graphics: 1280x800
(XRGB8888)` rendered, clean stop/start with no respawn pile-up.

## x86-64 support

Phase 10 also boots and runs the same desktop on x86-64 QEMU (OVMF/EFI).  All
smoke-tested in the x86 VM:

- **Kernel**: arm64 `.config` converted with `make ARCH=x86_64 olddefconfig`
  (EFI_STUB, VIRTIO_PCI, DRM_VIRTIO_GPU+KMS, VIRTIO_BLK/NET, SERIAL_8250_
  CONSOLE, DEVTMPFS_MOUNT, BLK_DEV_INITRD, FB_EFI kept); `bzImage` built and
  saved as `kernel/.config.x86_64`.
- **Userspace**: `rootfs-x86/` clone of the rootfs with native-gcc binaries;
  **bctl/init rebuilt --static for x86** and the dropbear family swapped to x86
  host binaries + the matching glibc set (`/lib/libc.so.6`, `libtomcrypt`,
  `libtommath`, `libgmp`, `libcrypt`, `libz`, `/lib64/ld-linux-x86-64.so.2`).
- **Boot**: `bulumos-uefi-x86.img` (OVMF 4M, grub x86_64-efi memdisk) boots the
  disk root (`disk-x86.img`); `boot-gui-x86.log` shows `graphics [ OK ]`,
  `desktop [ OK ]`, **`boot complete: 13/13 services`**, and an SSH listener
  via `dropbear` (hostfwd `localhost:2223`).
- **Console rule**: last `console=` wins for `/dev/console`; `tty0` last sends
  userspace to the framebuffer console instead of serial, so grub.cfg uses only
  `console=ttyS0,115200n8` so the serial log captures the boot.
- Build/run scripts: `tools/build-x86-userspace.sh`, `build-uefi-x86.sh`,
  `tools/run-desktop-x86.sh` (GUI), `tools/screenshot-x86.sh` (headless probe).
- Known limits: graphical screendump requires a live GTK display (works from a
  user session; `-display none`/VNC/egl-headless don't create a scanout
  surface), so the x86 screenshot is captured against the user-launched GUI VM
  the same way as aarch64.  TCG boot can rarely hang during early ACPI init on
  q35 (a reboot clears it).

## Verification

- `tools/test-apps.sh` (host `bapp_hosttest`): A01 window create/render,
  A02 all 11 apps open, A03 dialog+guard, A04 ANSI strip, A05 prefs
  round-trip, A06 /proc readers, A07 exec validation, A08 event path,
  A09 clipboard, A10 file association, A11 no-association safety —
  **13/13 PASS**.
- `tools/test-bdesktop.sh`: composite shell regression (now also links the
  app layer) — **12/12 PASS**.
- `tools/chk-target-bdesktop.sh`: full aarch64 static link —
  **TARGET-PHASE10-BUILD-OK**.
- `tools/chk-apps.sh`: per-file host compile check — all modules `ok`.
- Security audit (`tools/audit-phase10.sh`): no `system`/`popen`, no
  unbounded `strcpy`/`strcat`/`sprintf`, no `gets`/`alloca`; `fork`/`execve`/
  `pipe` only in `bsys.c`.
