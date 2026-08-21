#!/usr/bin/env bash
set -euo pipefail

APP_ID="${APP_ID:-org.webosbrew.wayland}"
APP_TITLE="${APP_TITLE:-Wayland EGL Native Lab}"
APP_VERSION="${APP_VERSION:-0.1.0}"
DEFAULT_RENDERER="${DEFAULT_RENDERER:-wayland_egl}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"

case "$APP_ID" in
  ""|*[!A-Za-z0-9._-]*)
    echo "ERROR: APP_ID may only contain letters, numbers, dots, underscores, and hyphens." >&2
    exit 2
    ;;
esac

case "$DEFAULT_RENDERER" in
  wayland_egl|wayland_rect|wayland_egl_stress) ;;
  *)
    echo "ERROR: unsupported DEFAULT_RENDERER: $DEFAULT_RENDERER" >&2
    exit 2
    ;;
esac

case "$APP_VERSION" in
  ""|*[!A-Za-z0-9._-]*)
    echo "ERROR: invalid APP_VERSION: $APP_VERSION" >&2
    exit 2
    ;;
esac

case "$APP_TITLE" in
  *[\"\\]*|*$'\n'*|*$'\r'*)
    echo "ERROR: APP_TITLE may not contain quotes, backslashes, or newlines." >&2
    exit 2
    ;;
esac

if [ ! -f "$ROOT/.webos-sdk.env" ]; then
  echo "ERROR: missing .webos-sdk.env; copy and edit .webos-sdk.env.example." >&2
  exit 2
fi

source "$ROOT/.webos-sdk.env"
: "${WEBOS_SDK:?WEBOS_SDK is not set by .webos-sdk.env}"

CC="$WEBOS_SDK/bin/arm-webos-linux-gnueabi-gcc"
STRIP="$WEBOS_SDK/bin/arm-webos-linux-gnueabi-strip"
SYSROOT="$WEBOS_SDK/arm-webos-linux-gnueabi/sysroot"

echo "===== USING TOOLCHAIN ====="
echo "CC=$CC"
echo "SYSROOT=$SYSROOT"
"$CC" --version | head -3

rm -rf "$OUT"
mkdir -p "$OUT/bin"

echo
echo "===== SDK CHECK ====="
find "$SYSROOT/usr/include" -maxdepth 3 -name 'wayland-client.h' -print | head || true
find "$SYSROOT/usr/include" -maxdepth 3 -name 'wayland-egl.h' -print | head || true
find "$SYSROOT/usr/include" -maxdepth 3 -name 'wayland-webos-shell-client-protocol.h' -print | head || true
find "$SYSROOT/usr/include" -maxdepth 4 \( -name 'egl.h' -o -name 'gl2.h' \) -print | head || true
find "$SYSROOT/usr/lib" -maxdepth 2 \( -name 'libwayland-egl*' -o -name 'libwayland-webos-client*' -o -name 'libEGL*' -o -name 'libGLESv2*' \) -print | head -20 || true

echo
echo "===== BUILD native_main ====="
"$CC" -O2 -Wall -Wextra \
  -DAPP_ID=\"${APP_ID}\" \
  "$ROOT/native/native_main.c" \
  -o "$OUT/bin/native_main"

echo
echo "===== BUILD wayland_rect fallback ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/wayland_rect.c" \
  "$ROOT/native/webos_input.c" \
  "$ROOT/native/webos_shell.c" \
  -o "$OUT/bin/wayland_rect" \
  -lwayland-webos-client -lwayland-client

echo
echo "===== BUILD wayland_egl ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/wayland_egl.c" \
  "$ROOT/native/webos_input.c" \
  "$ROOT/native/webos_shell.c" \
  -o "$OUT/bin/wayland_egl" \
  -lwayland-webos-client -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lm

if [ -f "$ROOT/native/wayland_egl_stress.c" ]; then
  echo
  echo "===== BUILD wayland_egl_stress ====="
  "$CC" -O2 -Wall -Wextra \
    "$ROOT/native/wayland_egl_stress.c" \
    "$ROOT/native/webos_input.c" \
    "$ROOT/native/webos_shell.c" \
    -o "$OUT/bin/wayland_egl_stress" \
    -lwayland-webos-client -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lm
fi

cat > "$OUT/appinfo.json" <<JSON
{
  "id": "$APP_ID",
  "version": "$APP_VERSION",
  "vendor": "local",
  "type": "native",
  "main": "bin/native_main",
  "title": "$APP_TITLE",
  "icon": "icon.png",
  "noSplashOnLaunch": true,
  "spinnerOnLaunch": false,
  "nativeLifeCycleInterfaceVersion": 2
}
JSON

cat > "$OUT/packageinfo.json" <<JSON
{
  "id": "$APP_ID",
  "version": "$APP_VERSION",
  "app": "$APP_ID"
}
JSON

base64 -d > "$OUT/icon.png" <<'B64'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=
B64

chmod 755 "$OUT/bin/native_main" "$OUT/bin/wayland_rect" "$OUT/bin/wayland_egl"
[ -f "$OUT/bin/wayland_egl_stress" ] && chmod 755 "$OUT/bin/wayland_egl_stress"
ln -sf "$DEFAULT_RENDERER" "$OUT/bin/client"
if [ "$APP_ID" = "org.webosbrew.android" ]; then
  ln -sf wayland_rect "$OUT/bin/android_backend"
fi

"$STRIP" --strip-unneeded "$OUT/bin/native_main"
"$STRIP" --strip-unneeded "$OUT/bin/wayland_rect"
"$STRIP" --strip-unneeded "$OUT/bin/wayland_egl"
if [ -f "$OUT/bin/wayland_egl_stress" ]; then
  "$STRIP" --strip-unneeded "$OUT/bin/wayland_egl_stress"
fi

echo
echo "===== ABI RESULT ====="
file "$OUT/bin/"* || true
for f in "$OUT/bin/"*; do
  [ -f "$f" ] || continue
  readelf -l "$f" | grep -i interpreter || true
done

echo
echo "===== HARD ABI GUARD ====="
if file "$OUT/bin/"* | grep -q 'aarch64'; then
  echo "ERROR: aarch64 binary detected. Do not install."
  exit 99
fi

if ! file "$OUT/bin/native_main" | grep -q 'ELF 32-bit.*ARM'; then
  echo "ERROR: native_main is not ARM 32-bit."
  exit 98
fi

if ! file "$OUT/bin/wayland_egl" | grep -q 'ELF 32-bit.*ARM'; then
  echo "ERROR: wayland_egl is not ARM 32-bit."
  exit 97
fi

if [ -f "$OUT/bin/wayland_egl_stress" ]; then
  if ! file "$OUT/bin/wayland_egl_stress" | grep -q 'ELF 32-bit.*ARM'; then
    echo "ERROR: wayland_egl_stress is not ARM 32-bit."
    exit 96
  fi
fi

echo "OK: ARM/webOS binaries generated."
ls -lh "$OUT/bin"
