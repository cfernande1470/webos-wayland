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
     -> first wl_seat

wl_compositor
  -> wl_surface
     -> wl_shell_surface (fullscreen role)
```

The target advertises several `wl_seat` globals. They may represent overlapping
webOS input paths rather than independent users. Binding all of them generated
duplicate focus events, so the clients intentionally bind only the first seat.

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
the complete render target. With `STRESS_FORCE_4K=1`, it ignores the compositor's
1920x1080 configure size and keeps a 3840x2160 EGL window.

Timing uses `CLOCK_MONOTONIC`. The renderer logs recent and average FPS every
two seconds. It is packaged only when `INCLUDE_STRESS=1` is supplied to the
installer.

## webOS-specific protocols

The compositor also advertises `wl_webos_shell`,
`wl_webos_input_manager`, surface groups, and Starfish extensions. The current
stable implementation uses core `wl_shell` because that is the path verified
through SAM on the target firmware.

`wl_webos_shell` remains a useful future experiment for exposed-region events,
close requests, state transitions, and webOS key masks. It should be introduced
behind a fallback rather than replacing the confirmed core-shell path without
hardware lifecycle testing.
