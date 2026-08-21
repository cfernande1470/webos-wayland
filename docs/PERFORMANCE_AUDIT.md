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

## Phase 2 methodology audit

Before changing the benchmark, the first-phase measurement path was reviewed
for self-inflicted bias:

| Possible bias | Classification | Finding |
|---|---|---|
| Eight in-flight timer queries | Confirmed | The old offscreen path called `glFinish()` whenever all eight slots were active. This bounded queue depth but could add artificial bubbles. |
| Final `glFinish()` | Confirmed | The old summary drained the whole GPU before printing. It is now counted separately and is not part of per-frame GPU samples. |
| Timer query overhead | Probable | `glBeginQueryEXT`/`glEndQueryEXT` add driver work. Timer on/off and CPU query-call metrics are now independently measurable. |
| Window EGL surface in FBO mode | Probable | The old FBO still used a Wayland EGL window surface and context. A surfaceless/pbuffer comparison path is now available. |
| 256x256 texture working set | Confirmed/probable | Two 256x256 textures are only 0.5 MiB total and can be cache-friendly. Texture size and access pattern are now parameters. |
| Shader compiler folding/unrolling | Probable | Loop bounds are compile-time constants and the compiler may unroll or combine operations. All workload accumulators feed the final color, and compile/link failures are logged; binary disassembly is not exposed by this firmware. |
| Logging | Little relevance | Progress logging occurs every two seconds and is outside the draw hot path. Machine output is emitted only once at completion. |
| One-second warmup | Probable for thermals, low relevance for shader steady state | Warmup is configurable; long thermal runs must be requested explicitly. |
| Thermal/DVFS drift | Unverified/probable | The firmware has no generic devfreq or thermal-zone nodes. Mali policy and driver nodes are sampled before/during/after, but frequency is not directly observable. |
| Benchmark ordering and cache history | Probable | Results can depend on the previous workload. The sweep uses independent process launches; randomized order remains a recommended follow-up. |
| Wayland callback and compositor pacing | Confirmed for presentation modes | `frame` and `swap` measure presentation behavior, not the GPU ceiling. `pbuffer`/surfaceless removes that dependency. |

The phase-two implementation therefore separates completed-GPU timing from CPU
submission throughput, exposes timer depth and backpressure counters, and adds
a surfaceless EGL path. It deliberately does not claim that any single short
run is an absolute Mali limit.

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

The stress renderer now has four explicit pacing modes:

```text
STRESS_PACING=frame      surface presentation plus wl_surface_frame
STRESS_PACING=swap       surface presentation, eglSwapInterval(0), no frame callback
STRESS_PACING=offscreen  FBO rendering with no presentation per iteration
STRESS_PACING=pbuffer    surfaceless EGL context, or pbuffer fallback
```

`swap` is useful because it tests whether EGL/surface-manager still blocks. On
this TV it remained at approximately 59.3 FPS and showed approximately 16.6 ms
GPU query p50, so `eglSwapBuffers`/the webOS surface path remains paced even with
interval zero. `offscreen` is the useful GPU-throughput mode; it uses a bounded
number of GPU queries in flight and applies backpressure, preventing an
unbounded command queue from turning final `glFinish()` into a misleading hang.

The target exposes `GL_EXT_disjoint_timer_query`. The benchmark loads its entry
points dynamically, keeps a configurable query ring in flight, recycles the
oldest query when full without a blanket `glFinish()`, polls availability
without blocking, and discards disjoint samples. It reports CPU submit time and
GPU time separately with average, p50, p95, and p99 values. A short verified run
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

## Phase 2 implementation and hardware validation

The stress benchmark now supports:

```text
STRESS_PACING=frame|swap|offscreen|pbuffer
STRESS_WORKLOAD=fill|alu|sfu|bandwidth
STRESS_GPU_TIMER=on|off
STRESS_TIMER_SLOTS=1..128 (8/16/32/64 are the useful comparison points)
STRESS_TEXTURE_SIZE=64..8192, clamped to GL_MAX_TEXTURE_SIZE
STRESS_TEXTURE_PATTERN=coherent|stride|randomish
STRESS_OUTPUT=human|jsonl|tsv
STRESS_CPU_AFFINITY=<index> (opt-in)
```

`pbuffer` first attempts `EGL_KHR_surfaceless_context` and otherwise creates an
EGL pbuffer. On the target the surfaceless path works. It still renders into a
complete FBO, so the comparison is between a Wayland-window EGL context and a
surfaceless/pbuffer EGL context, not between different fragment workloads.

The timer ring is dynamically allocated. When it fills, the benchmark recycles
the oldest query by waiting for that query's result rather than calling
`glFinish()` for every ring wrap. `glFinish()` remains counted for the final
drain and for the timer-disabled fallback backpressure path. The summary logs
ring-full events, query waits, finishes, completed queries, and disjoint
discarded samples.

The benchmark now reports independent distributions for:

```text
STRESS_CPU_DRAW_MS
STRESS_CPU_QUERY_BEGIN_END_MS
STRESS_CPU_SWAP_MS
STRESS_CPU_FRAME_TOTAL_MS
STRESS_FRAME_CALLBACK_INTERVAL_MS
STRESS_GPU_MS
```

It also derives MPixel/s and ns/pixel for every workload, an estimated ALU
GFLOP/s based on the generated GLSL operation count, and texture samples/s and
an approximate 16-byte/sample read rate. These are model-derived values, not
Mali hardware counters; caches, compression, and compiler lowering mean that
the texture byte rate is not DRAM bandwidth.

### Live phase-two results

The following short runs were executed on the target's Mali-G51 3-core driver
(`Bifrost r9p0`, OpenGL ES 3.2). They are representative, not final thermal
limits:

| Backend/workload | Configuration | GPU p50 | Throughput | Notes |
|---|---|---:|---:|---|
| surfaceless fill | 1280x720 | 0.28 ms | ~3.00 Gpixel/s | 0.333 ns/pixel; minimal fragment path |
| surfaceless fill | 1920x1080 | 0.63 ms | ~3.17 Gpixel/s | fill-rate remains approximately resolution-linear |
| surfaceless fill | 3840x2160 | 2.50 ms | ~3.18 Gpixel/s | no compositor presentation involved |
| surfaceless ALU | 1920x1080, highp, 1 iter | 8.44 ms | ~109–117 workloads/s | timer slots 8, 32, and 64 produced similar GPU p50 |
| window-FBO ALU | 1920x1080, highp, 1 iter | 8.44 ms | ~109 workloads/s | within short-run variance of surfaceless |
| surfaceless texture | 1920x1080, mediump, 4 iter, 256/1024 | 10.51 ms | ~86 workloads/s | 0.5/8 MiB working sets were similar |
| surfaceless texture | 2048, coherent, 32 MiB | 10.80 ms | ~83 workloads/s | modest cache/working-set effect |
| surfaceless texture | 2048, randomish, 32 MiB | ~242 ms | ~1.4 workloads/s | large deterministic cache-stressing cost; 8 disjoint samples discarded |

The randomish pattern is intentionally deterministic and avoids trigonometric
functions, but it still adds arithmetic to generate coordinates. It should be
interpreted as a cache-hostile stress case, not as a pure DRAM bandwidth
counter. The large latency and disjoint results make it unsuitable as a normal
application model without longer repeated runs.

With timer queries enabled, 8 slots caused many oldest-query waits but did not
change GPU p50 relative to 32/64 slots. With timer queries disabled, the
benchmark correctly exposes a much higher CPU submission rate, but only by
periodically calling `glFinish()`; that mode measures command submission rather
than completed GPU throughput and is therefore a control experiment, not a
higher performance result.

The target exposes the following relevant EGL capabilities:

```text
EGL_KHR_surfaceless_context       yes
EGL_KHR_create_context            yes
EGL_KHR_fence_sync                yes
EGL_KHR_wait_sync                 yes
EGL_KHR_image / image_base        yes
EGL_ANDROID_native_fence_sync    no
EGL_EXT_image_dma_buf_import     no
EGL_EXT_image_dma_buf_import_modifiers no
partial-update/swap-damage/age   no
```

No readable Mali hardware-counter interface was found in the repository SDK,
the target's sysfs, `/sys/module/mali_kbase`, already-mounted debugfs, procfs,
or `/proc/modules`. The driver exposes `gpuinfo`, fixed core availability
(`0x7`), scheduling periods, power policy, memory-pool values, and runtime
power state, but not shader/tiler/L2/busy counters. `galcore` is not the active
Mali Bifrost driver for this target. A future counter implementation would need
the vendor kernel's private kbase ioctl/debug interface or an instrumented
firmware; none was safely discoverable here.

The expanded `scripts/gpu_status.sh` remains read-only. It inspects generic
devfreq, Mali modules, already-mounted debugfs, procfs references, counter-like
nodes, thermal zones, clocks, and optional filtered `dmesg` output when
`GPU_STATUS_DMESG=1`. It never mounts debugfs or writes a governor, frequency,
voltage, OPP, or core mask.

### Interpretation and remaining limits

- The minimum measured fill cost is approximately 0.31–0.33 ns/pixel, or
  roughly 3.1–3.2 Gpixel/s for this opaque single-render-target shader.
- The current ALU highp 1-iteration case is around 8.4 ms GPU time at 1080p;
  this is a workload ceiling, not the maximum FLOPS of the silicon.
- `mediump` remains beneficial for suitable local math, but precision changes
  must be validated per shader and visual result.
- Texture sizes up to 8 MiB per texture did not materially change the coherent
  case; the 32 MiB pair produced only a modest increase. The randomish case is
  the first clear cache-hostile transition, but it is confounded by coordinate
  generation and disjoint behavior. A true cache/DRAM boundary needs repeated,
  randomized-order runs and vendor counters.
- The surfaceless ALU result is close to the window-FBO result, so the FBO's
  Wayland EGL association is not currently a dominant cost for this workload.
- Presentation still loses throughput to webOS/surface-manager pacing; the
  earlier `frame` and `swap` results remain the correct evidence for that path.
- A long 30–60 second run is still needed to quantify thermal throttling because
  no readable frequency or temperature telemetry is exposed by this firmware.

`scripts/run_gpu_sweep.sh` provides bounded `SWEEP=quick` and `SWEEP=full`
matrices and emits JSONL summaries suitable for plotting or aggregation.
`STRESS_REPEAT=N` repeats each case in independent processes, and
`scripts/summarize_gpu_sweep.py` computes mean, standard deviation, minimum,
and maximum for the main throughput and timing fields. The sweep does not run
automatically during install.

## Phase 3: practical 60 Hz workload audit

Phase 3 changes the question from “how many synthetic shader iterations can
the GPU execute?” to “how much real application work fits in a stable 60 Hz
frame?”. The production renderer remains unchanged. All new loads are opt-in
stress-benchmark workloads or diagnostic scripts.

### Methodology audit

The following sources of bias were reviewed before adding workloads:

| Source | Classification | Consequence |
| --- | --- | --- |
| Wayland frame callback and `eglSwapBuffers` pacing | Confirmed | `frame`/`swap` measure presentation/backpressure, not the GPU ceiling. |
| Single fullscreen draw extrapolated to UI complexity | Confirmed | Previous ALU/fill numbers did not describe overdraw, blending, passes, or draw-call pressure. |
| Timer queries and ring backpressure | Confirmed and instrumented | Results now report slots, waits, ring-full events, finishes, completed queries, and disjoint discards. `STRESS_GPU_TIMER=off` is a CPU-control run, not a completed-GPU measurement. |
| Shader compiler removing unused work | Probable risk | Every stress result is folded into a color or consumes the previous pass/texture. The benchmark still cannot prove instruction-level execution without a Mali disassembler. |
| Warmup, run order, DVFS, and thermal state | Probable | Use `STRESS_REPEAT`, independent processes, and JSONL aggregation. No readable frequency/temperature counters were found, so long-run throttling remains unconfirmed. |
| Window surface versus GPU-only path | Low for tested workloads | Surfaceless/pbuffer and window-backed FBO results were very similar for the measured ALU case. |
| Logging perturbation | Low | Progress logging is periodic; machine-readable output is emitted once at shutdown. |
| ASTC “support” versus an actual compressed asset | Confirmed limitation | The TV advertises ASTC, but this repository has no encoder/assets; `STRESS_TEXTURE_FORMAT=astc` explicitly falls back to RGBA8888 and is not an ASTC performance result. |

### New benchmark controls

`wayland_egl_stress` includes `overdraw`, the corrected multipass variants,
`blur`, command-pressure, and equivalent-geometry sprite workloads. Important
controls are:

```text
STRESS_LAYERS=1,2,4,8,16
STRESS_BLEND=none|alpha|premultiplied|additive
STRESS_PASSES=1,2,4,8
STRESS_BLUR_TAPS=3|5|9
STRESS_DRAWS=1|10|100|500|1000
STRESS_PROGRAM_SWITCHES=0|1|10|100
STRESS_BATCH=0|1
STRESS_TEXTURE_FORMAT=rgba8888|rgb565|etc2|astc
STRESS_FILTER=nearest|linear|trilinear
STRESS_TEXTURE_SAMPLES=1|2|4|8|16
STRESS_TEXTURE_LAYOUT=separate|atlas
```

The ETC2/ASTC modes currently report the requested format but upload a
deliberate RGBA8888 fallback. RGB565 is a real upload path. `STRESS_BATCH=1`
is a command-pressure control (one draw instead of N); it is not yet a full
sprite/instance batching implementation, so it must not be read as a complete
UI batching result.

`STRESS_PACING=pbuffer` prefers `EGL_KHR_surfaceless_context` and falls back
to an EGL pbuffer. Multipass and blur require an offscreen FBO and therefore
use `offscreen` or `pbuffer`, not a presented window. JSONL now carries the
workload parameters, blend name, pass/layer/draw counts, program switches,
batch flag, texture controls, GPU percentiles, CPU sections, and timer
counters. `scripts/summarize_gpu_sweep.py` aggregates repeated runs and
includes standard deviation/min/max.

### Measurements on the target TV

These are short, representative 1080p surfaceless/pbuffer runs with timer
queries enabled. They are measured observations, not silicon specifications;
repeat them with the exact workload and thermal state needed for a release
decision.

| Workload | Configuration | GPU p50 (ms) | GPU p95 (ms) | Interpretation |
| --- | --- | ---: | ---: | --- |
| Overdraw | opaque, 1/2/4/8 layers | 0.63 / 0.92 / 1.52 / 2.72 | ~0.66 / 0.98 / 1.57 / 2.74 | Opaque layers scale predictably and remain cheap at eight layers. |
| Overdraw | alpha, 1/2/4/8 layers | 1.08 / 1.60 / 2.67 / 4.80 | approximately the same order | Fullscreen transparency costs materially more than opaque writes. |
| Overdraw | additive, 1/2/4/8 layers | 1.08 / 1.60 / 2.67 / 4.80 | approximately the same order | Additive blending is not free; it tracked alpha in this shader. |
| Multipass | 1/2/4/8 fullscreen passes | 7.17 / 14.35 / 28.67 / 57.10 | 7.23 / 14.45 / 28.72 / 57.18 | Fullscreen passes are close to linear and dominate quickly. |
| Blur | 2 passes, 3/5/9 taps | 15.24 / 15.98 / 18.30 | 15.31 / 16.02 / 18.37 | This simple blur is already near/over a 16.67 ms frame. |
| Draw calls | 1/100/500 draws, 1x1 control viewport | 0.09 / 0.68 / 2.87 | — | CPU submit overhead becomes visible before the GPU is saturated. |
| Draw calls | 500-draw command control with `STRESS_BATCH=1` | 0.09 | — | One draw removes the measured command overhead; this is not sprite batching. |
| Texture | 1/4/8 samples, 1024 RGBA, separate | 9.52 / 12.01 / 17.41 | — | Texture samples consume the 12–16 ms budget before extreme cache-hostile access. |
| Texture | 8 samples, linear RGB565 or atlas | ~17.37 / ~17.41 | — | No material gain was observed in this synthetic case. |

The earlier phase-two results remain important: fill is approximately
3.1–3.2 Gpixel/s (about 0.31–0.33 ns/pixel), ALU highp is about 8.44 ms at
1080p for the reference shader, coherent texture loads are about 10.5–10.8 ms,
and the randomish 2048 texture case reached about 242 ms with disjoint
samples. The latter is a cache-hostile stress result, not a DRAM bandwidth
counter.

### Practical 60 Hz envelope

For this TV, use the following engineering classes until application-specific
measurements justify a different budget:

| Class | GPU p95 target | Use |
| --- | ---: | --- |
| Conservative production | < 10 ms | Animated UI, input-heavy scenes, or uncertain thermal/compositor conditions. |
| Recommended production | < 12 ms | Normal 60 Hz application frame, leaving useful room for jitter and non-GPU work. |
| Absolute stress ceiling | < 16 ms | A measured upper bound below the 16.67 ms interval; too little headroom for a consistently smooth product. |

These are budgets, not claims that the compositor always consumes a fixed
number of milliseconds. `scripts/find_gpu_budget.sh` sweeps iterations,
layers, or passes and reports the largest tested value below each p95 limit:

```bash
BUDGET_WORKLOAD=alu BUDGET_PRECISION=mediump \
  BUDGET_VALUES=1,2,4,8,16,32,64 ./scripts/find_gpu_budget.sh
BUDGET_WORKLOAD=overdraw BUDGET_VALUES=1,2,4,8 \
  ./scripts/find_gpu_budget.sh
BUDGET_WORKLOAD=multipass BUDGET_VALUES=1,2,4,8 \
  ./scripts/find_gpu_budget.sh
```

The values are empirical thresholds for the selected shader and resolution;
do not generalize an ALU iteration count to an unrelated material.

### Answers and remaining hypotheses

- **How much real work fits at 1080p/60?** A single reference ALU pass or up to
  several opaque fullscreen layers fit comfortably, while two reference
  multipass passes are already around the recommended/absolute boundary.
  Real scenes must be measured with their shader, geometry, and texture set.
- **How much overdraw is acceptable?** Eight opaque layers were cheap in this
  test; eight alpha layers were about 4.8 ms. That is not permission to use
  eight fullscreen transparent layers everywhere because geometry, blending,
  and other passes consume the same budget.
- **How much does mediump help?** Phase two confirmed a material ALU gain;
  phase three keeps precision selectable for overdraw/texture/multipass but
  does not claim a universal speedup. Validate visual error per shader.
- **When is cache pressure visible?** Coherent textures through 2048 remained
  close; randomish 2048 was catastrophically slower. The exact cache/DRAM
  boundary cannot be identified without vendor counters.
- **Can we read Bifrost counters?** No safe public interface was found in
  sysfs, already-mounted debugfs, procfs, the SDK, or the target modules.
- **What is the optimal application strategy?** Keep surfaces opaque where
  possible, use mediump for safe local math, avoid unnecessary fullscreen
  passes and transparent overdraw, batch real UI geometry, reuse textures and
  programs, render only when content changes, and reserve p95 headroom for
  compositor and thermal variance.

The opt-in `EGL_LATENCY_TRACE=1` path in the normal renderer records software
timestamps for input-to-submit, input-to-swap-return, input-to-frame-callback,
and callback jitter. It is not input-to-photon measurement, and launcher
environment propagation must be verified when collecting it through SAM.

## Phase 3.1: methodology corrections and affected reruns

Phase 3.1 was intentionally narrow. It corrected only biases found in the
phase-3 multipass, blur, batching, and program-switch measurements; no new
production renderer path or unrelated GPU workload was added.

### Corrections

- `multipass_copy` now contains one texture read and one render-target write,
  with a small temporal output perturbation so repeated identical RGBA8 tiles
  cannot make the pass look artificially free through transaction elimination.
- `multipass_alu` adds explicit lightweight ALU to that same dependency.
- The previous shader is now named `multipass_effect`; it retains the generic
  grid/cursor/theme/transcendental decoration and is documented as an effect
  workload, not a structural pass baseline.
- `blur` now performs only horizontal/vertical texture taps and weighted
  accumulation for 3, 5, or 9 taps.
- Both ping-pong targets are cleared and completed with `glFinish()` before
  `start_sec`/warmup. Source and target IDs persist across frames, so a
  one-pass run alternates valid A→B and B→A inputs.
- The timer query starts immediately before the pass loop and ends immediately
  after it. It includes per-pass FBO attachment, texture binding, and draw
  commands, but excludes target initialization, warmup, logging, and cleanup.
- `drawcalls` is now reported as `command_pressure`. It remains the 1x1
  viewport command-overhead control. A separate `sprites` workload constructs
  equivalent quad geometry once and compares N draws with one batched draw.
- Every GL program has its own attribute/uniform location set. Alternate
  program switching no longer reuses locations from the primary program.
- Atlas texture setup generates one texture in atlas mode; it no longer leaks a
  second generated name that is overwritten.

### Corrected hardware reruns

The following results are the mean of three independent 1080p surfaceless runs
(`STRESS_DURATION_MS=800`, 200 ms warmup). The summary also records min/max and
standard deviation in `/tmp/phase31-final-summary.jsonl`; p95 values below are
the mean of each run's p95.

| Workload | Configuration | GPU p50 (ms) | GPU p95 (ms) | CPU draw avg (ms) | CPU total avg (ms) |
| --- | --- | ---: | ---: | ---: | ---: |
| `multipass_copy` | 1 / 2 / 4 / 8 passes | 0.98 / 1.98 / 3.98 / 7.98 | 1.02 / 2.01 / 4.02 / 8.05 | 0.11 / 1.06 / 3.04 / 7.03 | 0.13 / 1.09 / 3.07 / 7.07 |
| `multipass_alu` | 1 / 2 / 4 / 8 passes | 1.86 / 3.73 / 7.48 / 14.98 | 1.93 / 3.87 / 7.72 / 15.12 | 0.11 / 1.94 / 5.72 / 13.21 | 0.13 / 1.98 / 5.76 / 13.25 |
| `multipass_effect` | 1 / 2 / 4 / 8 passes | 7.11 / 14.26 / 28.52 / 56.98 | 7.18 / 14.32 / 28.59 / 57.03 | 0.20 / 7.26 / 21.60 / 49.97 | 0.24 / 7.31 / 21.65 / 50.02 |
| `blur` | 3 / 5 / 9 taps, two passes | 3.21 / 5.33 / 9.59 | 3.26 / 5.40 / 9.63 | 1.69 / 2.77 / 4.90 | 1.72 / 2.81 / 4.95 |
| `command_pressure` | 100 / 500 / 1000 draws | 0.64 / 2.96 / 5.82 | 0.79 / 3.34 / 6.42 | 0.56 / 2.71 / 5.44 | 0.57 / 2.74 / 5.48 |
| `sprites` unbatched | 100 / 500 / 1000 quads | 1.23 / 2.50 / 4.89 | 1.25 / 2.83 / 5.45 | 0.49 / 2.33 / 4.63 | 0.51 / 2.35 / 4.67 |
| `sprites` batched | 100 / 500 / 1000 quads | 1.22 / 1.27 / 1.29 | 1.26 / 1.29 / 1.32 | 0.03 / 0.04 / 0.03 | 0.05 / 0.07 / 0.05 |

Program switching was measured with 1000 command-pressure draws. The mean
GPU p50/p95 was approximately 5.82/6.42 ms with zero switches,
5.70/6.06 ms with 10 switches, and 6.01/7.06 ms with 100 switches. The
variation is comparable to run-to-run noise for this short sweep; there is no
evidence here for a large standalone program-switch penalty, but the corrected
per-program locations remove a correctness risk.

### Old versus corrected conclusions

| Phase-3 statement | Phase-3.1 interpretation |
| --- | --- |
| “Two fullscreen passes are about 14.3 ms.” | True for the retained `multipass_effect` shader. A minimal copy pass is about 2.0 ms for two passes; texture+ALU is about 3.7 ms. |
| “Blur 5 taps/two passes is about 16 ms.” | That included the generic visual shader. Clean blur is about 5.3 ms at 5 taps and 9.6 ms at 9 taps. |
| “500 draws versus one draw demonstrates batching.” | The old test was command pressure only. Equivalent-geometry `sprites` now measures the real batching benefit: CPU draw time falls from about 2.33 ms to 0.04 ms at 500 quads. |

Therefore the earlier conservative production rule remains valid, but its
justification is more precise: two minimal copy passes are inexpensive, while
two passes containing meaningful ALU/effect work can approach or exceed the
12–16 ms budget. A clean 9-tap two-pass blur remains below 10 ms p95 in this
synthetic test, but application kernels, blending, and composition still need
their own margin.

The JSONL/TSV schema now includes `blur_taps`, `sprites`, CPU summary fields,
and the existing per-program `program_switches`. Use the affected-only matrix:

```bash
STRESS_REPEAT=3 SWEEP=phase31 \
  SWEEP_DURATION_MS=2500 SWEEP_WARMUP_MS=500 \
  ./scripts/run_gpu_sweep.sh > phase31.jsonl
python3 scripts/summarize_gpu_sweep.py phase31.jsonl > phase31-summary.jsonl
```

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
