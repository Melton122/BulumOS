# Phase 15: KDE Plasma Integration

## Objective

Transition BulumOS from its native `bappui` shell to KDE Plasma while retaining
BulumOS init, services, package management, and native utilities.

## Decisions

* Keep BulumOS init, service definitions, and native tools; use Ubuntu 24.04
packages as the initial KDE runtime source.
* Build into an isolated `out/plasma-x86` stage. Do not overwrite
`rootfs-x86`, which may contain local changes.
* Bring up x86-64 first, then port the working stack to aarch64.
* Provide graphical account management through a separately authorized
privileged service, not by running the desktop session as root.

## Milestones

### 1\. x86-64 build foundation

* Stage the display server, Qt, KDE Frameworks, KWin, Plasma, and runtime
dependencies from Ubuntu 24.04 amd64 packages.
* Record package versions and validate the userspace and executable/library
presence.
* Integrate the runtime with the BulumOS package repository and boot the
resulting rootfs in QEMU.

### 2\. Plasma session

* Provide the Wayland/X11 display stack, D-Bus, input and graphics support.
* Start the session as the configured regular user through `bctl`.
* Keep the native `bdesktop` available as a recovery/fallback session.

### 3\. BulumOS integration

* Expose BulumOS services, packages, storage, network and system status through
KDE applications or Plasma integrations.
* Retain `bctl`, `bulum`, `bstorage`, and native BulumOS tools.

### 4\. Account management

* Add GUI user creation, password changes, group management, and safe account
removal.
* Route privileged changes through a narrow helper/service with explicit
validation and authorization.
* Keep account creation available from the console for recovery.

### 5\. aarch64 and release verification

* Port the validated x86-64 build to aarch64.
* Verify boot, login, graphics, networking, package installation, user
management, session shutdown, and recovery on supported QEMU targets.

## Current status

An Ubuntu 24.04 amd64 minbase was staged at `out/plasma-x86` with Plasma
5.27.12, KWin X11, Xorg, D-Bus, login tools, and required runtime packages.
BulumOS init, `bsh`, native `b\*` tools, service definitions, account settings,
and `.bapp` metadata are overlaid into that isolated tree. This is a package-
based runtime stage, not a KDE source build or a merge into the production
`rootfs-x86`; that existing worktree was left untouched.

The staging checks pass for `plasmashell --version`, Xorg, D-Bus, package
configuration, and the KWin/session executable shared-library dependencies.
The stage boots through BulumOS init in QEMU; D-Bus, Xorg, KWin, and
`plasmashell` remain running, and X11 accepts client queries. Plasma's
`startplasma-x11` launcher is configured for its non-systemd session path, and
the activity manager is started before the session to avoid a D-Bus startup
race. The build now ensures the regular user's KDE cache is writable; an earlier
root-owned cache caused shader-cache permission errors.

The isolated runtime now includes Konsole, KDE System Settings, the KDE Users
module, AccountsService, and PolicyKit. A dedicated `bulum-admin` group is
authorized only for AccountsService user administration; the configured
`melton` desktop account belongs to that group. D-Bus service activation for
AccountsService and PolicyKit is configured for BulumOS's non-systemd init.
The desktop starts from a seeded Plasma panel layout instead of a runtime D-Bus
script, avoiding a KWin crash observed when the shell was still starting.

The stage-based QEMU harness passes its boot and integration checks, including
Konsole and Users settings launch, a mapped Plasma panel, and successful
AccountsService user creation. Its visible desktop screenshot is saved at
`out/plasma-x86-desktop.png`. KWin compositing remains disabled because its
OpenGL initialization froze/crashed in earlier QEMU runs.

`build_kde.sh --image` creates a separate bootable x86-64 set without changing
`rootfs-x86` or the normal image artifacts. The standalone image at
`out/plasma-x86-image-verified2` was booted in QEMU and passed all 30 guest checks,
including authorized account creation and removal, Konsole, the Users settings
module, and a mapped 1280x800 desktop and panel. Its ext4 root disk passes
`e2fsck`. The QEMU framebuffer screendump for this image was black despite the
mapped desktop, so the verified stage screenshot above is not represented as a
screenshot of this image.

The Plasma stage now builds BulumOS init with a volatile `/run` tmpfs mount;
this avoids stale system D-Bus pid/socket files across boots. Desktop-initiated
shutdown, reliable framebuffer capture from the standalone image, normal-image
integration, and aarch64 support remain to be verified.

### Interactive input

The session was display-only until the device manager was fixed: Xorg loaded
`libinput_drv.so`, but libinput enumerates input devices through libudev, and
the service tree only ran busybox `mdev`. `mdev` creates `/dev` nodes (devtmpfs
does that as well) but never builds the udev database, so libinput found no
device carrying `ID_INPUT_KEYBOARD` and the session had only the XTEST virtual
devices.

A `udev` service (`tools/install-services.sh`) now starts `systemd-udevd`,
reloads rules, triggers the input and USB subsystems, and settles before the
`xorg` service runs (`xorg` depends on `udev`). Targeting the input and USB
subsystems keeps start-up inside the service-start budget on emulated hardware;
a full device-tree trigger can exceed it. On rootfs trees without udev the
service reports "not installed" and stays out of the way, so the native busybox
image is unaffected.

`bctl`'s service start timeout was raised from 8 s to 25 s, because populating
the udev database under TCG emulation regularly exceeded the old budget and the
control script was killed mid-enumeration.

The stage harness now asserts the full chain (37 checks, all passing): the udev
daemon is running, `/dev/input/event*` exists, `udevadm info` reports `ID_INPUT`
for those devices, Xorg lists a real keyboard and pointer next to the XTEST
devices, Xorg loads `libinput`, and QMP-injected keystrokes arrive at the X
server as raw key events. The QEMU framebuffer screendump captured a rendered
Plasma desktop in the same run (`out/plasma-x86-input6-screen.png`).

## Verification criteria

* \[x] x86-64 standalone image boots into a KDE Plasma session as a regular user.
* \[ ] D-Bus, display server, graphics, input, and session lifecycle are managed
by BulumOS services. D-Bus, the display server, graphics and input are `bctl`
services and are verified; session lifecycle is not yet.
* \[ ] User accounts can be managed graphically through an authorized helper.
* \[ ] BulumOS services and packages are accessible from the Plasma session.
* \[ ] The same features pass on aarch64 after x86-64 validation.
