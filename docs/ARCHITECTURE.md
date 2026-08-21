# Architecture

## Application lifecycle

The supported foreground path is:

```text
SAM launches org.webosbrew.wayland
  -> bin/native_main
     -> exports the Wayland/EGL runtime environment
     -> execs bin/client
        -> wayland_egl by default
```

`native_main` replaces itself with the renderer. It is not a supervisor and
does not leave an extra wrapper process behind. This makes SAM signals and exit
status apply directly to the renderer.

For `org.webosbrew.wayland`, the fallback order is:

```text
bin/client -> bin/wayland_egl -> bin/wayland_rect
```

For `org.webosbrew.android`, `bin/android_backend` is attempted first. Keeping
this decision in the launcher prevents a copied or dereferenced Android symlink
from intercepting the normal Wayland application.

## Runtime environment

The launcher sets:

```text
APP_ID=<compiled application ID>
XDG_RUNTIME_DIR=/tmp/xdg
WAYLAND_DISPLAY=wayland-0
DISPLAY_ID=0
EGL_PLATFORM=wayland
```

It removes `LD_PRELOAD` inherited from webOS before executing third-party
native code. The stress-test variables are defaults only and have no effect
unless the stress renderer is explicitly selected.

## Wayland objects

All renderers use these core objects:

```text
wl_display
  -> wl_registry
     -> wl_compositor
     -> wl_shell
     -> wl_webos_shell (optional v1+ lifecycle extension)
     -> all wl_seat globals (shared input router)

wl_compositor
  -> wl_surface
     -> wl_shell_surface (fullscreen role)
     -> wl_webos_shell_surface (optional lifecycle metadata/events)
```

The core shell role is always created. The webOS shell surface supplements it
with platform lifecycle information; it does not replace the core surface role.
If the extension is absent or attachment fails, rendering continues through
the verified `wl_shell` path.

The target advertises three `wl_seat` globals. They represent overlapping webOS
input paths, but the Magic Remote pointer is delivered through the second seat
rather than the first. `webos_input.c` therefore owns every advertised seat and
its pointer/keyboard objects. It aggregates focus and suppresses identical key
or pointer-button events received within a 20 ms window, preserving Magic
Remote input without applying duplicate actions.

Pointer and keyboard objects are created and released when seat capabilities
change. The application handles Linux input key codes delivered by the webOS
compositor.

## GPU renderer

`wayland_egl` creates:

```text
wl_surface
  -> wl_egl_window
     -> EGLSurface

EGLDisplay
  -> EGLConfig
  -> OpenGL ES 2 context
```

### EGL configuration selection

The client enumerates configs instead of accepting the first result from
`eglChooseConfig`. Eligible configs must:

- support `EGL_WINDOW_BIT`;
- support `EGL_OPENGL_ES2_BIT`;
- provide at least 8 bits each of red, green, and blue;
- not be marked `EGL_SLOW_CONFIG`.

The score prefers, in order:

1. zero alpha bits;
2. zero depth and stencil bits;
3. the smallest color buffers that satisfy RGB888.

The chosen attributes are logged. The target Mali driver currently selects an
RGBA8888 config with no depth or stencil buffer.

### Opaque surface hint

Every renderer creates a `wl_region` covering the current surface dimensions
and applies it with `wl_surface_set_opaque_region`. The hint is updated after a
resize.

This is valid even when the EGL native buffer contains an alpha channel because
all renderer output has alpha 1.0. It lets the compositor avoid blending or
redrawing content behind the surface when its implementation supports that
optimization. It does not guarantee direct scanout.

### Frame pacing

The normal renderer combines:

```text
eglSwapInterval(1)
+ wl_surface_frame callback
```

The next frame is rendered only from the callback. Hidden surfaces normally
stop receiving callbacks, preventing an uncontrolled busy loop. Animation time
uses the callback timestamp relative to the first presented frame, so a delayed
frame slows presentation without changing animation speed based on an assumed
refresh rate.

When `wl_webos_shell` explicitly reports a minimized or fully obscured surface,
the client destroys its outstanding frame callback and stops submitting new
frames. A later visible event starts the frame chain again.

## CPU fallback

`wayland_rect` allocates three reusable shared-memory buffers:

```text
mkstemp + unlink
  -> ftruncate
  -> mmap
  -> wl_shm_pool
  -> WL_SHM_FORMAT_XRGB8888 wl_buffer
```

The buffer release callback marks a buffer reusable. Dimensions and allocation
sizes are validated before conversion to Wayland's signed 32-bit pool size.

The CPU renderer redraws the full frame and is intentionally simple. It should
remain a diagnostics and fallback path.

## Stress renderer

`wayland_egl_stress` uses a full-screen triangle and a fragment shader to cover
the complete render target. It has four deliberately separate measurement
paths: frame-callback presentation, swap-without-frame-callback presentation,
a window-backed FBO path, and a pbuffer/surfaceless EGL path that avoids both
presentation and the Wayland EGL window surface. The offscreen paths limit GPU
work in flight so they measure completed work rather than an ever-growing
submission queue.

The shader is generated with a constant loop bound for the GLES compiler and
supports fill, ALU, SFU, texture, overdraw, multipass, blur, and draw-call
workloads. It parameterizes iteration counts, layers, blend modes, passes,
draws, texture working sets, filters, samples, and deterministic access
patterns, while retaining highp/mediump comparison. GPU timing uses dynamically loaded
`GL_EXT_disjoint_timer_query` functions when the driver advertises them; CPU
draw, query, swap, frame-total, and frame-callback timing are collected
independently. Resolution, window color format, timer depth, timer enablement,
texture pattern, output format, and optional IMG context priority are benchmark
variables, never production defaults.

Overdraw uses controlled fullscreen layers with none, alpha, premultiplied, or
additive blending. Multipass ping-pongs two RGBA8 FBO textures; blur uses a
small separable-style kernel. The draw-call workload uses a 1x1 viewport to
measure CPU/driver command pressure without conflating it with full-screen
fragment cost. These are benchmark-only paths.

Timing uses `CLOCK_MONOTONIC` and reports workload FPS, presented FPS, derived
MPixel/s and ns/pixel, machine-readable JSONL/TSV summaries, and percentiles
for every relevant CPU/GPU metric. Timer waits, ring pressure, finishes,
completed queries, and disjoint samples are explicit counters. It is packaged
only when `INCLUDE_STRESS=1` is supplied to the installer.

`egl_diagnostics.c` logs full EGL/GL strings and selected profiling-related
extensions. On the target, timer queries are available but partial-update,
buffer-age, and swap-with-damage extensions are absent, so the normal renderer
continues to use conservative full-frame presentation.

`EGL_LATENCY_TRACE=1` is an opt-in normal-renderer diagnostic. It records
software timestamps from input callbacks through render submit, swap return, and
the next Wayland frame callback, including callback jitter percentiles. It does
not claim input-to-photon latency and allocates no tracing buffers unless the
variable is enabled.

## webOS-specific protocols

All renderers share `webos_shell.c`. The helper binds at most protocol version 2
and was hardware-tested against the target's advertised version 1. It:

- associates the existing `wl_surface` with `wl_webos_shell_surface`;
- sets `appId` and display affinity `0`;
- requests fullscreen state;
- applies the default key mask plus Back and Exit;
- logs state, pending-state, position, exposed-region, close, and add-on events;
- derives effective visibility from both state and exposed rectangles;
- notifies the renderer to suspend or resume its frame chain;
- requests a clean renderer exit on a compositor close event.

The compositor also advertises `wl_webos_input_manager`, surface groups, and
Starfish extensions. Those protocols are not required for the current input or
graphics path.
