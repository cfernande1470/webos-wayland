# TV operations and recovery

## Safety model

The scripts assume root SSH access to a development TV. They validate the
application ID before constructing local or remote removal paths.

Application shutdown is targeted by resolved executable path:

```text
/media/developer/apps/usr/palm/applications/<app-id>/bin/*
```

This avoids broad `killall` commands that could stop unrelated experiments
with the same binary name.

The installer only removes known files inside the selected application and its
application-specific temporary upload directory.

## First-time setup

1. Copy `.webos-sdk.env.example` to `.webos-sdk.env`.
2. Set the absolute `WEBOS_SDK` path.
3. Confirm passwordless or otherwise usable SSH access to the TV.
4. Build and inspect the ABI.

```bash
cp .webos-sdk.env.example .webos-sdk.env
./scripts/build.sh
file dist/org.webosbrew.wayland/bin/*
```

Expected executable ABI:

```text
ELF 32-bit LSB executable, ARM, EABI5
interpreter /lib/ld-linux.so.3
```

## Normal development cycle

```bash
./scripts/dev_cycle.sh
```

This performs:

```text
build -> targeted install -> SAM launch -> status/log output
```

The build emits `appinfo.json` and `packageinfo.json`. The installer updates the
matching application and package directories atomically so a subsequent SAM
scan has complete metadata.

The deployment directories are normalized to mode `755`. SAM launches these
native applications through `jailer` as the unprivileged `prisoner` user; a
directory without execute permission for that user makes an existing binary
appear as `No such file or directory`.

Override the TV without editing scripts:

```bash
TV=root@tv-address ./scripts/dev_cycle.sh
```

## Renderer selection

Normal GPU path:

```bash
RENDERER=wayland_egl ./scripts/install_tv_lowspace.sh
```

CPU fallback:

```bash
RENDERER=wayland_rect ./scripts/install_tv_lowspace.sh
```

4K stress path:

```bash
RENDERER=wayland_egl_stress INCLUDE_STRESS=1 \
  ./scripts/install_tv_lowspace.sh
```

Reinstall the normal path afterwards. A normal install removes the stress
binary from the application directory.

## SAM registration

Direct file installation does not notify an already running SAM process. The
installer writes the required package metadata, but the first install can still
require one SAM restart or TV reboot. Symptoms are:

- the Luna launch call returns no useful output;
- no process starts;
- no client log is created.

Reboot once after first installation:

```bash
./scripts/reboot_tv.sh
```

Do not reboot during firmware updates, storage repair, or other critical TV
operations.

`scripts/package_ipk.sh` creates a standard IPK for inspection or firmware that
accepts developer packages. The audited commercial firmware rejected the
unsigned IPK during verification, so IPK installation is not the deployment
fallback for this rooted target.

## Direct smoke test

A direct SSH run is useful for EGL and driver diagnostics, but it is not a
replacement for SAM lifecycle testing:

```bash
ssh root@192.168.2.121 '
APP=/media/developer/apps/usr/palm/applications/org.webosbrew.wayland
XDG_RUNTIME_DIR=/tmp/xdg \
WAYLAND_DISPLAY=wayland-0 \
APP_ID=org.webosbrew.wayland \
EGL_PLATFORM=wayland \
"$APP/bin/native_main"
'
```

Stop that foreground command with Ctrl-C. A direct surface may be briefly
visible or may remain behind the current foreground app.

## Logs and verification

```bash
./scripts/status_tv.sh
```

The GPU path is confirmed only when the log includes an ARM EGL vendor and a
Mali renderer. An OpenGL ES version string alone is not enough to rule out a
software implementation.

Useful remote checks:

```bash
ssh root@192.168.2.121 '
grep -E "EGL_|GL_RENDERER|GL_VERSION|CONFIGURE|EGL_FRAME" \
  /tmp/org.webosbrew.wayland.client.log
ls -l /dev/mali0 /dev/dri 2>/dev/null
df -h / /tmp /media/developer
uptime
'
```

Lifecycle-specific log markers are:

```text
WEBOS_SHELL_BOUND
WEBOS_SHELL_ATTACHED
WEBOS_SHELL_STATE
WEBOS_SHELL_EXPOSED
WEBOS_SHELL_VISIBILITY
WEBOS_SHELL_CLOSE
```

`WEBOS_SHELL_FALLBACK` means the renderer continued with core `wl_shell`.

The audited firmware requires an SSH pseudo-terminal for remote `luna-send`
calls. The launch, status, stop, and installer scripts therefore use `ssh -tt`
for commands that call Luna. They also recognize executable paths prefixed by
`/var/palm/jail/...` when reporting or stopping SAM-owned processes.

## Storage

On the audited TV, `/dev/root` was full while `/mnt/lg/appstore` still had about
400 MiB available. The installed normal app is roughly 70-90 KiB, so it is not
the source of root-filesystem pressure.

Build products are not stored in Git. The normal installer excludes the stress
binary, and temporary uploads are removed after installation.

Do not indiscriminately delete files from `/`, `/var`, or webOS application
partitions. Identify the owning package or service before cleanup.

## Failure modes

### `No such file or directory` for an existing binary

Inspect the requested interpreter with `readelf -l`. An AArch64 executable that
requests `/lib/ld-linux-aarch64.so.1` cannot run when that loader is absent from
the native app environment.

### `wl_display_connect` fails

Confirm:

```text
XDG_RUNTIME_DIR=/tmp/xdg
WAYLAND_DISPLAY=wayland-0
/tmp/xdg/wayland-0 exists
```

Also verify that the process user can access the socket.

### EGL initialization fails

Switch to `wayland_rect`. If the shared-memory client works, the Wayland/SAM
path is healthy and the problem is isolated to EGL, GLES, the native window, or
the Mali userspace driver.

### Low or unstable benchmark FPS

Record system load and competing applications. During the audit, another
Chromium renderer and a load average around 14-16 made whole-system comparisons
unreliable. Use per-process CPU counters and repeat tests under comparable load.

### Application cannot be stopped by process name

Use `scripts/stop_tv.sh`. Symlinked executables can have names such as `client`,
so process-name matching is deliberately not the control mechanism.
