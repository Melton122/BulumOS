# BulumOS — Phase 11: Hardware & Driver System

Phase 11 gives BulumOS a real hardware layer on **both architectures** (aarch64
and x86-64): an in-kernel BulumOS driver framework, a portable userspace
hardware library, five native CLI tools, and desktop integration built on the
Phase 10 `bappui`/`bapp` framework.  Everything is deliberately small,
dependency-free and additive — it never conflicts with the standard Linux
device model (sysfs/procfs remain the source of truth).

## Deliverables

| Area | Component | Status |
|---|---|---|
| Kernel | `drivers/bulum/Kconfig`, `Makefile`, `bulum.h`, `bulum-core.c`, `bulum-dummy.c` | done |
| Kernel | `CONFIG_BULUM_DRIVERS` / `CONFIG_BULUM_DRIVER_CORE` / `CONFIG_BULUM_DRIVER_DUMMY` in both `.config.arm64` and `.config.x86_64` | done |
| Kernel | ARM64 `Image` + x86_64 `bzImage` rebuilt with the framework **built-in** | done |
| Userspace | `src/bhw.h` + `src/bhw.c` hardware API library | done |
| Userspace | `bhardware`, `bdevices`, `bdriver`, `bnetwork`, `bpower` CLIs | done |
| Desktop | `Hardware` app (`src/bapp_hardware.c`) + System Monitor shows driver framework | done |
| Tests | Host app test 13/13; guest boot 13/13 on both arches; `/proc/bulum` verified over SSH | done |

## 1. In-kernel driver framework (`kernel/drivers/bulum/`)

- `bulum.h` — framework header: `enum bulum_driver_type` (PLATFORM/EXAM/OTHER),
  `struct bulum_driver` (name, type, describe callback), register/unregister
  prototypes.
- `bulum-core.c` — the registry:

  - `bulum_driver_register()` / `bulum_driver_unregister()` (`EXPORT_SYMBOL_GPL`)
    with a bounded array (no dynamic alloc in atomic contexts).
  - `/proc/bulum/hardware` — a one-shot proc file reporting:
    `architecture` (from `CONFIG_ARM64`/`CONFIG_X86_64`), `cpus_online`
    (`num_online_cpus()`), and the synthetic `bulum` bus name.
  - `/proc/bulum/drivers` — a `seq_file` listing each registered driver:
    `name type state`.
  - `module_init`/`module_exit` to install the proc entries.

- `bulum-dummy.c` — example driver `bulum-info` that registers through the
  framework and is reported by `bdriver`/`bhardware`.

The framework is **built-in only** (no `CONFIG_MODULES` dependency, no misc
char device — procfs only, so it also works on the ARM64 config where
`CONFIG_MISC_DEVICES` is off).  Verified both kernels carry the symbols:
`bulum_core_init`, `bulum_dummy_init`, `bulum_driver_register`.

## 2. Kernel build (verified with `nm` before boot)

- ARM64: `make -j8 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image`
  → copied back to `kernel/arch/arm64/boot/Image`.
- x86-64: `make -j8 ARCH=x86_64 bzImage`
  → copied back to `kernel/arch/x86/boot/bzImage`.
- Configs kept per-arch in the repo: `kernel/.config.arm64`,
  `kernel/.config.x86_64` (both set `CONFIG_BULUM_DRIVERS=y`,
  `CONFIG_BULUM_DRIVER_CORE=y`, `CONFIG_BULUM_DRIVER_DUMMY=y`).

## 3. Userspace hardware API (`src/bhw.[ch]`)

Pure libc, portable across the busybox rootfs on both arches.  Reads only
procfs/sysfs:

- CPU: `/proc/cpuinfo` (model, architecture, cores, online).
- Memory: `/proc/meminfo` (total/free/available, `MemAvailable` handled).
- PCI: scan of `/sys/bus/pci/devices` with class/vendor/device names.
- GPU: `/sys/class/drm` (card names/drivers) + `/sys/class/graphics`
  (resolution, bpp, `fb*` path).
- Storage: `/sys/block` device names + count.
- Network: `/sys/class/net` + `statistics/` (`rx_bytes`/`tx_bytes`).
- USB: `/sys/bus/usb/devices` count.
- Audio: `/proc/asound` cards (absent on the current VM images → reported none).
- Power: `/sys/class/power_supply` — battery capacity + AC status (reports
  "AC-powered system" in VMs where no battery node exists).
- Kernel framework: `/proc/bulum/hardware` + `/proc/bulum/drivers`.

## 4. Native tools

All statically linked, stripped, installed into both rootfs trees:

| Tool | Purpose |
|---|---|
| `bhardware` | full hardware report (CPU/mem/GPU/storage/net/USB/audio/power + `/proc/bulum`) |
| `bdevices` | PCI / block / net / input enumeration |
| `bdriver` | BulumOS driver registry report (reads `/proc/bulum/drivers`) |
| `bnetwork` | interfaces with MAC/state/stats + IPv4 via fib_trie |
| `bpower` | battery/AC status + `shutdown` / `reboot` via `reboot(2)` |

## 5. Desktop integration

- New **Hardware** app (`src/bapp_hardware.c`): scrollable textarea showing CPU,
  memory, GPU, storage, network, USB and the kernel driver framework, with a
  Refresh button.  Registered as `internal:hardware` in `bapps.c` and as
  `bulum-hardware.bapp` in `/usr/share/applications` (launcher auto-discovers it).
- **System Monitor** (`src/bapp_monitor.c`) now also prints the `/proc/bulum`
  hardware line (architecture/cpus/bus) under "Drivers:".
- App count grew 11 → 12; host test suite updated (`test-apps.sh` → 13/13).

## 6. Verification

Host (`tools/test-apps.sh`): 13 passed / 0 failed, including **A02 all 12 apps
open windows** (files, settings, about, monitor, hardware, network, security,
services, logs, users, packages, terminal).

Guest headless boots (QEMU TCG, serial + SSH over dropbear):

- **ARM64** (AAVMF): `boot complete: 13/13 services`
  - `/proc/bulum/hardware` → `architecture : arm64`, `bus : bulum`
  - `/proc/bulum/drivers` → `bulum-info  unknown  bound`
  - `bhardware` rc=0, all 5 tools present, `bulum-hardware.bapp` present.
- **x86-64** (OVMF): `boot complete: 13/13 services` (identical results,
  `architecture : x86_64`).

Note: dropbear SSH host keys are now **pre-generated** into
`rootfs/etc/dropbear` (shared, copied into `rootfs-x86`), so no slow RSA
`dropbearkey` generation runs under TCG at first boot (that occasional 9 s
timeout was the only 12/13 in prior runs).

## 7. Build wiring

- `rebuild-userspace.sh` (ARM64) — builds the 5 tools plus `bdesktop`
  (with `bapp_hardware.c` + `bhw.c`) and writes `bulum-hardware.bapp`.
- `tools/build-x86-userspace.sh` — same for x86-64.
- `tools/test-apps.sh` — host test now links the hardware app.
- Boot images rebuilt: `bulumos-uefi.img`, `bulumos.iso`,
  `bulumos-uefi-x86.img`, `bulumos-x86.iso`, `disk.img`, `disk-x86.img`.

## Security & robustness notes

- No `system()`/`popen()` anywhere; tools use fork/execve paths only.
- All buffers and enumeration counts are bounded (`BAPPUI_MAX_WIDGETS`,
  `BAPP_MAX_APPS`, `BHW_MAX_STR`, fixed arrays in `bdriver`, etc.).
- Kernel proc readers are `seq_file`/one-shot, bounded, and never depend on
  `CONFIG_MODULES` or a misc device.
- The bulum bus is **synthetic**: it adds no real device conflicts with the
  Linux device model, and the whole Phase 11 layer degrades gracefully on a
  stock kernel (tools report the framework is absent instead of failing).