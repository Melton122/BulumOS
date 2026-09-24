# BulumOS — Phase 12: Package Ecosystem & Repository

Phase 12 finishes BulumOS's package story: a real multi-architecture
**repository** for the `bpk`/`bulum` package manager, signed release metadata,
dependency resolution, an official repo seeded from the OS's own binaries, and
a graphical **Packages** desktop app — all working on **both aarch64 and
x86-64** from the same OS image.  Everything is additive: no kernel, boot or
file-system changes; the entire phase lives in userspace, scripts and the
desktop layer.

## Deliverables

| Area | Component | Status |
|---|---|---|
| Package manager | `src/bulum.c`, `src/bulum_crypto.c` — update/install/remove/search/info/list/upgrade/clean/verify/owns/doctor/version + `repo add/remove/list` + `--json` + self-verify | done |
| Debian archive | gzip `.deb` (ar + control.tar.gz + data.tar.gz) read/write via busybox/`dpkg-deb` `-e`/`-x`/`-f` | done |
| Signing | RSA-2048 + PKCS#1 v1.5 SHA-256 signatures on `Release` (repo) and per-package control (signed by `build-package.sh`) | done |
| Repos | `main` (test, self-signed httpd) + `official` (44 entries, 22 base packages × 2 arch) at `out/repo` | done |
| Tooling | `tools/build-package.sh`, `tools/build-official-repo.sh`, `tools/seed-rootfs-db.sh`, `tools/build-bb-x86.sh` | done |
| Desktop | `Packages` app in `src/bapp_packages.c` (list/search/details/window) + desktop regression | done |
| Tests | Host 23/23; VM e2e **23/23 on aarch64 and x86-64**; legacy suite 15/15; desktop host 13/13 | done |

## 1. Repository format

A repository is an HTTP directory served by plain `python3 http.server`;

```
out/repo/
  Packages            index — one control paragraph per package entry
  Release             repo metadata (origin, version, arch, sig metadata)
  Release.sig         RSA-2048 + PKCS#1 v1.5 SHA-256 signature over Release
  *.bpk               binary packages (ar + control.tar.gz + data.tar.gz)
  *.pub               (key already baked into rootfs /etc/bulum/keys/)
```

The seeded database (`/var/lib/bulum`, baked via `tools/seed-rootfs-db.sh`)
starts with 15 index entries, so `update`/`install`/`search`/`info` work
offline and unprivileged before the first network `update`.

Architecture policy: `all`/`any` match every system; `aarch64`↔`arm64` and
`x86_64`↔`amd64` are alias pairs; `sys_arch()` reads `/proc/self`, honors the
`BULUM_ARCH` override, and matches index `Architecture:` with `all`/native.
The repo `arch` field in `repos.conf` is informational only.

## 2. Package manager (bulum)

Commands (all verified in the VM on both arches):

- `update` — fetch + verify each repo's `Release.sig` against the baked public
  key, download `Packages`, parse into the available database:
  `[ OK ] main         15 packages (signature verified)`.
- `search <s>` / `info <name>` — match names, versions, section, maintainer,
  description, provides; `info` reports Status (installed/available), arch,
  installed-size, description, maintainer, origin.
- `install <name|file.deb>` — dependency-aware planner
  (`The following packages will be installed:`), downloads the cache deb,
  runs busybox `dpkg-deb -x` into the subroot, moves files into `/`,
  writes the installed database + download/install logs.  Local `.deb`
  install reads the control with `dpkg-deb -f` (busybox has no `-I`).
- `remove <name>` — uninstall + `/usr/share/doc` residue cleanup, refusing
  removal of a package others depend on (`[ OK ] removed hello 1.1`).
- `upgrade` — plan + install newer available versions
  (`upgh 1.0 -> 1.1`); `update` after publishing a new version picks it up.
- `verify [name|all]` — compare metadata/timestamps/hash of every recorded
  file (`verified: 1 files / modified: 0 / missing: 0`); `doctor` (and
  `verify all`) then reports `OK` with `modified: 0, missing: 0`.
- `clean`, `json`, `repo add/list/remove`, `version`.

Output is stable for testing: node headings, `[ OK ]`/`[ !! ]` prefixes and
`--json` (`{"repos":[...],"count":N}`) fixtures the VM suite asserts on.

### Fixes that mattered this phase

- **Multi-repo index clobber (install failure root cause):** the second index
  scanner restarted its entry counter at 0 per file, so the second repo's
  scan overwrote the first repo's `Filename/SHA256/Format` for the shared
  entries (and left its tail entries empty).  Now `parse_packages` folds
  `Filename/Size/SHA256/Format` back into the *same* paragraph pass
  (`idx = navail`), so the mapping between name/version/arch and the download
  URL can never drift.  The symptom had been `download <empty>` → the repo
  directory listing saved as the cache deb → busybox `dpkg-deb: invalid ar
  magic`.
- **`build-package.sh` manifest format:** `packages/*.pk` use lowercase
  `key=value` manifests (`name=`, `version=`, `architecture=%ARCH%`,
  `maintainer=`, `license=`, `description=`, `depends=`, ...); the builder
  now translates them into dpkg-control headings (`Package:`, `Version:`,
  `Architecture:`, ...) with an awk step, so the published control paragraphs
  match what `bulum.c` parses.
- **busybox dpkg-deb compatibility:** the guest busybox `dpkg-deb` implements
  `-c -f -e -x -X` but not `-I`; local-file install now uses `-f` (print the
  control paragraph), which both busybox and GNU dpkg-deb support.

## 3. Official repo + tooling

`tools/build-official-repo.sh` builds `out/repo` from the 22 `packages/*.pk`
manifests × both architectures (44 entries + `Packages` + `Release` +
`Release.sig`); each `.bpk` control is signed at build time and carries
`SHA256` + `Size` so the index is self-verifying.  The seed step runs during
the normal rebuild pipeline (`rebuild-userspace.sh` Phase 12 wiring and
`tools/build-x86-userspace.sh`), keeping both rootfs trees in lock-step with
the baked keys.

## 4. Desktop Packages app

`src/bapp_packages.c` renders the seeded/installed view (name, version,
status, size) into a scrollable `bappui` list with a detail panel, search
filters as you type, and install-status coloring — no `system()`/`popen()`,
all through the whitelisted `bsys_exec`.  Covered by `tools/test-apps.sh` and
the in-VM desktop smoke test.

## 5. Test matrix (all green)

| Suite | Result |
|---|---|
| `tools/test-bpk.sh` (host, sandbox dirs + fixture repo) | **23/23** |
| `tools/test-bpk-vm.sh arm64` (VM e2e, phases P0–P10) | **23/23** |
| `tools/test-bpk-vm.sh x86_64` (VM e2e, phases P0–P10) | **23/23** |
| `test-bulum.sh` (legacy Phase 5 CLI suite) | **15/15** |
| `tools/test-apps.sh` (desktop apps incl. Packages) | **13/13** |
| `test-bdesktop-vm.sh` (desktop smoke: D6/D9 self-tests rc=0) | PASS |

VM phases: arch/version/bpk-symlink, update (both repos, signatures), search,
info available, seeded list, owns, install plan + run, dependency resolution
(bar→hello), local `.deb` install (upgh 1.0), publish 1.1 → update →
upgrade 1.0→1.1, verify (+all), JSON repo list, clean, negative install,
remove + gone check, doctor — with correct per-arch downloads
(`hello_1.1_arm64.deb` vs `hello_1.1_amd64.deb`).