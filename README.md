# BulumOS

A custom Linux distribution built from the ground up — its own kernel driver
framework, init (PID 1), busybox userspace, native C utilities, desktop shell,
app framework, and a signed package manager. Bootable and testable in QEMU on
**aarch64 and x86-64**.

## Highlights

- **binit** — own `/sbin/init` (PID 1): mounts, hostname, service boot, login, shutdown
- **bctl** — service manager: boot/shutdown/start/stop/status/supervise/log
- **bulum / bpk** — signed package manager: `update/install/remove/search/info/list/upgrade/verify/clean/doctor/owns/repo` with dependency resolution, Debian-style archives, RSA-2048 + SHA-256 signed repos, multi-arch (`aarch64`/`x86_64`), JSON output
- **bterm, bsh, bnet, bsecure, bfirewall, bstatus, blog** — native console, shell, networking, security tools
- **bdesktop** — full desktop shell: compositor, windowing, launcher, widget toolkit (`bappui`), and 12 built-in apps (files, settings, packages, hardware, terminal, ...)
- **Phase 11 hardware layer** — in-kernel driver registry (`/proc/bulum/{hardware,drivers}`) + `bhardware/bdevices/bdriver/bnetwork/bpower`
- **Phase 12 package ecosystem** — 22 base packages in a signed multi-arch official repo, seeded offline DB, desktop Packages app (23/23 VM e2e checks on both arches)

## Quick start

The public repo intentionally trims the Linux kernel source tree (several GB);
keep only the per-arch configs (`kernel/.config.*`) and the custom driver
(`kernel/drivers/bulum`). A full build needs a configured kernel + busybox.

```sh
# host-side unit suites (build tools run on a plain Ubuntu/glibc host)
bash tools/test-bpk.sh          # package manager host checks (23 checks)

# VM e2e on both architectures (QEMU + UEFI)
bash tools/test-bpk-vm.sh arm64
bash tools/test-bpk-vm.sh x86_64

# interactive VM
bash tools/boot-bpk-vm.sh arm64    # login root / root
```

Inside the VM:

```sh
bulum update            # fetch + RSA-verify both repos
bulum search hello
bulum install hello && hello
bulum install bar       # resolves hello dependency
bulum verify all
bulum doctor
```

## Test status

| Suite | Result |
|---|---|
| `tools/test-bpk.sh` (host) | 23/23 |
| `tools/test-bpk-vm.sh` aarch64 / x86_64 | 23/23 each |
| `test-bulum.sh` (legacy CLI) | 15/15 |
| `tools/test-apps.sh` (desktop apps) | 13/13 |

## Layout

- `src/` — native C sources (init, services, shell, desktop, apps, package manager, hardware)
- `rootfs/`, `rootfs-x86/` — system root trees (busybox, `/etc/bulum/*` service tree)
- `packages/` — package manifests for the official repo (`*.pk`)
- `tools/` — builders, repo tooling, host test suites
- `*.sh` — build (UEFI/initramfs/disk) and in-VM test scripts
- `kernel/` — trimmed: per-arch `.config.*` + `drivers/bulum` only
- `PHASE8.md` … `PHASE12.md`, `BULUMOS.md` — phase documentation

## License

MIT — see [LICENSE](LICENSE).