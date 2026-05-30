# webos-wayland

Native Wayland + EGL/GLES experiments for rooted LG webOS TVs.

This repository contains a minimal native webOS application that is launched by SAM (System Application Manager), creates a real Wayland surface on the TV compositor, receives input from the LG remote / Magic Remote, and renders using either:

- `wl_shm` software buffers as a CPU fallback;
- `wl_egl_window` + EGL + OpenGL ES as the GPU path;
- an optional 4K EGL/GLES stress-test client.

The project does **not** use Qt, SDL, X11, Xwayland, Firefox, Chromium, Electron, or a browser-based UI.

It is plain C on top of native webOS + Wayland.

## Final confirmed result

The EGL/GLES renderer was confirmed on the target TV with:

```text
EGL_VERSION 1.4
EGL_VENDOR ARM
EGL_CLIENT_APIS OpenGL_ES
GL_VENDOR ARM
GL_RENDERER Mali-G51
GL_VERSION OpenGL ES 3.2 v1.r9p0-01rel0.5fa3737ac24396a69b1d512b70e9e31d
```

That means rendering is going through the ARM Mali GPU driver, not through the previous `wl_shm` CPU path.

## What this project proves

The important discovery is that a Wayland client launched directly over SSH/root can create a surface and draw briefly, but it may not stay visible as a real foreground application.

The stable route is:

```text
Install as native webOS app
→ let SAM launch it
→ run inside webOS foreground app lifecycle/context
→ create Wayland surface
→ render with wl_shm or EGL/GLES
```

SAM foreground context matters.

## High-level architecture

```text
webOS SAM
└── org.webosbrew.wayland
    └── bin/native_main
        └── bin/client -> wayland_egl or wayland_rect
```

`native_main` is a small native entry point. It prepares the runtime environment and executes `bin/client`.

`bin/client` is a symlink so the renderer can be switched without rebuilding:

```text
client -> wayland_egl       # normal GPU renderer
client -> wayland_rect      # CPU / wl_shm fallback
client -> /tmp/...          # temporary experimental renderer
```

## Android shell experiment

The same native webOS app shell can also be used as a launcher surface for an `android` package:

- build with `scripts/build_android.sh`;
- install with `scripts/install_tv_android.sh`;
- launch with `scripts/launch_tv_android.sh`.

That package uses `APP_ID=org.webosbrew.android` and still keeps the normal Wayland fallbacks, but it also probes `bin/android_backend` before `bin/client`.

For the `android` package, `bin/android_backend` is now the bootstrap path. It tries to launch the Android sidecar if the USB payload is present and then shows a non-demo launch surface instead of the normal triangle client. The default `wayland` package still links `android_backend` back to `client`, so nothing changes for the existing Wayland app.

## Renderers

### `wayland_rect`

CPU fallback renderer.

Uses:

- `wl_compositor`;
- `wl_shell`;
- `wl_shm`;
- `wl_seat`;
- `wl_keyboard`;
- `wl_pointer`.

This client renders into shared-memory buffers and is useful for debugging, validating Wayland visibility, and checking input.

### `wayland_egl`

Normal GPU renderer.

Uses:

- `wl_compositor`;
- `wl_shell`;
- `wl_egl_window`;
- EGL;
- OpenGL ES;
- `wl_seat`;
- `wl_keyboard`;
- `wl_pointer`.

This is the preferred normal renderer.

### `wayland_egl_stress`

Experimental GPU stress-test renderer.

Uses the same EGL/GLES path as `wayland_egl`, but forces a 3840x2160 render target and runs a heavier fullscreen fragment shader.

It is intended as a benchmark / torture test, not as the default renderer.

Important: 4K video playback on a TV uses dedicated hardware video decode blocks. A 4K fullscreen fragment shader is a different workload and can be much heavier than video playback.

Observed heavy stress-test result on Mali-G51:

```text
STRESS_FRAME frame=... size=3840x2160 fps=~4-5 avg=~4-5 force4k=1
```

This low FPS is expected for the intentionally expensive shader. It does not mean the TV cannot play 4K video.

## Target environment

Observed target TV:

```text
Kernel: Linux LGwebOSTV 4.4.84 aarch64
Native app ABI used here: ARM 32-bit EABI
Dynamic loader: /lib/ld-linux.so.3 -> /lib/ld-2.28.so
Wayland socket: /tmp/xdg/wayland-0
XDG_RUNTIME_DIR: /tmp/xdg
WAYLAND_DISPLAY: wayland-0
Compositor: /usr/bin/surface-manager
```

Observed Wayland globals included:

```text
wl_compositor
wl_shm
wl_mali
wl_seat
wl_webos_surface_group_compositor
wl_webos_foreign
wl_output
wl_shell
text_model_factory
input_method
input_panel
wl_webos_xinput_extension
wl_starfish_output
wl_starfish_pointer
wl_webos_shell
wl_webos_input_manager
```

## 32-bit app on a 64-bit TV

The TV kernel is `aarch64`, but the working native app ABI in this setup is ARM 32-bit EABI.

Correct binary format:

```text
ELF 32-bit LSB executable, ARM, EABI5
interpreter /lib/ld-linux.so.3
```

Incorrect binary format for this setup:

```text
ELF 64-bit LSB executable, ARM aarch64
interpreter /lib/ld-linux-aarch64.so.1
```

The incorrect aarch64 build can fail with:

```text
Error: No such file or directory, exe: ...
```

because the requested loader is not available in the native app runtime.

This does not prevent GPU rendering. The EGL/GLES renderer still uses the ARM Mali GPU through the TV's native graphics stack.

## Toolchain

This project was built with the Homebrew/webOS ARM toolchain:

```text
arm-webos-linux-gnueabi_sdk-buildroot
```

Expected SDK path used during development:

```bash
/home/pi/disk/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
```

Create `.webos-sdk.env`:

```bash
cat > .webos-sdk.env <<'EOF'
export WEBOS_SDK="/home/pi/disk/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot"
EOF
```

If the SDK wrapper cannot find the real GCC, create this compatibility symlink:

```bash
source ./.webos-sdk.env

cd "$WEBOS_SDK/bin"
ln -sf arm-webos-linux-gnueabi-gcc-12.2.0.br_real arm-webos-linux-gnueabi-gcc.br_real
```

Verify the compiler:

```bash
source ./.webos-sdk.env
"$WEBOS_SDK/bin/arm-webos-linux-gnueabi-gcc" --version | head
```

## Repository layout

```text
native/
  native_main.c
  wayland_rect.c
  wayland_egl.c
  wayland_egl_stress.c

scripts/
  build.sh
  install_tv_lowspace.sh
  install_tv_safe.sh
  install_tv_atomic.sh
  launch_tv.sh
  stop_tv.sh
  status_tv.sh
  dev_cycle.sh

dist/
  org.webosbrew.wayland/
    appinfo.json
    icon.png
    bin/
      native_main
      wayland_rect
      wayland_egl
      wayland_egl_stress
```

`dist/` is a build output directory and does not need to be committed.

## Build

```bash
cd ~/disk/webos-wayland
./scripts/build.sh
```

Expected result:

```text
===== ABI RESULT =====
native_main:        ELF 32-bit LSB executable, ARM ...
wayland_rect:       ELF 32-bit LSB executable, ARM ...
wayland_egl:        ELF 32-bit LSB executable, ARM ...
wayland_egl_stress: ELF 32-bit LSB executable, ARM ...

===== HARD ABI GUARD =====
OK: ARM/webOS binaries generated.
```

## Install

The app is installed under:

```text
/media/developer/apps/usr/palm/applications/org.webosbrew.wayland
```

Use the low-space installer because the developer/appstore partition can be almost full:

```bash
./scripts/install_tv_lowspace.sh
```

The TV in this experiment had `/mnt/lg/appstore` at 100%, so experimental binaries should not be left in the app partition.

## Recommended TV app directory state

Keep the installed TV app minimal:

```text
/media/developer/apps/usr/palm/applications/org.webosbrew.wayland/bin/
  native_main
  wayland_egl
  wayland_rect
  client -> wayland_egl
```

Do not leave stress-test binaries in the app directory if storage is tight.

## Launch

```bash
./scripts/launch_tv.sh
```

Manual launch:

```bash
ssh root@192.168.2.121 '
luna-send -n 1 -f luna://com.webos.applicationManager/launch "{\"id\":\"org.webosbrew.wayland\"}"
'
```

## Stop

```bash
./scripts/stop_tv.sh
```

Manual fallback:

```bash
ssh root@192.168.2.121 '
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"org.webosbrew.wayland\"}"
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
killall client 2>/dev/null
'
```

## Switching renderers

### Use normal EGL/GPU renderer

```bash
ssh root@192.168.2.121 '
APP_DIR="/media/developer/apps/usr/palm/applications/org.webosbrew.wayland"

set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"org.webosbrew.wayland\"}" >/dev/null 2>&1
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
killall client 2>/dev/null

ln -sf wayland_egl "$APP_DIR/bin/client"
ls -l "$APP_DIR/bin/client"
'
```

Then:

```bash
./scripts/launch_tv.sh
```

### Use CPU fallback

```bash
ssh root@192.168.2.121 '
APP_DIR="/media/developer/apps/usr/palm/applications/org.webosbrew.wayland"

set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"org.webosbrew.wayland\"}" >/dev/null 2>&1
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
killall client 2>/dev/null

ln -sf wayland_rect "$APP_DIR/bin/client"
ls -l "$APP_DIR/bin/client"
'
```

Then:

```bash
./scripts/launch_tv.sh
```

## Running temporary experiments from `/tmp`

Because the appstore/developer partition can be full, experimental binaries should be uploaded to `/tmp` and selected through the `client` symlink.

Example for `wayland_egl_stress`:

```bash
scp dist/org.webosbrew.wayland/bin/wayland_egl_stress root@192.168.2.121:/tmp/wayland_egl_stress
```

```bash
ssh root@192.168.2.121 '
APP_DIR="/media/developer/apps/usr/palm/applications/org.webosbrew.wayland"

set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"org.webosbrew.wayland\"}" >/dev/null 2>&1
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
killall client 2>/dev/null

chmod 755 /tmp/wayland_egl_stress
ln -sf /tmp/wayland_egl_stress "$APP_DIR/bin/client"

rm -f /tmp/org.webosbrew.wayland.client.log
rm -f /tmp/org.webosbrew.wayland.native_main.log
'
```

Then launch:

```bash
./scripts/launch_tv.sh
```

Return to normal EGL afterwards:

```bash
ssh root@192.168.2.121 '
APP_DIR="/media/developer/apps/usr/palm/applications/org.webosbrew.wayland"

set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"org.webosbrew.wayland\"}" >/dev/null 2>&1
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
killall client 2>/dev/null

rm -f /tmp/wayland_egl_stress
rm -f /tmp/org.webosbrew.wayland.client.log
rm -f /tmp/org.webosbrew.wayland.native_main.log

ln -sf wayland_egl "$APP_DIR/bin/client"
'
```

## Logs

```bash
ssh root@192.168.2.121 '
echo "===== PROCS ====="
ps -ef | grep -E "org.webosbrew.wayland|native_main|wayland_rect|wayland_egl|wayland_egl_stress|client" | grep -v grep || true

echo
echo "===== native_main ====="
cat /tmp/org.webosbrew.wayland.native_main.log 2>/dev/null || true

echo
echo "===== client ====="
tail -200 /tmp/org.webosbrew.wayland.client.log 2>/dev/null || true

echo
echo "===== wl_shm fallback log ====="
tail -120 /tmp/org.webosbrew.wayland.wayland_rect.log 2>/dev/null || true
'
```

Confirm EGL/GPU:

```bash
ssh root@192.168.2.121 '
grep -E "EGL_VERSION|EGL_VENDOR|EGL_CLIENT_APIS|GL_VENDOR|GL_RENDERER|GL_VERSION" /tmp/org.webosbrew.wayland.client.log
'
```

Expected:

```text
EGL_VERSION 1.4
EGL_VENDOR ARM
EGL_CLIENT_APIS OpenGL_ES
GL_VENDOR ARM
GL_RENDERER Mali-G51
GL_VERSION OpenGL ES 3.2 ...
```

## Input

Observed input events:

```text
KEY key=28        OK / Enter
KEY key=106       Right
KEY key=105       Left
KEY key=103       Up
KEY key=108       Down
KEY key=1198      LG-specific remote key
KEY key=1199      LG-specific remote key / system exit-related
POINTER_BUTTON button=272
POINTER_MOTION x=... y=...
```

The Magic Remote pointer may start delivering motion only after pointer focus is acquired, usually after clicking once.

The system Exit/Back menu is owned by webOS/SAM. When that menu appears, the app can receive `KEYBOARD_LEAVE` and `POINTER_LEAVE`, so the app cannot reliably click inside that system dialog.

## 4K stress-test notes

The 4K stress client can force:

```text
size=3840x2160
force4k=1
```

A heavy shader produced around:

```text
~4-5 FPS on Mali-G51
```

That does **not** mean the TV cannot handle 4K video. Video playback uses dedicated decode hardware. The stress client runs a fragment shader across 8.29 million pixels per frame, which is a very different workload.

Use `wayland_egl` for normal interactive rendering.

Use `wayland_egl_stress` only for experimentation.

## Clean TV temporary files

```bash
ssh root@192.168.2.121 '
set +e

APP_ID="org.webosbrew.wayland"
APP_DIR="/media/developer/apps/usr/palm/applications/$APP_ID"

luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$APP_ID\"}" >/dev/null 2>&1
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
killall client 2>/dev/null

rm -f /tmp/wayland_egl_stress
rm -f /tmp/wayland_egl
rm -f /tmp/wayland_rect
rm -f /tmp/native_main
rm -rf /tmp/webos-wayland-upload-org.webosbrew.wayland
rm -f /tmp/org.webosbrew.wayland.native_main.log
rm -f /tmp/org.webosbrew.wayland.client.log
rm -f /tmp/org.webosbrew.wayland.wayland_rect.log

if [ -d "$APP_DIR/bin" ]; then
  rm -f "$APP_DIR/bin/wayland_egl_stress"
  rm -f "$APP_DIR/bin/client"
  ln -sf wayland_egl "$APP_DIR/bin/client"
fi

ls -lh "$APP_DIR/bin" 2>/dev/null || true
ls -l "$APP_DIR/bin/client" 2>/dev/null || true
df -h /tmp /media/developer /mnt/lg/appstore 2>/dev/null || df -h
'
```

## Troubleshooting

### `No such file or directory` when launching native binary

Check ABI:

```bash
file dist/org.webosbrew.wayland/bin/native_main
readelf -l dist/org.webosbrew.wayland/bin/native_main | grep interpreter
```

Correct:

```text
ELF 32-bit LSB executable, ARM
[Requesting program interpreter: /lib/ld-linux.so.3]
```

Incorrect:

```text
ELF 64-bit LSB executable, ARM aarch64
[Requesting program interpreter: /lib/ld-linux-aarch64.so.1]
```

### Surface appears briefly and disappears

Launch through SAM, not direct SSH:

```bash
./scripts/launch_tv.sh
```

### App does not appear to run after install

Reboot once so SAM reloads app metadata:

```bash
ssh root@192.168.2.121 'sync; reboot'
```

### No space left on device

The developer/appstore partition can be full.

Use `/tmp` for temporary renderers and keep the installed app directory minimal.

Clean temporary files:

```bash
ssh root@192.168.2.121 '
rm -rf /tmp/webos-wayland-upload-org.webosbrew.wayland
rm -f /tmp/wayland_egl_stress
rm -f /tmp/org.webosbrew.wayland.*.log
find /media/developer/apps/usr/palm/applications/org.webosbrew.wayland -name "*.new" -type f -delete 2>/dev/null || true
df -h /media/developer /tmp 2>/dev/null || df -h
'
```

## GitHub

Repository:

```text
git@github.com:cfernande1470/webos-wayland.git
```

Push to `main`:

```bash
cd ~/disk/webos-wayland

git init
git branch -M main

git add .
git commit -m "Initial native webOS Wayland EGL client"

git remote remove origin 2>/dev/null || true
git remote add origin git@github.com:cfernande1470/webos-wayland.git

git push -u origin main
```

If the remote already has commits:

```bash
git pull --rebase origin main
git push -u origin main
```

## License

No license has been selected yet.

Add a `LICENSE` file before treating this as a reusable open-source project. MIT is a reasonable default for a small experimental native client.
