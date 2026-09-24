# BulumOS

Custom aarch64 Linux OS. Own PID 1 (`src/init.c`), busybox userspace, bctl service manager,
UEFI/GRUB boot, persistent ext4 root, bulum package manager — all bootable in QEMU.

Status: **Phase 1–7 complete (core, bctl, bulum, BPK, bterm, bsh, networking+security) + Phase 8 (graphics + input) + Phase 9 (desktop shell + compositor) + Phase 10 (app ecosystem) + Phase 11 (hardware & driver system) + Phase 12 (package ecosystem & repository, aarch64 + x86-64) — BulumOS 1.9**

## Layout

| Path | Role |
|---|---|
| `src/init.c` | binit — PID 1: mounts, hostname, bctl boot/shutdown, console login, zombie reaping |
| `src/bctl.c` | service manager: boot/shutdown/start/stop/restart/status/list/enable/disable/supervise/diagnose/log |
| `src/bstatus.c`, `src/blog.c` | legacy boot status/log readers |
| `src/bulum.c`, `src/bulum_crypto.c` | package manager: update/install/remove/search/info/list/upgrade/clean/verify/owns/doctor/version + `repo add/remove/list` + `--json` |
| `src/bterm.c` | bterm — native console/PTY terminal: PTY shell frontend, ANSI tracker, 10000-line scrollback viewer (Ctrl+A prefix), UTF-8, `-e`, `/etc/bterm.conf` |
| `src/bsh.c` | bsh — native interactive shell (login shell, also used by bterm and recovery) |
| `src/bnet.c` | network tool: status/list/info/up/down/ip/route/dns/dhcp/ping/resolve/sockets/apply |
| `src/bsecure.c` | security audit: `audit --root <dir>` + quick `status` |
| `src/bfirewall.c` | firewall status/enable/disable; honestly reports when no iptables userspace exists |
| `rootfs/etc/bulum/network.conf` | network config: `interface=`, `method=dhcp|static`, `address`, `gateway`, `dns` |
| `rootfs/etc/hosts`, `rootfs/etc/resolv.conf` | static host table + resolver (rewritten by DHCP) |
| `tools/test-bnet.sh`, `tools/test-bsecure.sh` | host-side suites (22 and 10 checks) |
| `test-bnet-vm.sh` | in-VM networking/security verification (21 checks) |
| `rootfs/etc/bterm.conf` | bterm defaults (scrollback, theme, cursor-blink, banner) |
| `tools/test-bterm.sh` | host-side bterm checklist (31 checks, real pty) |
| `test-bterm-vm.sh` | in-VM bterm verification (18 checks) |
| `rootfs/` | system root: busybox, initramfs contents, `/etc/bulum/*` service tree |
| `rootfs/etc/bulum/services/<name>/` | `meta` (NAME/DESC/DEPS/RESPAWN/ONESHOT/PIDFILE) + `control` (start/stop) |
| `rootfs/etc/bulum/enabled/<name>` | marker file = enabled at boot |
| `rootfs/etc/bulum/repos.conf` | repository template (commented): `name url arch key` |
| `rootfs/etc/bulum/keys/<name>.pub` | RSA public key (e, n) for repository signature verification |
| `tools/install-services.sh` | installs the 9 service definitions into rootfs |
| `tools/populate-disk.sh` | builds persistent ext4 `disk.img` from rootfs (debugfs) |
| `tools/make-test-repo.sh` | test repo: signs Release with RSA key, builds gzip debs + Packages |
| `tools/build-package.sh`, `tools/build-official-repo.sh` | Phase 12: build one package / the full official repo (44 entries, x2 arch) at `out/repo` |
| `tools/seed-rootfs-db.sh` | Phase 12: seed `/var/lib/bulum` index (15 entries + no-download cache) into both rootfs trees |
| `tools/test-bpk.sh`, `tools/test-bpk-vm.sh` | Phase 12 host suite (23 checks) + VM e2e for both arches (23 checks each) |
| `build-uefi.sh`, `build-all-uefi.sh` | UEFI + memdisk + hybrid ISO + `bulumos-uefi.img` |
| `rebuild-userspace.sh` | compile init/bctl/bstatus/blog/bulum/bterm/bsh/bnet/bsecure/bfirewall/bgfx*/binput*, initramfs (full + switch), disk.img |
| `src/bgfx.c`, `src/bgfx.h` | graphics library: fbdev frontend, color/line/rect/fill, 8x8 VGA font, text, PNG/LCN decode, framebuffer readback hash |
| `src/bgfxinfo.c`, `src/bgfxdemo.c`, `src/bgfxcli.c` | `bgfxinfo` (fb geometry/format), `bgfxdemo` (`--selfcheck` hashes fb output), `bgfx` (status/info/test/clear/rect/line/fill/text/image/hash/input) |
| `src/binput.c`, `src/binput.h` | input library: evdev scan, keyboard/mouse classification from sysfs capabilities, bounded FIFO, poll+read |
| `src/binputtest.c` | `binputtest`: `--list`, `--timeout N` (seconds) event monitor, keyboard/mouse attribution |
| `rootfs/etc/bulum/graphics.conf` | `mode=auto|fb`, `backend=fb|drm` (drm detection-only, honest "not implemented") |
| `rootfs/etc/mdev.conf` | /dev/fb* 660 root:video, /dev/dri + /dev/input/* 660 root:input |
| `tools/gen-bgfx-font.py` | generates `src/bgfx_font8x8.h` from an 8x8 font bit source |
| `tools/test-bgfx.sh` | host-side bgfx checklist (17 checks, fake framebuffer) |
| `tools/test-binput.sh` | host-side input checklist (13 checks incl. sysfs classification fixtures) |
| `test-bgfx-vm.sh` | in-VM graphics+input suite (G1–G17 graphical + H1–H6 headless) |
| `src/bappui.c`, `src/bappui.h` | Phase 10 native widget toolkit for in-process desktop apps |
| `src/bapps.c`, `src/bapps.h` | app dispatcher (`internal:*`), file associations, shared confirm dialog |
| `src/bapp_*.c` | 12 apps: files, settings, about, monitor, hardware, network, security, services, logs, users, packages, terminal |
| `src/bhw.c`, `src/bhw.h` | Phase 11 hardware API: CPU/mem/PCI/GPU/block/net/USB/audio/power + `/proc/bulum` |
| `src/bhardware.c`, `bdevices.c`, `bdriver.c`, `bnetwork.c`, `bpower.c` | Phase 11 native hardware/device/driver/power CLIs (both arches) |
| `kernel/drivers/bulum/` | Phase 11 in-kernel driver framework (registry + `/proc/bulum/{hardware,drivers}`) |
| `PHASE10.md`, `PHASE11.md`, `PHASE12.md` | phase documentation |

## Networking (bnet)

`bnet` manages the interfaces directly (ioctls, no `system()`/`popen()`):

```
bnet status                 interface/route/DNS summary
bnet list / info <iface>    names and per-interface details
bnet up/down <iface>        bring interface up/down
bnet ip <iface>             show addresses;  ip add/ip del assign
bnet route                  table;  route add/del <dest> <gw> [iface]
bnet dns                    show;  dns set <ns>...  (writes /etc/resolv.conf)
bnet dhcp <iface>           acquire lease via busybox udhcpc
bnet ping / resolve         reachability + lookup (own resolver via NSS)
bnet sockets                listening TCP/UDP sockets with owning process
bnet apply                  apply /etc/bulum/network.conf
```

- `/etc/bulum/network.conf`: `interface=`/`method=dhcp|static`/`address`/`gateway`/`dns`
  blocks; the `network` service runs `bnet apply` (RESPAWN=0, PIDFILE so the
  supervisor does not loop on it; udhcpc keeps `/var/run/bulum-network.pid`).
- Exit codes: 0 ok, 2 usage/invalid input, 3 interface does not exist, 1 failure.
- The udhcpc helper uses the DHCP-provided prefix (`${subnet:-24}`) instead of a
  hardcoded `/24`.

## Security

- `bsecure audit` (and quick `status`): /etc/shadow/passwd/group permissions,
  /root + /root/.ssh, /tmp sticky bit, dropbear host keys, service users
  (httpd uid 110, no-login shell), SUID/SGID scan, world-writable check,
  listeners, iptables presence.  `--root <dir>` audits a tree
  (`BSECURE_ANY_OWNER=1` tolerates build-host ownership in fixtures).
  Exit code: 0 clean, 1 warnings, 2 critical.  VM audit: 16 passed,
  4 warning, 0 critical (warnings are intentional: telnetd/httpd enabled,
  no iptables userspace).
- `bfirewall status|enable|disable`: probes `/proc/net/netfilter` +
  `/proc/net/ip_tables_names` + for the iptables binary; since the kernel is
  built without iptables userspace (`CONFIG_IP_NF_IPTABLES=m`, no tool, no
  `/lib/modules`) it reports "firewall unavailable" honestly instead of
  pretending.  Rules are a conservative allowlist (established/related in,
  loopback, ICMP echo, dhcp/dns, ssh/http/telnet) applied with `execve`.
- Hardening applied: `/root` 700, `/etc/dropbear` 700 + 600 keys (created at
  first boot and persisted on disk.img), `httpd -u httpd` drops to uid 110,
  telnetd authenticates via `/bin/login` (no more unauthenticated root shell),
  dropbear runs `-R -I 300 -K 3600 -P /var/run/dropbear.pid` (idle timeout +
  keepalive, keys regenerated only if missing).
- `libnss_dns.so.2`/`libnss_files.so.2` are shipped so statically linked
  `bnet resolve`/`ping` can resolve names.

## Boot

1. GRUB (memdisk) → `boot-uefi.img` (FAT ESP, vda)
2. `initramfs-switch.cpio.gz` → tiny `/init` mounts `/dev/vdb` rw, `switch_root` → `/sbin/init`
3. Persistent root on `disk.img` (vdb, ext4): users, `/home/melton`, crontabs, dropbear keys
4. `init → bctl boot` starts services in dependency order (9/9), supervised + logged
5. Recovery entry: full ramdisk initramfs, shell, no services

## Services (bctl)

syslogd, mdev, disk, network (`bnet apply`), dropbear, httpd, telnetd, crond,
ntpd — meta-driven deps, pidfiles, respawn with crash-loop guard (5/min → 30s
backoff), `service.state` files under `/var/run/bulum`, boot log
`/var/log/bulum/boot.log`.

`bstatus` now includes a Network section (interfaces, gateway, DNS, summary).

## Package manager (bulum)

```
bulum update                 fetch + verify repository indexes (signed Release)
bulum install <pkg|deb>      install package, resolve dependencies, journaled
bulum remove <pkg>           remove package, check reverse deps
bulum search/info/list       query available / installed packages
bulum upgrade                upgrade installed packages to newest available
bulum clean / doctor         drop cached archives / verify DB + recover interrupted ops
```

- SHA-256 + MD5 + 2048-bit RSA PKCS#1 v1.5 verification implemented in `bulum_crypto.c`
  (no external crypto); every `update` checks the repo signature against `/etc/bulum/keys`,
  every download is checked against `SHA256:`/`Size:` from the signed index.
- Installs/removes are journaled under `/var/lib/bulum/tx`; an interrupted operation
  is rolled back on the next run, and the staged-file walk refuses to overwrite files
  owned by other packages or unmanaged files (unless `--force`).
- dpkg-compatible state: `/var/lib/dpkg/status` (+ `.list`, `.md5sums` per package),
  indexes in `/var/lib/bulum/lists/`, archives in `/var/lib/bulum/cache/`,
  extraction in `/var/lib/bulum/stage/`, rollback backups in `/var/lib/bulum/backup/`.
- Test debs are built with gzip compression (`dpkg-deb --build -Zgzip`): busybox's
  `dpkg-deb -x` silently extracts nothing from zstd archives.

## Build

```
wsl -d Ubuntu-24.04 -- bash -lc "cd /home/melton/myos && rebuild-userspace.sh && build-all-uefi.sh"
tools/make-test-repo.sh          # regenerate /tmp/bulum-repo + signing key
```

## Tests

| Suite | Covers |
|---|---|
| `test-uefi8.sh` | full boot: grub, switch_root, login, fb, root on vdb, poweroff (15 checks) |
| `test-uefi9.sh` | persistence: user login, file survives across reboot in disk.img |
| `test-uefi10.sh` | bctl: stop/start, enable/disable, respawn after kill -9, log, diagnose (10 checks) |
| `test-recovery.sh` | GRUB recovery entry → recovery shell |
| `tools/verify.sh` | host crypto checks: sha256/md5, RSA verify, randomized mulmod vs python |
| `test-bulum.sh` | bulum end-to-end in VM: update, search/info, install, run, list, tampered sig rejected, recovery, upgrade, remove, doctor, poweroff (15 checks) |
| `tools/test-bnet.sh` | host-side bnet: commands, exit codes 2/3, config parsing (22 checks) |
| `tools/test-bsecure.sh` | host-side bsecure: hardened vs broken fixtures, --root, exit codes (10 checks) |
| `test-bnet-vm.sh` | in-VM: bnet read-only + ping, bsecure audit (0 critical), bfirewall honest report, services up, key persistence, poweroff (21 checks) |
| `tools/test-bsh.sh` | bsh: non-interactive + real-pty interactive checks (18 checks) |
| `tools/test-bgfx.sh` | host-side bgfx: fake-fb render checks, status/info/hash, drm honesty (17 checks) |
| `tools/test-binput.sh` | host-side binput: options, empty/malformed input dirs, sysfs classification, timeout (13 checks) |
| `test-bgfx-vm.sh` | in-VM: virtio-gpu fb 1280x800, selfcheck hash, renderer, readback, input detection, HMP/QMP keyboard+mouse events captured by binputtest, graphics service state, bterm+bsh, SSH E2E, headless fallback (G1–G17 + H1–H6) |
| `tools/test-bdesktop.sh` | host-side bdesktop composite shell + Phase 10 app layer regression (G01–G11 + A0x, 25/25) |
| `tools/test-apps.sh` | host-side `bapp_hosttest`: window create/render, all 11 apps open, dialog guard, ANSI strip, prefs round-trip, /proc readers, exec validation, clipboard, file associations (13 checks) |
| `tools/chk-target-bdesktop.sh` | verifies the full bdesktop + 11 apps statically links for aarch64 |
| `tools/chk-apps.sh` | per-file host compile check (error-only) for every Phase 10 app module |
| `test-poweroff.sh` | boot → login → poweroff → no qemu processes |

## Graphics (bgfx) + Input (binput)

Phase 8 brings graphics and input to BulumOS on QEMU virt (aarch64):

- Kernel: `CONFIG_DRM_VIRTIO_GPU=y`, `CONFIG_FB=y` (fbdev on virtio-gpu),
  `CONFIG_VIRTIO_INPUT=y` (virtio keyboard + mouse), `CONFIG_INPUT_EVDEV=y`,
  `CONFIG_DEVTMPFS=y` (device nodes at boot).  GRUB menu entry boots with
  `video=...` defaults; the fb device is `/dev/fb0`, 1280x800 XRGB8888.
- `bgfx` library + CLI: fbdev mmap renderer — colors, lines, rectangles,
  filled rects, 8x8 font text, image blits, PNG (zlib inflate) and LCN
  decode; `bgfxdemo --selfcheck` hashes the rendered output (deterministic
  golden hash) and the CLI has `hash`/`image` commands to verify from a
  script.  A `test` command renders a known pattern; `bgfx status` reports
  backend honestly (`backend=drm` → "not implemented").
- `binput` library + `binputtest`: scans `/dev/input/event*`, classifies
  keyboard vs mouse from `/sys/class/input/inputN/capabilities/{ev,key,rel}`
  (sysfs bitmap parsing, MSB-first hex), reads events with poll + bounded
  FIFO; `binputtest --timeout N` monitors for N seconds and reports events
  per device (`Keyboard: KEY_X pressed`, `Mouse: X +7`, ...).
- Graphics service: oneshot `graphics` service writes
  `/var/run/bulum/graphics.state` (available/resolution or unavailable);
  `bctl stop/start graphics` re-runs it.  Headless boots report
  "Graphics: unavailable" and fall back to the serial console gracefully.
- Device permissions via `mdev.conf`: fb/dri `660 root:video`,
  `/dev/input/*` `660 root:input`, groups `video`/`input` in `/etc/group`.
- Input verification in QEMU uses QMP `input-send-event` and HMP
  `sendkey`/`mouse_move`/`mouse_button`; virtio-input devices are attached
  with `vectors=3` so the event vq gets its own MSI vector.

Known limitation: `binput` supports keyboards and relative mice; absolute
touch/tablet devices are still readable but reported as `other`.

## Desktop shell + compositor (Phase 9)

Phase 9 builds a native C desktop on the Phase 8 stack — no X11/Wayland/GTK:

- `bwm` (window manager/compositor): bounded window table, z-order + focus,
  title-bar chrome with `–`/`[]`/`x` buttons, move (title drag), resize
  (bottom-right grip), minimize/maximize/restore/close, pointer hit-testing
  and event dispatch.  Composes the whole desktop into an **off-screen back
  buffer** (double buffering) in the device pixel format and presents it with
  a single `bgfx_blit_from`, so it works on any framebuffer format.
- `bshell` (desktop shell): system panel (launcher button, taskbar, clock +
  notification/battery/wifi tray), an app launcher menu, the desktop
  background, and notification popups; registers built-in in-process apps.
- `bdesktop`: the event-loop daemon that init order turns into the 
  `desktop` service (`tools/install-services.sh`), started only when the
  `graphics` service reports the framebuffer available.
- Config: `/etc/bulum/desktop.conf` (theme, background) and
  `/etc/bulum/theme.conf` (per-colour overrides via the `btheme` engine).
- `bapp` stays the launcher backend: `.bapp` files on disk, or built-in
  defaults; external apps launch through the safe `fork`/`execve` path.
- Verification: host functional suite `tools/test-bdesktop.sh` (G01–G11,
  12/12 against a software framebuffer stub), host/target builds, and
  `test-bdesktop-vm.sh` (D1–D10 in QEMU).  See `PHASE9.md`.

## Desktop apps (Phase 10)

Phase 10 turns the in-process desktop into a real application ecosystem.  The
Phase 10 apps are **in-process modules** hosted by `bdesktop` (cross-process
windowing is deferred to Phase 11); they register as `internal:*` builtins the
same way the Phase 9 demo app did.

- `bappui` — a tiny widget toolkit (labels, buttons, lists, text inputs,
  checks, textareas, separators) layered on `bwindow`.  Windows are created via
  `appui_win_create`, widgets added by value with `appui_add`, and events
  delivered through a callback; per-app state lives in `ui->extra` (heap
  allocated by the app, freed by `appui_win_close`).
- 11 applications, each a self-contained module: **Files**, **Settings**,
  **Packages**, **System Monitor**, **Network**, **Security**, **Services**,
  **Logs**, **Users**, **About**, **Terminal** (a bounded command console via
  the safe `bsys_exec` path; a full PTY terminal stays in Phase 11).
- `bsys` — safe subprocess capture (`bsys_exec`, whitelisted absolute paths,
  bounded pipe read, `waitpid` so no zombies), `/proc` readers (CPU, mem,
  processes), kernel/hostname/arch, preferences (`/etc/bulum/preferences.conf`),
  and a **clipboard** (`bsys_clip_set/get/has`).
- `bapps` — the dispatcher (`internal:name` → module), a shared non-blocking
  confirm dialog (`bapp_dialog`, single-dialog guard), and **file
  associations** via `/etc/bulum/associations.conf` (`.ext=app` or
  `.ext=internal:x`).
- `.bapp` metadata gained `version=` and `description=`; `bapp_rescan` parses
  them from `/usr/share/applications/*.bapp`.
- Launcher keyboard search: type to filter apps by name/category inside the
  launcher, Enter launches, Backspace/Esc clear, arrow keys move the
  highlight — all driven by `bshell_key` with `bdesktop` forwarding unbound
  key events.
- Icons: `bicon` gained `log` and `services` aliases.
- Launch integration: `bshell_launch` now dispatches builtin apps through
  `bapps_open("internal:name")` instead of the Phase 9 demo window.

Security: the app layer never uses `system()`/`popen()` and no unbounded
`strcpy`/`strcat`/`sprintf`; all subprocess work goes through the whitelisted,
bounded `bsys_exec`.  See `PHASE10.md` for the full spec.

## Terminal (bterm)

`bterm` is a small native terminal frontend: it allocates a PTY, runs the child
(`/bin/sh` by default, or `bterm -e <program> [args...]`) on the pty slave, and
forwards raw bytes — so vi/less/top and full ANSI color work exactly as on the
underlying terminal.  In parallel a minimal VT parser tracks the output into a
bounded scrollback ring (default 10000 lines; `/etc/bterm.conf` +
`~/.config/bterm/config`).

Scrollback viewer (Ctrl+A prefix — a plain Ctrl+A still reaches the shell):

```
Ctrl+A Ctrl+A  enter viewer      Ctrl+A u / PgUp   scroll back a page
Ctrl+A d / PgDn scroll down      Ctrl+A q / Enter  return to live output
viewer: u/PgUp, d/PgDn page; k/j line; q/Ctrl+A exit
```

Signals: SIGWINCH pushes the real terminal size into the pty, SIGCHLD reaps the
child and propagates its exit status, SIGTERM/SIGINT kill the child and exit;
the terminal is always restored on exit.

## Packages (Phase 12)

`bulum`/`bpk` (symlink) manages signed repositories on both architectures:

```
bulum update            fetch + RSA-verify Release.sig + Packages per repo
bulum search <s>        match name/version/section/maintainer/description
bulum info <name>       status (installed/available), arch, size, deps
bulum install <..>      dependency-aware plan; .deb from repo or local file
bulum remove <name>     uninstall + residue cleanup; respects dependencies
bulum upgrade           newer-version plan + install
bulum verify [all|..]   per-file modified/missing check (hash + mtime)
bulum clean | doctor | repo add/remove/list | --json | version
bulum list|owns|log     installed packages, file ownership, install log
```

Repos: `main` (self-signed httpd test repo, `tools/make-test-repo.sh`) and
`official` (44 entries — 22 base packages × aarch64/x86_64 — built by
`tools/build-official-repo.sh` into `out/repo`).  Architecture policy:
`all`/`any` match every system, `aarch64`↔`arm64` and `x86_64`↔`amd64` are
alias pairs, `BULUM_ARCH` overrides detection.  The seeded database
(`tools/seed-rootfs-db.sh`) makes search/info/install work offline too.
Debian `.deb` archives are extracted with busybox `dpkg-deb -x`, installed
into `/`, and recorded for `verify`/`remove`/`upgrade`.  See `PHASE12.md`.

All currently pass.