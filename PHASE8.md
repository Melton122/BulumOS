# BulumOS Phase 8 — Graphics & Display System (final report)

## Summary

Phase 8 adds a complete graphics and input stack to BulumOS (custom aarch64
Linux, QEMU `virt` machine): a framebuffer graphics library with CLI and
demo, an evdev input library with keyboard/mouse classification, kernel
support (virtio-gpu fbdev, virtio keyboard/mouse, evdev, devtmpfs), a
graphics service with honest headless fallback, device permissions via
mdev, and host + in-VM test suites.  All suites are green.

## Deliverables

| Artifact | Location | Notes |
|---|---|---|
| Graphics library | `src/bgfx.c` + `src/bgfx.h` | fbdev mmap, colors, line/rect/fill, 8x8 VGA font, text, PNG/LCN decode, readback hash |
| Font generator | `tools/gen-bgfx-font.py` | emits `src/bgfx_font8x8.h` |
| CLI + tools | `src/bgfxcli.c`, `src/bgfxinfo.c`, `src/bgfxdemo.c` | `bgfx`, `bgfxinfo`, `bgfxdemo` |
| Input library | `src/binput.c` + `src/binput.h` | evdev scan, sysfs classification, bounded FIFO, poll+read |
| Input tool | `src/binputtest.c` | `--list`, `--timeout N` (seconds) |
| Config / udev | `rootfs/etc/bulum/graphics.conf`, `rootfs/etc/mdev.conf` | mode/backend; fb 660 root:video, input 660 root:input |
| Kernel | `kernel/.config`, `build-kernel.sh` | virtio-gpu/fbdev/virtio-input/evdev/devtmpfs built in |
| Host tests | `tools/test-bgfx.sh` (17), `tools/test-binput.sh` (13) | green |
| VM tests | `test-bgfx-vm.sh` | G1–G17 + headless H1–H6, green |
| Docs | `BULUMOS.md` | updated with Phase 8 section |

## Kernel graphics/input config (all =y)

`CONFIG_DRM=y`, `CONFIG_DRM_VIRTIO_GPU=y`, `CONFIG_FB=y`,
`CONFIG_FB_VIRTIO=y`, `CONFIG_INPUT=y`, `CONFIG_INPUT_EVDEV=y`,
`CONFIG_INPUT_KEYBOARD=y`, `CONFIG_INPUT_MOUSE=y`,
`CONFIG_VIRTIO_INPUT=y`, `CONFIG_VIRTIO=y`, `CONFIG_DEVTMPFS=y`,
`CONFIG_DEVTMPFS_MOUNT=y`.

## Debugging story (input event delivery)

An apparent "no input events reach the guest" problem consumed most of the
effort; the truth was three independent artifacts:

1. **`binputtest --timeout N` treated N as milliseconds** (poll timeout
   passed straight through) while the docs/tests said seconds.  Fixed in
   `src/binputtest.c` (`binput_poll(timeout * 1000)`).  The stale binary in
   the VM rootfs made every in-VM binputtest exit after ~40 ms, looking like
   "no events".
2. **Device classification always returned `other`** because
   `event_to_input()` used `fopen()` on `/sys/class/input/eventN/device`,
   which is a symlink to a *directory* (EISDIR).  Fixed with `readlink()` +
   basename; `SYS_INPUT` is now overridable (`BINPUT_SYSFS`) so the host
   suite covers classification with fixtures.  Also fixed the path-builder
   (`INPUT_DIR` vs `BINPUT_DIR` override) and basename handling.
3. **The rootfs `od` binary proved unreliable** for reading evdev nodes;
   all event verification is now done with the rebuilt `binputtest`.

Bisection used an x86_64 build of the same kernel source in QEMU with a
minimal initramfs (decidedly working) versus the aarch64 VM, plus temporary
printk instrumentation in `virtio_input.c` / `input.c` / `evdev.c`
(removed afterwards).  Instrumentation proved: QEMU pushed events into the
virtqueue (trace events), the guest MSI fired (with `vectors=3`),
`virtqueue_get_buf` returned the events, `input_event()` ran,
`input_pass_values` reached the open evdev handle, `evdev_events` had 1
client — i.e. the whole stack was fine once the test tools were correct.

QEMU input injection used in tests: QMP
`input-send-event` (`qcode` string + `down` flag; mouse `rel`/`btn` without
a `device` field) and HMP `sendkey` / `mouse_move` / `mouse_button`.  The
virtio keyboard/mouse PCI devices are attached with `vectors=3` so the
event vq gets a dedicated MSI vector (default shared vector never fired).

## VM verification (test-bgfx-vm.sh)

Graphical boot: G1 boot marker, G2 `bgfxinfo` + fb geometry, G3 device
nodes (fb0/dri/input), G4 `bgfx status`, G5 `bgfx info`, G6 renderer, G7
`bgfxdemo --selfcheck` (golden hash, 1280x800), G8 framebuffer readback,
G9 input detection (`event1 keyboard (QEMU Virtio Keyboard)`, `event2
mouse (QEMU Virtio Mouse)`), G10 keyboard events (`KEY_A pressed /
released` via HMP sendkey captured by binputtest), G11 mouse events
(`X +5`, `LEFT pressed/released` via mouse_move/mouse_button), G12
graphics service state, G13 `bctl stop/start graphics`, G14 bterm+bsh,
G15 SSH E2E from host, G17 clean poweroff.

Headless boot: H1 marker, H2 `bgfxinfo` headless, H3 `bgfx status`, H4
service state ("Graphics: unavailable"), H5 no fb/dri, H6 shell OK.

## Regression (all green)

Host: `tools/test-bgfx.sh` 17/17, `tools/test-binput.sh` 13/13,
`tools/test-bsh.sh` 18/18, `tools/test-bnet.sh` 22/22,
`tools/test-bsecure.sh` 10/10, `tools/test-bterm.sh` 31/31,
`test-bulum.sh` 15/15 (fixed `block()` prompt matching for the real
`root@BulumOS:~# ` prompt), `test-uefi8/9/10.sh`.
VM: `test-bgfx-vm.sh`, `test-bnet-vm.sh`, `test-bterm-vm.sh`.

## Build/size notes

- Kernel `Image` 53,152,256 B; memdisk ≥64 MiB, ESP ≥72 MiB (EFI app
  67,915,776 B); `build-all-uefi.sh`/`rebuild-uefi.sh` gained
  `set -euo pipefail` (a `| grep` pipeline previously masked a mid-build
  failure and repackaged a stale EFI app).  Kernel builds happen in the
  space-free mirror `/home/melton/ksrc`.
- `bulumos-uefi.img` 75,497,472 B, `bulumos.iso` 143,790,080 B; kernel
  md5 verified inside both artifacts.

## Security notes

- No secrets in repo; `/dev/input/*` root:input 660, fb/dri root:video 660,
  `/root` 700.  `binput`/`bgfx` parse length-checked buffers; malformed
  device data and empty dirs handled (host tests cover both, valgrind when
  available).
- No `system()`/`popen()` in bgfx/binput; no shell interpolation.

## Known limitations

- `bgfx status` reports `backend=drm` honestly as "not implemented"
  (detection only); rendering is fbdev-based.
- Absolute touch/tablet devices are readable but classified `other`.
- Graphics is a console framebuffer + demo/CLI; no compositor/desktop
  (out of scope).