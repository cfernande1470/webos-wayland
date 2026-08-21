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
| Two fullscreen passes | ~14.3 ms | Already close to the conservative 60 Hz budget |
| Blur, 5 taps, two passes | ~16.0 ms | Exceeds the recommended production budget |
| 500 tiny draw calls | ~2.9 ms GPU, ~2.6 ms CPU draw | CPU/driver overhead becomes visible |
| 500 calls reduced to one call | ~0.1 ms GPU, ~0.03 ms CPU draw | Batching is highly valuable |

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
3. Avoid repeated fullscreen passes. Two realistic passes already approach the
   conservative budget; blur kernels multiply that cost.
4. Batch sprites and UI geometry. The 500-draw experiment was roughly an order
   of magnitude slower than the equivalent single-call control.
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

`multipass` ping-pongs two RGBA8 FBO textures. `blur` performs horizontal and
vertical-style passes with 3, 5, or 9 taps. These are intentionally simple
proxies for bloom, blur, filters, and visualizers. They do not model every cache,
format, or shader variation in a production effect.

## CPU and state-change guidance

Use `drawcalls` with `STRESS_DRAWS`, `STRESS_BATCH`, and
`STRESS_PROGRAM_SWITCHES` to identify CPU/driver pressure. The benchmark uses a
1x1 viewport for draw-call pressure so it does not confuse command overhead
with a full-screen fragment workload. It is therefore not a sprite-quality or
layout benchmark.

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
