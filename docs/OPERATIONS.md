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

Capture a read-only platform snapshot with:

```bash
./scripts/gpu_status.sh
```

This reports generic devfreq nodes when present, then the target's
`/sys/devices/platform/mali.0` diagnostics, runtime power files, galcore
parameters, thermal zones, and exposed frequency/clock files. It performs no
writes. On the audited firmware there is no generic devfreq node and no
thermal-zone entry; the Mali driver reports a fixed policy and all three cores
available (`0x7`).

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

Input diagnostics include the seat index in `BIND_SEAT`, `SEAT_CAPS`,
`KEY`, `POINTER_ENTER`, and `POINTER_BUTTON`. `INPUT_DUPLICATE` records an
event intentionally suppressed by the shared multi-seat router.

The stress renderer is never selected by the normal installer. Install it only
for a controlled benchmark:

```bash
INCLUDE_STRESS=1 ./scripts/install_tv_lowspace.sh
```

Its `frame`, `swap`, `offscreen`, and `pbuffer` pacing modes are documented in
`docs/PERFORMANCE_AUDIT.md`. The pbuffer mode uses a surfaceless context when
the EGL extension is available. Always record `STRESS_SUMMARY`, the separate
CPU draw/query/swap/frame metrics, and `STRESS_GPU_MS` together; presented FPS
alone cannot distinguish compositor pacing from Mali throughput.

For repeatable machine-readable runs:

```bash
SWEEP=quick ./scripts/run_gpu_sweep.sh > sweep.jsonl
```

The benchmark supports `STRESS_GPU_TIMER=on|off`, `STRESS_TIMER_SLOTS`,
`STRESS_TEXTURE_SIZE`, `STRESS_TEXTURE_PATTERN`, `STRESS_OUTPUT=jsonl|tsv`, and
the benchmark-only `STRESS_CPU_AFFINITY`. Timer-disabled runs are CPU submission
controls and use periodic fallback synchronization; they are not completed-GPU
throughput measurements. Use `STRESS_REPEAT=3` with the sweep for independent
process launches, then aggregate with:

```bash
python3 scripts/summarize_gpu_sweep.py sweep.jsonl > sweep-summary.jsonl
```

The aggregation reports mean, standard deviation, minimum, and maximum for
workload FPS, GPU p50/p95/average, MPixel/s, and ns/pixel. Use
`SWEEP=production` for the bounded UI-oriented matrix, or `SWEEP=full` for the
larger exploration matrix.

After the phase-3.1 methodology corrections, rerun only the affected cases:

```bash
STRESS_REPEAT=3 SWEEP=phase31 ./scripts/run_gpu_sweep.sh > phase31.jsonl
python3 scripts/summarize_gpu_sweep.py phase31.jsonl > phase31-summary.jsonl
```

This matrix covers minimal/ALU/effect multipass, clean blur, command pressure,
equivalent-geometry sprites, and program switching. It does not rerun the
unrelated fill, SFU, or texture matrices.

Find a safe p95 complexity budget directly:

```bash
BUDGET_WORKLOAD=alu BUDGET_PRECISION=mediump \
  ./scripts/find_gpu_budget.sh

BUDGET_WORKLOAD=overdraw BUDGET_VALUES=1,2,4,8 \
  ./scripts/find_gpu_budget.sh
```

For opt-in normal-renderer software latency tracing, launch the normal binary
with `EGL_LATENCY_TRACE=1`. The log reports input-to-submit,
input-to-swap-return, input-to-frame-callback, and frame-callback interval
percentiles. It is not an input-to-photon measurement.

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
