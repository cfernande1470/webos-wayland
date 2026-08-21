# webos-wayland

Native Wayland, EGL, and OpenGL ES experiments for rooted LG webOS TVs.

The project creates a real foreground Wayland surface through the webOS System
Application Manager (SAM), receives LG remote and Magic Remote input, and can
render with either the Mali GPU or a shared-memory CPU fallback. It does not use
Qt, SDL, X11, Xwayland, Chromium, Electron, or a browser UI.

## Confirmed graphics path

The normal renderer has been verified on the target TV as:

```text
webOS SAM
  -> surface-manager (Wayland compositor)
  -> wl_egl_window
  -> EGL 1.4 (ARM)
  -> OpenGL ES
  -> /dev/mali0
  -> Mali-G51
```

Observed driver information:

```text
EGL_VERSION 1.4
EGL_VENDOR ARM
EGL_CLIENT_APIS OpenGL_ES
GL_VENDOR ARM
GL_RENDERER Mali-G51
GL_VERSION OpenGL ES 3.2 v1.r9p0-01rel0.5fa3737ac24396a69b1d512b70e9e31d
```

The application requests an OpenGL ES 2 context for compatibility. A driver
that reports OpenGL ES 3.2 does not make an ES 3 context inherently faster for
this simple renderer.

## Current target

```text
TV OS:             webOS TV 6.2.0
Kernel:            Linux 4.4.84, aarch64
SoC:               LG1212
GPU:               Mali-G51, 3 shader cores
Application ABI:   ARM 32-bit EABI5
Dynamic loader:    /lib/ld-linux.so.3
Compositor:        /usr/bin/surface-manager
Wayland socket:    /tmp/xdg/wayland-0
UI resolution:     1920x1080
```

The 32-bit ABI is intentional. The target's native application environment
provides the ARM 32-bit loader and matching EGL/GLES libraries. The 64-bit
kernel does not imply that a 64-bit application binary can run in that
environment.

## Renderers

### `wayland_egl`

The production/default renderer. It uses `wl_egl_window`, EGL, and OpenGL ES.

Important behavior:

- selects a window-capable, non-slow EGL config with RGB888 or better;
- prefers a config without alpha when one exists;
- marks the full Wayland surface as opaque;
- uses `eglSwapInterval(1)` and Wayland frame callbacks;
- derives animation time from compositor callback timestamps;
- binds only the first advertised `wl_seat` to avoid duplicate input;
- binds `wl_webos_shell` when version 1 or newer is advertised;
- pauses frame production while the surface is minimized or fully obscured;
- handles compositor close requests and requests Back/Exit key delivery;
- falls back to the configured RGBA8888 surface when the Mali stack provides
  no compatible RGB888 config without alpha.

### `wayland_rect`

CPU fallback and Wayland diagnostic renderer. It uses three reusable
`WL_SHM_FORMAT_XRGB8888` buffers and marks the surface opaque. It is useful for
isolating EGL/driver problems, not for efficient full-screen animation.

### `wayland_egl_stress`

An opt-in 3840x2160 fragment-shader benchmark. It is never installed or
selected automatically. It must be explicitly requested because it is a test
load, not an application renderer.

## Repository layout

```text
native/
  native_main.c              SAM entry point and renderer launcher
  webos_shell.c/.h           shared webOS shell lifecycle integration
  wayland_egl.c              normal GPU renderer
  wayland_egl_stress.c       opt-in 4K stress renderer
  wayland_rect.c             wl_shm CPU fallback

scripts/
  build.sh                   cross-build and ABI checks
  package_ipk.sh             optional standard IPK packaging
  install_tv_lowspace.sh     canonical installer
  launch_tv.sh               SAM launch and diagnostics
  stop_tv.sh                 targeted application shutdown
  status_tv.sh               process and log status
  dev_cycle.sh               build, install, and launch

docs/
  ARCHITECTURE.md
  OPERATIONS.md
  PERFORMANCE_AUDIT.md

dist/                        generated, ignored build output
```

Build outputs, local SDK configuration, snapshots, and temporary source backups
are intentionally excluded from Git.

## Toolchain setup

The known working toolchain is:

```text
arm-webos-linux-gnueabi_sdk-buildroot
GCC 12.2.0
```

Create the ignored local environment file from the example:

```bash
cp .webos-sdk.env.example .webos-sdk.env
```

Edit it so `WEBOS_SDK` points to the SDK root:

```bash
export WEBOS_SDK="/absolute/path/to/arm-webos-linux-gnueabi_sdk-buildroot"
```

## Build

```bash
./scripts/build.sh
```

The build fails if it produces AArch64 binaries or if the main binaries are not
ARM 32-bit EABI executables.

It also generates both `appinfo.json` and `packageinfo.json`. The latter is
required for the developer application tree to be discoverable as a package by
SAM.

Supported build variables:

```text
APP_ID             default: org.webosbrew.wayland
APP_TITLE          default: Wayland EGL Native Lab
APP_VERSION        default: 0.1.0
DEFAULT_RENDERER   default: wayland_egl
```

`APP_ID` is validated before any output directory is removed. It may only
contain letters, numbers, dots, underscores, and hyphens.

Create a standard unsigned development IPK with:

```bash
./scripts/package_ipk.sh
```

`INCLUDE_STRESS=1` includes the stress renderer in the IPK. Commercial TV
firmware normally rejects unsigned IPKs; the low-space SSH installer remains
the supported rooted-TV deployment path.

## Install

The canonical normal installation is:

```bash
./scripts/install_tv_lowspace.sh
```

Equivalent compatibility entry points are `install_tv.sh`,
`install_tv_safe.sh`, and `install_tv_atomic.sh`; they delegate to the canonical
installer.

Supported installation variables:

```text
TV               default: root@192.168.2.121
APP_ID           default: org.webosbrew.wayland
RENDERER          wayland_egl or wayland_rect; default: wayland_egl
INCLUDE_STRESS    0 or 1; default: 0
```

Install the CPU fallback:

```bash
RENDERER=wayland_rect ./scripts/install_tv_lowspace.sh
```

Install and explicitly select the stress client:

```bash
RENDERER=wayland_egl_stress INCLUDE_STRESS=1 \
  ./scripts/install_tv_lowspace.sh
```

The installer stops only processes whose executable resolves inside the target
application directory. It does not use broad `killall` commands.

The installer atomically updates application files and writes the matching
package metadata under `/media/developer/apps/usr/palm/packages/<app-id>`.

## Register and launch

An app added directly to the developer tree may not be known to an already
running SAM instance until it rescans its metadata. Restart SAM or reboot once
after the first installation if launch returns without creating a process:

```bash
./scripts/reboot_tv.sh
```

Then launch through SAM:

```bash
./scripts/launch_tv.sh
```

Launching through SAM is important. A client started directly over SSH can
connect to Wayland and render, but it does not receive the normal foreground
application lifecycle.

## Status and stop

```bash
./scripts/status_tv.sh
./scripts/stop_tv.sh
```

Logs are written to:

```text
/tmp/<app-id>.native_main.log
/tmp/<app-id>.client.log
```

Confirm GPU use with:

```bash
ssh root@192.168.2.121 \
  'grep -E "EGL_VERSION|EGL_VENDOR|EGL_CONFIG|GL_RENDERER|GL_VERSION" \
   /tmp/org.webosbrew.wayland.client.log'
```

Confirm the webOS lifecycle extension with:

```bash
ssh root@192.168.2.121 \
  'grep -E "WEBOS_SHELL_(BOUND|ATTACHED|STATE|EXPOSED|VISIBILITY|CLOSE)" \
   /tmp/org.webosbrew.wayland.client.log'
```

## Android launcher experiment

The same shell can build `org.webosbrew.android`:

```bash
./scripts/build_android.sh
./scripts/install_tv_android.sh
./scripts/launch_tv_android.sh
```

Only this application ID attempts `bin/android_backend`. The normal Wayland
application always starts `bin/client` first, so renderer switching cannot be
intercepted by the Android path.

## Measured performance

The optimized normal renderer was measured on the target TV on 2026-08-21:

```text
Resolution:                 1920x1080
Frames over sample:         300 frames / 5.03 seconds
Frame rate:                 59.6 fps
Client CPU:                 about 7.8% of one CPU core
Client resident memory:     about 12.9 MiB
Mali device descriptors:    1
Input seats bound:          1
Selected EGL config:        RGBA8888, depth 0, stencil 0
```

The driver did not expose a suitable RGB888 window config without alpha, so the
client uses RGBA8888 and supplies the compositor with a full-surface opaque
region. See [the performance audit](docs/PERFORMANCE_AUDIT.md) for context.

## Why not DRM, fbdev, or forced 4K UI rendering?

The target exposes `/dev/mali0` but no usable `/dev/dri/card*` node. The
proprietary Mali userspace stack is integrated with Wayland/EGL. Direct fbdev
writes compete with `surface-manager`, cause visible artifacts, and do not
provide OpenGL ES acceleration.

The webOS UI compositor runs at 1920x1080 on this TV. Forcing a 3840x2160 render
target quadruples fragment work and buffer bandwidth without improving the
compositor's native UI resolution. Video playback is different: it should use
the platform media pipeline and dedicated video decode hardware.

The original fbdev experiment is retained in
[fbdev_readme_section.md](fbdev_readme_section.md).

## Further documentation

- [Architecture](docs/ARCHITECTURE.md)
- [TV operations and recovery](docs/OPERATIONS.md)
- [Performance and code audit](docs/PERFORMANCE_AUDIT.md)

## License

No project license has been selected yet. Add one before expecting third parties
to copy, modify, or redistribute the project.
