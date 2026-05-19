# webos-wayland

A minimal native Wayland application for rooted LG webOS TVs.

This project proves that a native webOS app launched by SAM (System Application Manager) can create a real Wayland surface on the TV compositor, draw into it using `wl_shm`, and receive basic input from the LG remote and Magic Remote pointer.

It does **not** use Qt, X11, Firefox, Electron, Chromium, SDL, or any GUI toolkit. It is a small native C experiment built directly on top of `libwayland-client`.

## Current status

Working:

- Native webOS app registered and launched by SAM.
- Native ARM/webOS binaries built with the Homebrew/webOS `arm-webos-linux-gnueabi` SDK.
- Wayland connection to the real webOS compositor.
- Fullscreen `wl_shell` surface.
- Software rendering through `wl_shm`.
- Frame pacing through Wayland frame callbacks.
- Triple-buffered shared-memory buffers.
- Keyboard/remote input through `wl_seat`.
- LG remote OK button.
- Directional keys.
- Magic Remote pointer motion after pointer focus is acquired.
- Pointer click events.
- Basic draggable demo window inside the rendered surface.
- Install/run/debug scripts for fast iteration.

Known limitations:

- The system Exit/Back UI is owned by webOS/SAM, not by this app. When that system dialog appears, the app loses pointer/keyboard focus.
- The app currently uses `wl_shell`, not `wl_webos_shell`.
- Rendering is CPU/software based through `wl_shm`; it does not use EGL/GLES yet.
- This is not a real Wayland compositor and not an X server. It is a native Wayland client drawing its own UI inside one fullscreen surface.
- Packaging as a clean IPK was not the successful route during this experiment; direct install under `/media/developer/apps/usr/palm/applications` was used.

## Architecture

```text
webOS SAM launch
└── org.webosbrew.wayland
    └── bin/native_main
        └── sets runtime environment
        └── starts bin/wayland_rect
            └── connects to /tmp/xdg/wayland-0
            └── creates wl_shell fullscreen surface
            └── renders with wl_shm
            └── receives wl_seat input
```

The important discovery is that launching the Wayland client directly over SSH/root is not enough for stable foreground presentation. Direct execution can show the surface briefly, but webOS may later hide or replace it. Running the client as a real native app launched by SAM gives it the correct foreground application context.

## What this is not

This project is **not Qt**.

It also is not:

- a QtWayland application;
- a web app;
- a Chromium/Electron wrapper;
- an X server;
- Xwayland;
- a nested compositor;
- a Firefox launcher.

The app is plain C using:

- `libwayland-client`;
- `wl_compositor`;
- `wl_shell`;
- `wl_shm`;
- `wl_seat`;
- `wl_pointer`;
- `wl_keyboard`.

## Repository layout

```text
native/
  native_main.c        # native app entry point / launcher wrapper
  wayland_rect.c       # Wayland client, renderer, input handling

scripts/
  build.sh             # cross-build using the webOS/Homebrew SDK
  install_tv_safe.sh   # older safe installer
  install_tv_atomic.sh # atomic installer variant
  install_tv_lowspace.sh # low-space installer used when TV storage is tight
  launch_tv.sh         # launch app through SAM
  stop_tv.sh           # stop app/processes
  status_tv.sh         # inspect process/log state
  dev_cycle.sh         # optional build/install/relaunch loop

dist/
  org.webosbrew.wayland/
    appinfo.json
    icon.png
    bin/
      native_main
      wayland_rect
```

## Target device

Tested on an LG webOS TV with:

```text
Kernel: Linux 4.4.84 aarch64
webOS native app userland: ARM 32-bit EABI
Dynamic loader: /lib/ld-linux.so.3 -> /lib/ld-2.28.so
Wayland socket: /tmp/xdg/wayland-0
XDG_RUNTIME_DIR: /tmp/xdg
WAYLAND_DISPLAY: wayland-0
Compositor: /usr/bin/surface-manager
```

Detected Wayland globals included:

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

## Toolchain

The TV kernel is `aarch64`, but the native application ABI used here is **ARM 32-bit EABI**, not Ubuntu/aarch64.

The correct output should look like this:

```text
ELF 32-bit LSB executable, ARM, EABI5, dynamically linked,
interpreter /lib/ld-linux.so.3
```

If your binary looks like this, it is wrong for this setup:

```text
ELF 64-bit LSB pie executable, ARM aarch64,
interpreter /lib/ld-linux-aarch64.so.1
```

That wrong aarch64 binary typically fails under `jailer` or SAM with:

```text
Error: No such file or directory, exe: ...
```

because the requested dynamic loader is not available in the native app environment.

## SDK setup

This project was built with the Homebrew/webOS native SDK:

```text
arm-webos-linux-gnueabi_sdk-buildroot
```

Expected SDK path in this setup:

```bash
/home/pi/disk/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
```

Create `.webos-sdk.env`:

```bash
cat > .webos-sdk.env <<'EOF'
export WEBOS_SDK="/home/pi/disk/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot"
EOF
```

If the SDK wrapper cannot find the real GCC, create the compatibility symlink:

```bash
source ./.webos-sdk.env

cd "$WEBOS_SDK/bin"
ln -sf arm-webos-linux-gnueabi-gcc-12.2.0.br_real arm-webos-linux-gnueabi-gcc.br_real
```

Verify:

```bash
source ./.webos-sdk.env

"$WEBOS_SDK/bin/arm-webos-linux-gnueabi-gcc" --version | head
file "$WEBOS_SDK/bin/toolchain-wrapper"
file "$WEBOS_SDK/bin/arm-webos-linux-gnueabi-gcc-12.2.0.br_real"
```

## Build

```bash
cd ~/disk/webos-wayland
./scripts/build.sh
```

Successful build output should include:

```text
===== ABI RESULT =====
dist/org.webosbrew.wayland/bin/native_main:  ELF 32-bit LSB executable, ARM ...
dist/org.webosbrew.wayland/bin/wayland_rect: ELF 32-bit LSB executable, ARM ...

===== HARD ABI GUARD =====
OK: binarios webOS ARM 32-bit generados.
```

## Install on TV

The app is installed directly into the developer app directory:

```text
/media/developer/apps/usr/palm/applications/org.webosbrew.wayland
```

Recommended installer when storage is tight:

```bash
cd ~/disk/webos-wayland
./scripts/install_tv_lowspace.sh
```

The low-space installer stops the running app, removes old binaries, uploads new binaries, and avoids needing double space for `.new` files.

If there is enough storage, the atomic installer can also be used:

```bash
./scripts/install_tv_atomic.sh
```

## Launch

Launch through SAM:

```bash
cd ~/disk/webos-wayland
./scripts/launch_tv.sh
```

Equivalent manual command on the TV:

```bash
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"org.webosbrew.wayland"}'
```

Expected process tree:

```text
/media/developer/apps/usr/palm/applications/org.webosbrew.wayland/bin/native_main
/media/developer/apps/usr/palm/applications/org.webosbrew.wayland/bin/wayland_rect
```

Depending on the current `native_main` wrapper implementation, `native_main` may either keep `wayland_rect` as a child process or `exec()` into it.

## Stop

```bash
cd ~/disk/webos-wayland
./scripts/stop_tv.sh
```

Manual fallback:

```bash
ssh root@192.168.2.121 '
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{"id":"org.webosbrew.wayland"}"
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
'
```

## Logs

```bash
ssh root@192.168.2.121 '
echo "===== PROCS ====="
ps -ef | grep -E "org.webosbrew.wayland|native_main|wayland_rect" | grep -v grep || true

echo
echo "===== native_main ====="
cat /tmp/org.webosbrew.wayland.native_main.log 2>/dev/null || true

echo
echo "===== wayland_rect ====="
tail -160 /tmp/org.webosbrew.wayland.wayland_rect.log 2>/dev/null || true
'
```

Live log:

```bash
ssh root@192.168.2.121 'tail -f /tmp/org.webosbrew.wayland.wayland_rect.log'
```

## Input behavior

Observed input events:

```text
KEY key=28       # OK / Enter
KEY key=106      # Right
KEY key=105      # Left
KEY key=103      # Up
KEY key=108      # Down
KEY key=1198     # LG-specific remote key
KEY key=1199     # LG-specific remote key / system exit-related
POINTER_BUTTON button=272
POINTER_MOTION x=... y=...
```

The Magic Remote pointer may not deliver motion until pointer focus is acquired, typically after an OK/click interaction.

The system Exit/Back dialog is handled by webOS, not this app. When it appears, the app can receive `KEYBOARD_LEAVE` and `POINTER_LEAVE`, so the app cannot reliably click inside that system dialog.

## Development loop

Optional fast iteration script:

```bash
./scripts/dev_cycle.sh
```

A typical manual loop:

```bash
cd ~/disk/webos-wayland

./scripts/build.sh
./scripts/install_tv_lowspace.sh

ssh root@192.168.2.121 '
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{"id":"org.webosbrew.wayland"}"
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
rm -f /tmp/org.webosbrew.wayland.wayland_rect.log
rm -f /tmp/org.webosbrew.wayland.native_main.log
'

./scripts/launch_tv.sh
```

## Safety notes

Do not overwrite system partitions, kernel images, rootfs images, TV services, or platform binaries.

This project only installs under:

```text
/media/developer/apps/usr/palm/applications/org.webosbrew.wayland
```

and writes temporary logs under:

```text
/tmp/org.webosbrew.wayland.native_main.log
/tmp/org.webosbrew.wayland.wayland_rect.log
```

## Troubleshooting

### `jailer` says `No such file or directory`

Check the binary ABI:

```bash
file dist/org.webosbrew.wayland/bin/native_main
readelf -l dist/org.webosbrew.wayland/bin/native_main | grep interpreter
```

If it says `aarch64` or `/lib/ld-linux-aarch64.so.1`, it was built with the wrong compiler.

Correct result:

```text
ELF 32-bit LSB executable, ARM
[Requesting program interpreter: /lib/ld-linux.so.3]
```

### Surface appears for two seconds and then disappears

This usually means the app was launched directly over SSH/root instead of as a foreground app through SAM.

Launch with:

```bash
./scripts/launch_tv.sh
```

or:

```bash
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{"id":"org.webosbrew.wayland"}'
```

### App launches but no process appears

Reboot the TV once after installing or modifying `appinfo.json`:

```bash
ssh root@192.168.2.121 'sync; reboot'
```

After reboot:

```bash
./scripts/launch_tv.sh
```

### `No space left on device`

Clean temporary files:

```bash
ssh root@192.168.2.121 '
rm -rf /tmp/webos-wayland-upload-org.webosbrew.wayland
rm -f /tmp/org.webosbrew.wayland.native_main.log
rm -f /tmp/org.webosbrew.wayland.wayland_rect.log
find /media/developer/apps/usr/palm/applications/org.webosbrew.wayland -name "*.new" -type f -delete 2>/dev/null || true
df -h /media/developer /tmp 2>/dev/null || df -h
'
```

Use:

```bash
./scripts/install_tv_lowspace.sh
```

Strip binaries if needed:

```bash
source ./.webos-sdk.env
"$WEBOS_SDK/bin/arm-webos-linux-gnueabi-strip" dist/org.webosbrew.wayland/bin/native_main
"$WEBOS_SDK/bin/arm-webos-linux-gnueabi-strip" dist/org.webosbrew.wayland/bin/wayland_rect
```

## GitHub

Repository:

```text
https://github.com/cfernande1470/webos-wayland
```

Initial push:

```bash
cd ~/disk/webos-wayland

cp /path/to/this/README.md README.md

git init
git branch -M main

git add .
git commit -m "Initial native webOS Wayland client"

git remote remove origin 2>/dev/null || true
git remote add origin git@github.com:cfernande1470/webos-wayland.git

git push -u origin main
```

If SSH auth is not configured, use HTTPS instead:

```bash
git remote set-url origin https://github.com/cfernande1470/webos-wayland.git
git push -u origin main
```

## License

No license has been selected yet.

Before publishing for reuse, add a `LICENSE` file. MIT is a reasonable default for this kind of small experimental native client.
