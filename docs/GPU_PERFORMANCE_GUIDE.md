# webOS GPU performance guide

This guide turns the benchmark results into practical rules for applications
running on the audited LG webOS 6.2 TV. Values below are synthetic measurements
on one LG1212/Mali-G51 device and are not universal Mali limits.

## Hardware measured

- Mali-G51, three shader cores, Bifrost r9p0-class driver.
- OpenGL ES 3.2 exposed through ARM EGL 1.4.
- 1920x1080 compositor/presentation path, approximately 60 Hz.
- `GL_EXT_disjoint_timer_query` available.
- Surfaceless EGL context available.
- No readable generic devfreq, thermal-zone, or Bifrost performance-counter
  interface was found.

## Measured baseline at 1080p

| Operation | Observed GPU p50 | Interpretation |
|---|---:|---|
| Minimal opaque fill | ~0.63 ms | ~3.1–3.2 Gpixel/s effective fill rate |
| ALU, highp, one iteration | ~8.4 ms | Fragment ALU workload, not silicon FLOPS maximum |
| Texture, 1 sample pair | ~9.5–10.5 ms | Depends on filter, precision, working set |
| Opaque overdraw x2 | ~0.9 ms | Tile-based renderer retains useful locality |
| Alpha overdraw x4 | ~2.7 ms | Blending costs materially more than opaque fill |
| Minimal copy pass, 1 pass | ~0.98 ms | Structural texture-read plus render-target-write cost |
| Minimal copy pass, 2 passes | ~1.98 ms | Approximately linear ping-pong cost |
| Lightweight texture+ALU pass, 2 passes | ~3.73 ms | Shader arithmetic adds a measurable but bounded cost |
| Effect shader, 2 passes | ~14.3 ms | The old synthetic visual shader, not a minimum pass |
| Clean blur, 3/5/9 taps, 2 passes | ~3.21 / 5.33 / 9.59 ms | Simple weighted accumulation without visual decoration |
| 500 command-pressure draws | ~3.0 ms GPU, ~2.7 ms CPU draw | CPU/driver overhead is visible |
| 500 equivalent sprites, one batched draw | ~1.27 ms GPU, ~0.04 ms CPU draw | Real geometry batching removes submit overhead |

The exact values vary with thermal state, background load, timer depth, and run
ordering. Always compare p95, not just p50.

## Practical 60 Hz envelope

The absolute refresh interval is 16.67 ms. The benchmark suggests the following
working policy for production applications:

- **Hard upper bound:** GPU p95 below 16 ms. Frames above this have little
  compositor/jitter headroom.
- **Recommended production target:** GPU p95 below 12 ms for continuously
  animated content.
- **Conservative UI target:** GPU p95 below 10 ms when input responsiveness and
  background system load matter.

These are engineering budgets, not measurements of a hidden hardware limit.
They reserve approximately 4–6 ms for CPU work, presentation variance, thermal
variation, and other compositor activity.

Use `scripts/find_gpu_budget.sh` to find the highest iteration/layer/pass value
that stays below 12, 14, and 16 ms p95 for a selected workload.

## Golden rules

1. Prefer `mediump` for local shader math when visual error is acceptable.
   Keep high precision for large coordinates, long-running time values, and
   numerically sensitive accumulations.
2. Keep opaque UI surfaces opaque. Fullscreen alpha blending is substantially
   more expensive than the minimal fill baseline.
3. Avoid repeated fullscreen passes. A minimal copy pass is cheap, but shader
   work and especially effect-style passes can multiply the cost quickly.
4. Batch sprites and UI geometry. The equivalent-geometry sprite test shows
   the CPU benefit directly; the command-pressure control is a separate test.
5. Use atlases when they reduce state changes, but measure the actual shader and
   cache behavior. The current texture atlas experiment binds one texture for
   both logical inputs; it is a layout control, not a complete sprite engine.
6. Keep texture filtering appropriate to the asset. Linear filtering and more
   samples per pixel raise cost measurably; trilinear filtering should be used
   only with valid mipmaps.
7. ASTC is advertised by the driver, but this repository does not contain a
   verified ASTC encoder/asset. The benchmark logs ASTC requests and falls back
   to RGBA8888, so no ASTC speedup claim is made.
8. Stop rendering when content is static or the surface is hidden. This saves
   substantially more energy than micro-optimizing a continuously idle shader.
9. Keep `eglSwapInterval(1)`, lifecycle handling, opaque regions, and visibility
   suspension in the normal renderer. Aggressive pacing belongs only in stress
   tests.

## Texture guidance

The benchmark supports `nearest`, `linear`, and `trilinear` filter modes,
RGBA8888 and RGB565 uploads, texture sample counts, coherent/stride/randomish
patterns, and separate/atlas controls. Coherent 0.5–8 MiB working sets were
similar on the TV; a 32 MiB randomish workload became dramatically slower, but
its coordinate hash means it is not a pure DRAM-bandwidth measurement.

ETC2 and ASTC extension presence is logged by `egl_diagnostics.c`. Compressed
formats require real encoded assets. Do not substitute zero-filled blocks and
call the result a valid compression benchmark.

## Multipass and post-processing

The benchmark now separates three multipass meanings:

- `multipass_copy`: one texture sample and a render-target write, with a small
  temporal perturbation to prevent identical-frame transaction elimination.
- `multipass_alu`: the same dependency plus explicit lightweight ALU.
- `multipass_effect`: the previous synthetic shader with grid, cursor, theme,
  transcendental operations, and `pow`.

All variants ping-pong two RGBA8 FBO textures. Both targets are cleared and
finished before the measured warmup. The source/target IDs persist between
frames, so `STRESS_PASSES=1` alternates valid A→B and B→A chains rather than
sampling undefined storage. The GPU timer starts before the pass loop and ends
after it, including FBO reattachment, texture binding, and draw commands, but
excluding target initialization, warmup, logging, and cleanup.

`blur` is now a clean horizontal/vertical-style weighted accumulation with 3,
5, or 9 taps. It does not include the generic visual decoration block.

The old phase-3 “two fullscreen passes ≈14.3 ms” remains valid for
`multipass_effect`; it must not be used as the structural cost of every
render-to-texture pass.

## CPU and state-change guidance

Use `STRESS_WORKLOAD=command_pressure` (the `drawcalls` spelling remains an
alias) with `STRESS_DRAWS` and `STRESS_PROGRAM_SWITCHES` to identify CPU/driver
pressure. This control uses a 1x1 viewport and is intentionally not a visual
scene. For equivalent geometry, use `STRESS_WORKLOAD=sprites` with
`STRESS_SPRITES=N` and compare `STRESS_BATCH=0` against `STRESS_BATCH=1`: the
same prebuilt quad VBO is drawn N times or once.

## Presentation and latency

The normal renderer remains paced by Wayland callbacks and `eglSwapBuffers`.
`EGL_LATENCY_TRACE=1` adds opt-in software timestamps for input, render submit,
swap return, and callback intervals. These are not input-to-photon measurements:
the panel scanout and display electronics are not instrumented.

## Reproducibility

Use independent repetitions and aggregate JSONL output:

```bash
STRESS_REPEAT=3 SWEEP=production ./scripts/run_gpu_sweep.sh > sweep.jsonl
python3 scripts/summarize_gpu_sweep.py sweep.jsonl > sweep-summary.jsonl
```

Capture `scripts/gpu_status.sh` before and after long runs. The audited firmware
does not expose enough telemetry to prove thermal throttling; a sustained fall
in throughput across repeated 30–60 second runs is evidence to investigate,
not proof by itself.
