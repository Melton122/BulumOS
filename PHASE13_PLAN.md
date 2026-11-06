


# BulumOS Phase 13 Plan: Storage & Filesystem Platform

## Audit Findings

- `src/bhw.c` currently enumerates only names in `/sys/block`; `bhw_block_count()` and `bhw_block_names()` expose no device sizing, partition, filesystem, or mount data. The Hardware report and app consume these minimal helpers directly.
- `src/bstatus.c` is a standalone status executable with no `--json` support and no storage section. `bctl`, `buser`, `bsecure`, and `bnet` are separate existing subsystems; Phase 13 should integrate reporting rather than replace their ownership or command behavior.
- The desktop is an in-process `bapps`/`bappui` application framework. Apps are selected by `.bapp` metadata under `/usr/share/applications` and an `internal:<name>` dispatcher. Existing app metadata and desktop modules are present in both rootfs trees. The host app test checks all registered builtins and must include Storage.
- `rebuild-userspace.sh` and `tools/build-x86-userspace.sh` use explicit compile/link lists for utilities and desktop modules; each needs matching Phase 13 wiring. The two rootfs trees are separate outputs.
- `tools/build-official-repo.sh` discovers every `packages/*.pk` manifest automatically. `tools/seed-rootfs-db.sh` likewise indexes every manifest, so a combined `bstorage` package can follow the existing BPK format without changes to BPK itself.
- There is no existing storage-focused host or VM test. Current Phase 12 VM testing is implemented by shell scripts that boot QEMU and inspect serial output; desktop smoke coverage is separate.
- The worktree has pre-existing user changes across build scripts, rootfs files, and security/user files. Those changes are out of scope and must be preserved.

## Missing Functionality

- Bounded disk and partition records, portable sizes/sector data, sysfs and `/proc/partitions` fallback discovery.
- Mountinfo parsing including octal escapes, filesystem type, flags, and mount state.
- Filesystem capacity and used/available data via `statvfs`, plus safe path-to-mount lookup.
- Read-only CLIs for listing disks, mounts, filesystem usage, directory usage, and filesystem diagnostics; no format, partition-edit, mount, or repair operation.
- Storage status and hardware reporting backed by one shared discovery implementation; dedicated Storage desktop view with explicit refresh.
- Host fixture tests, ARM64/x86-64 guest verification, package/build integration, and Phase 13 documentation.

## Architecture

- Add `src/bstorage.h` and `src/bstorage.c` as the storage source of truth. Use fixed-size strings and bounded arrays with `uint32_t`, `uint64_t`, and `size_t`; expose scan/count/get APIs for disks, partitions, filesystems, mounts, usage, and device diagnostics.
- Discover devices primarily from `/sys/class/block`, merge partition and device facts from sysfs and `/proc/partitions`, and tolerate missing sysfs attributes. Keep Linux device naming generic, including NVMe partition suffixes.
- Parse `/proc/self/mountinfo` with bounded line/token handling and decode kernel octal escapes. Use mountinfo's major:minor relationship and mount source/target/type/options; use `statvfs()` for usage of mounted paths.
- Allow host tests to point the library at fixture roots through a narrow test-only environment/configuration boundary; production defaults remain `/sys`, `/proc`, and `/dev`. Avoid command execution and global mutable cache; explicit `bstorage_scan()` refreshes the snapshot.
- Keep old `bhw` CPU/network/GPU/etc. APIs intact. Replace its storage enumeration implementation with adapters backed by `bstorage`; link the shared source where hardware tools or desktop binaries need it.
- Keep `bmount` discovery-only and `bformat` help/safety-only. `bfsck` reports filesystem, device, mounted/read-only state, and whether offline checking may be appropriate; it never invokes a checker in Phase 13.
- Use the existing `bappui` toolkit for `bapp_storage.c`. A refresh action performs a fresh scan; no timer polling.
- Add one `packages/bstorage.pk` manifest containing the storage command suite and shared library is statically linked into consumers, consistent with current package payload conventions. Rely on automatic manifest discovery and normal rootfs seeding.

## Files to Modify

- `src/bhw.c`, `src/bhw.h`, `src/bhardware.c`, and `src/bstatus.c`
- `src/bapps.c`, `src/bapps.h`, `src/bapp_hosttest.c` (and any actual desktop host-test source/list), plus both desktop source manifests in build scripts
- `rebuild-userspace.sh`, `tools/build-x86-userspace.sh`
- `tools/build-official-repo.sh` should need no source change because it globs manifests; verify the generated package is discovered instead
- Rootfs integration outputs: `rootfs/usr/bin`, `rootfs/sbin`, `rootfs/usr/share/applications`, and corresponding `rootfs-x86` paths, populated by build scripts rather than manually diverging them
- `BULUMOS.md`, `README.md` if appropriate, and new `PHASE13.md`

## Files to Create

- `src/bstorage.c`, `src/bstorage.h`
- `src/bdisks.c`, `src/bmount.c`, `src/bdf.c`, `src/bdu.c`, `src/bfsck.c`, `src/bformat.c`
- `src/bapp_storage.c`
- `packages/bstorage.pk`
- `tools/test-bstorage.sh`, `tools/test-bstorage-vm.sh`, and a desktop storage test or focused extension to the existing desktop app host test
- `PHASE13.md`

## Testing Strategy

1. Compile the core and tools with host GCC using strict warnings; run `tools/test-bstorage.sh` against temporary sysfs/proc/mountinfo fixtures for disks, partitions, escaped mountpoints, unknown filesystems, malformed and long records, overflow boundaries, JSON validity, and missing/disappearing files. Tests must not inspect or mutate host disks beyond read-only system interfaces.
2. Run focused host CLI checks for `bdisks`, `bmount`, `bdf`, `bdu`, `bfsck`, and `bformat`, including symlink-loop and permission-failure behavior for `bdu`; confirm formatter/checker tools are never invoked.
3. Build both ARM64 and x86-64 userspaces. Verify package manifest staging and BPK repo discovery for both architectures without committing generated keys or repo outputs.
4. Run QEMU storage VM tests on ARM64 and x86-64 for discovery, root mount/filesystem/usage, bounded JSON, and malformed arguments. Run the desktop app smoke test on each graphical-capable path.
5. Run the existing Phase 1–12 host and VM regression suites required by the task, especially hardware, desktop, user, security, network, and BPK checks. Record suites unavailable due missing tools/resources instead of claiming success.
6. Review `git status`, `git diff`, and staged diff for secrets and generated/temporary artifacts; do not reset or clean the pre-existing dirty worktree.

## Security Considerations

- Discovery is read-only. No format, repartition, mount, unmount, or filesystem-repair operation is added.
- Treat sysfs/procfs, labels, paths, mount options, and CLI arguments as untrusted. Use bounded parsing, canonical path checks where required, checked arithmetic, and safe JSON string escaping.
- Directory usage uses `lstat`/non-following traversal by default, tracks visited directories or enforces depth/entry caps, handles races and permissions as nonfatal per-entry errors, and checks size accumulation for overflow.
- Never invoke a shell, `system()`, or `popen()`. No privilege escalation or automatic removable-media mounting.
- Filesystem diagnostics are informational only. Formatting output explicitly states that formatting is not implemented.

## Definition of Done

- The architecture, tools, integration, and safety model described above are implemented without replacing existing subsystems.
- Host fixture tests pass; both architecture builds pass; ARM64 and x86-64 VM tests and supported desktop tests pass.
- Existing Phase 1–12 regressions pass, or each unavailable/failed suite is reported explicitly with evidence and remaining work.
- `PHASE13.md` documents implementation and limitations, and `BULUMOS.md` reflects Phase 13 accurately.
- Worktree and generated artifacts are reviewed; unrelated user changes and credentials are preserved and not committed.
