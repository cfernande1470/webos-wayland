# Performance and code audit

Audit date: 2026-08-21

Target: rooted LG webOS TV at the development address configured by the project.

## Scope

The audit covered:

- C renderer and launcher source;
- cross-build flags and target ABI;
- install, launch, stop, and diagnostic scripts;
- repository hygiene and generated artifacts;
- live Wayland, EGL, GLES, Mali, memory, CPU, and filesystem state;
- controlled direct execution of the normal renderer;
- behavior before and after the changes documented below.

No DRM/KMS or fbdev write tests were performed during this audit. Previous
fbdev results were reviewed from the repository.

## Baseline findings

The original normal renderer already used hardware acceleration. Runtime proof
included:

```text
open file descriptor -> /dev/mali0
EGL_VENDOR            -> ARM
GL_RENDERER            -> Mali-G51
GL_VERSION             -> OpenGL ES 3.2 driver
```

The compositor configured a 1920x1080 surface. Over a stable five-second
window, the client produced 300 frames in 5.03 seconds, or 59.6 fps.

The client used about 0.40 CPU seconds over that interval: approximately 8% of
one CPU core, or 2.7% of the TV's three online cores. Resident memory was about
12.9 MiB. These values include Mali userspace driver threads.

The system was under significant unrelated load, so the audit relies on
per-process counters rather than total CPU or load average.

## Graphics changes

### EGL configuration enumeration

The original normal renderer required `EGL_ALPHA_SIZE=8` and accepted the first
matching config. The stress renderer tried an alpha config before its fallback,
and omission of `EGL_ALPHA_SIZE` did not guarantee a config without alpha.

Both EGL clients now enumerate configs and prefer a non-alpha RGB888 config
when one exists. The selected target config is logged.

Live validation selected:

```text
EGL_CONFIG id=1 rgba=8/8/8/8 depth=0 stencil=0
```

This confirms that the target stack does not provide a higher-scoring
window-capable RGB888 config without alpha. The fallback is therefore expected,
not a selection bug.

### Opaque surface region

All three renderers now mark the full surface opaque and update the region on
resize. The content is genuinely opaque: EGL fragment shaders output alpha 1.0
and the shared-memory renderer uses XRGB8888.

Wayland defines the opaque region as a compositor optimization hint. It can
reduce blending and redraw work behind the surface, but compositor policy,
other overlays, buffer format, and scanout hardware determine the actual gain.

Reference:

- <https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface-request-set_opaque_region>

### Frame time

The normal renderer previously advanced animation by an assumed 16 ms per
frame. It now uses the Wayland callback timestamp. The stress renderer now uses
`CLOCK_MONOTONIC` instead of wall-clock time for both animation and statistics.

### Post-change result

```text
Process name:             client
Mali descriptors:         1
Seats bound:              1
Keyboard focus events:    1
Resolution:               1920x1080
Frames:                   300 / 5.03 seconds
Frame rate:               59.6 fps
CPU:                      about 7.8% of one core
```

The changes preserve frame rate and slightly reduce measured CPU time within
normal run-to-run variance. The important verified improvements are correct
input binding, stable launcher identity, and the compositor opacity hint.

## Mali benchmark and bottleneck separation

The original stress renderer had `STRESS_SWAP_INTERVAL=0`, but that did not
remove its `wl_surface_frame` dependency: `render()` installed a frame callback
and `frame_done()` scheduled every subsequent render. It therefore measured a
compositor-paced workload, not the GPU ceiling.

The stress renderer now has three explicit pacing modes:

```text
STRESS_PACING=frame      surface presentation plus wl_surface_frame
STRESS_PACING=swap       surface presentation, eglSwapInterval(0), no frame callback
STRESS_PACING=offscreen  FBO rendering with no presentation per iteration
```

`swap` is useful because it tests whether EGL/surface-manager still blocks. On
this TV it remained at approximately 59.3 FPS and showed approximately 16.6 ms
GPU query p50, so `eglSwapBuffers`/the webOS surface path remains paced even with
interval zero. `offscreen` is the useful GPU-throughput mode; it uses a bounded
number of GPU queries in flight and applies backpressure, preventing an
unbounded command queue from turning final `glFinish()` into a misleading hang.

The target exposes `GL_EXT_disjoint_timer_query`. The benchmark loads its entry
points dynamically, keeps eight queries in flight, polls availability without
blocking, and discards disjoint samples. It reports CPU submit time and GPU
time separately with average, p50, p95, and p99 values. A short verified run
at 1920x1080, ALU workload, one iteration produced:

```text
frame:    59.32 workload FPS, GPU p50 10.60 ms, CPU submit p50 0.22 ms
swap:     59.29 workload FPS, GPU p50 16.62 ms, CPU submit p50 15.59 ms
offscreen:117.07 workload FPS, GPU p50  8.38 ms, CPU submit p50 0.04 ms
```

These values show that the normal 60 FPS result is not proof of GPU saturation:
the compositor and EGL queue constrain presentation, while offscreen rendering
exposes additional GPU headroom for this particular shader.

The shader is now parameterized and separated into workloads:

```text
STRESS_WORKLOAD=alu        vector multiply/add/dot operations
STRESS_WORKLOAD=sfu        sin/cos/sqrt/inversesqrt/pow
STRESS_WORKLOAD=bandwidth  two repeat-sampled RGBA textures
STRESS_ITERS=1|2|4|8|16|32|64
STRESS_PRECISION=highp|mediump|auto
STRESS_RESOLUTION=1080p|1440p|4k
```

At 1920x1080, 16 ALU iterations measured GPU p50 approximately 30.15 ms in
highp and 21.29 ms in mediump. At 3840x2160 and four iterations, the measured
p50 was approximately 49.64 ms highp versus 40.50 ms mediump. This is a
measured driver/compiler result, not an assumption that all application math
can safely be reduced to FP16. Coordinate and accumulation precision must still
be evaluated per workload.

At 1920x1080, mediump and 16 iterations measured GPU p50 approximately 21.33 ms
for ALU, 48.58 ms for SFU, and 31.87 ms for the texture workload. These are
synthetic shader workloads; they identify sensitivity to ALU, special-function,
and texture paths but do not predict an application's exact frame time.

The benchmark also supports:

```text
EGL_COLOR_MODE=auto|8888|rgb888|565
EGL_CONTEXT_PRIORITY=default|high
STRESS_DURATION_MS=...       (default 10000)
STRESS_WARMUP_MS=...         (default 1000)
STRESS_OFFSCREEN_BATCH=...   fallback backpressure when timer queries are absent
```

On the target, `EGL_IMG_context_priority` is available and high priority is
accepted (`actual=0x3101`), but the short comparison showed no meaningful GPU
improvement. It changes scheduling priority, not shader-core FLOPS, and remains
opt-in. RGB565 is window-capable (`EGL_CONFIG id=5`, 5/6/5/0), while a separate
alpha-free RGB888 config was not available. RGB565 remains experimental because
the offscreen target does not measure compositor conversion, banding, or final
presentation quality.

The runtime logs full EGL and GL extension strings. The target supports
`GL_EXT_disjoint_timer_query`, `GL_OES_vertex_array_object`, ASTC, and several
Mali/ARM extensions. It does not advertise `EGL_KHR_partial_update`, either
swap-with-damage extension, or buffer age. No partial-damage path was added:
there is no safe extension combination to activate on this firmware, and the
normal renderer's full clear/animation would make an unsafe damage path worse
than a conservative full update.

The cross compiler reports ARMv7-A, Cortex-A9 defaults, NEON FP16, and
`-mfloat-abi=softfp`, while the TV's CPUs identify as ARM Cortex-A55. The
default ABI is retained for compatibility. `scripts/build.sh` accepts
`EXTRA_CFLAGS` for explicitly separated experiments, but no `-mcpu=cortex-a55`,
`-O3`, `-ffast-math`, LTO, or hard-float flags are enabled by default.

The firmware has no `/sys/class/devfreq` node and no readable thermal-zone
entries. The Mali platform driver does expose `gpuinfo`, a fixed core policy,
core mask `0x7` for all three job slots, scheduling periods, and memory-pool
values. `scripts/gpu_status.sh` collects these read-only values and must be run
before/during/after a benchmark; it never changes governors, clocks, voltage, or
core masks.

## Additional hot-path audit

The render loops do not call `glFinish`, `glReadPixels`, or `glFlush` per frame,
do not upload buffers or recreate programs/textures, and do not allocate in the
normal frame path. Wayland dispatch is used for lifecycle/input events; there
are no per-frame roundtrips. The normal renderer has one static VBO and one
program. Its repeated viewport/attribute setup costs are small compared with a
full-screen fragment workload, and the measured CPU submit cost is low.

The stress renderer intentionally keeps the full-screen triangle and repeated
uniform/state setup visible so its CPU submit metric remains honest. VAOs are
available (`GL_OES_vertex_array_object`), but adding a VAO to a single-draw
benchmark would measure a tiny driver-state difference rather than improve the
normal application's GPU throughput. It remains a possible microbenchmark, not
a production change.

The current shaders have one fullscreen primitive, no blending, no depth or
stencil attachment, and an opaque output. This is favorable for a tile-based
Mali path. Direct scanout cannot be inferred from Wayland/EGL client behavior;
surface-manager policy, format, fullscreen state, and other overlays decide it.
The opaque-region hint is retained, but no unsupported webOS protocol or DRM
path is assumed to force scanout.

No production busy loop, aggressive swap mode, high context priority, RGB565,
partial damage, compiler fast-math, or overclocking change was enabled. These
experiments either belong exclusively to the stress binary or are unavailable
on the audited firmware.

## webOS shell lifecycle integration

The target advertises `wl_webos_shell` version 1. The renderers now bind the
official SDK interface while retaining the core `wl_shell` fallback. Live
hardware validation produced:

```text
WEBOS_SHELL_BOUND advertised=1 bound=1
WEBOS_SHELL_ATTACHED version=1 app_id=org.webosbrew.wayland
WEBOS_SHELL_STATE state=3
WEBOS_SHELL_EXPOSED rectangles=1 visible=1
```

A SAM-managed lifecycle test opened HDMI and moved the renderer to minimized
state. It logged `WEBOS_SHELL_VISIBILITY visible=0`, lost keyboard focus,
remained alive, and stopped at frame 2580 for the entire observation interval.
This verifies that obscured rendering is suspended by compositor lifecycle
events rather than by polling or a timer.

The integration also requests the default webOS key mask plus Back and Exit,
and maps the protocol close event to a clean event-loop exit. Foreground and
close behavior was tested through SAM: relaunching returned the app to
fullscreen state with keyboard focus, and `closeByAppId` removed every jailed
application process. This firmware started a fresh native instance when the
minimized app was launched again rather than resuming the old one.

## Launcher and deployment changes

The original launcher tried `android_backend` before `client` for every app ID.
Because SCP follows symlinks, an installer could turn `android_backend` into a
regular copy of a renderer. That copy then intercepted every renderer switch.
The process also appeared as `android_backend`, so name-based stop scripts could
miss it.

The corrected behavior is:

- normal Wayland app: `client`, then direct EGL and SHM fallbacks;
- Android app: `android_backend`, then the normal fallback chain;
- normal install: no `android_backend` file;
- Android install: create the backend symlink on the TV;
- stop/install: identify processes by resolved executable path.

The stress renderer is excluded from normal installation and cannot be selected
without `INCLUDE_STRESS=1`.

## Input changes

The TV advertises three version-3 `wl_seat` globals. Binding only the first seat
avoided duplicate focus events but failed the physical Magic Remote test: Back
was handled by webOS, while pointer motion and clicks never reached the client.
Live diagnostics showed that the Magic Remote uses seat index 1.

All renderers now use a shared multi-seat input router. It owns each seat's
pointer and keyboard independently, aggregates focus, deduplicates identical
key/button events within 20 ms, handles capability removal, and destroys proxies
using requests valid for their advertised protocol versions. Hardware testing
confirmed pointer entry and BTN_LEFT events on seat 1; three clicks moved the
triangle to three distinct offsets without duplicate actions.

## Build and repository changes

- `APP_ID` and renderer values are validated before path construction is used
  for deletion or installation.
- Strip failures now fail the build.
- Build version and default renderer are configurable.
- Generated binaries, `dist/`, snapshots, backup sources, and local SDK paths
  are no longer tracked.
- `.webos-sdk.env.example` documents local configuration without committing a
  machine-specific path.
- Legacy installer entry points delegate to one canonical implementation.
- Builds now generate package metadata required by the SAM developer tree.
- A standard, stress-opt-in IPK packager is available for inspection and
  compatible developer firmware.
- `egl_diagnostics.c` records complete EGL/GL strings and extension capability
  flags for both normal and stress EGL clients.
- `scripts/gpu_status.sh` provides read-only Mali/devfreq/thermal diagnostics.
- `EXTRA_CFLAGS` enables explicitly separated compiler experiments while
  preserving the SDK's ARMv7-A softfp defaults.
- Deployment normalizes directory traversal permissions for SAM's jailed
  `prisoner` user and recognizes jail-prefixed executable paths.
- Remote Luna operations allocate the pseudo-terminal required by the audited
  firmware.
- Shell scripts pass `bash -n`.
- GCC 12.2 `-fanalyzer` reports no findings.
- The renderer sources compile cleanly under the project's normal warning set.

## Remaining opportunities

### Event-driven static rendering

The normal demo intentionally rotates continuously. A real mostly-static UI
should request frames only while an animation is active or after content/input
changes. This can save more power than shader micro-optimization.

### Video

Do not implement video by sampling decoded 4K frames in a heavy general-purpose
fragment shader. Use the webOS/GStreamer media pipeline so dedicated decode and
overlay hardware can be used.

### Instrumentation

For larger applications, add:

- frame-time percentiles rather than average FPS only;
- EGL extension and swap-limit logging;
- optional GPU/job counters when the firmware exposes a stable interface;
- idle, foreground, and obscured power measurements;
- repeatable cold-start and memory-growth tests.

## Environment risks outside this repository

At audit time:

```text
/dev/root:        100% used
/mnt/lg/appstore: about 400 MiB free
load average:     approximately 14-16
```

The installed normal application is under 100 KiB and is not responsible for
root filesystem exhaustion. Root storage should be investigated separately and
carefully because a full root filesystem can cause unrelated services, logging,
updates, and application registration to fail.

## Licensing

The public repository currently has no license. Selecting a license is an owner
decision and was not automated as part of the technical changes.
