#!/usr/bin/env bash
set -euo pipefail

APP_ID="org.webosbrew.wayland"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"

source "$ROOT/.webos-sdk.env"

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
find "$SYSROOT/usr/include" -maxdepth 3 \( -name 'egl.h' -o -name 'gl2.h' \) -print | head || true
find "$SYSROOT/usr/lib" -maxdepth 2 \( -name 'libwayland-egl*' -o -name 'libEGL*' -o -name 'libGLESv2*' \) -print | head -20 || true

echo
echo "===== BUILD native_main ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/native_main.c" \
  -o "$OUT/bin/native_main"

echo
echo "===== BUILD wayland_rect fallback ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/wayland_rect.c" \
  -o "$OUT/bin/wayland_rect" \
  -lwayland-client

echo
echo "===== BUILD wayland_egl ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/wayland_egl.c" \
  -o "$OUT/bin/wayland_egl" \
  -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lm

cat > "$OUT/appinfo.json" <<JSON
{
  "id": "$APP_ID",
  "version": "0.0.6-egl",
  "vendor": "local",
  "type": "native",
  "main": "bin/native_main",
  "title": "Wayland EGL Native Lab",
  "icon": "icon.png",
  "noSplashOnLaunch": true,
  "spinnerOnLaunch": false,
  "nativeLifeCycleInterfaceVersion": 2
}
JSON

base64 -d > "$OUT/icon.png" <<'B64'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=
B64

chmod 755 "$OUT/bin/native_main" "$OUT/bin/wayland_rect" "$OUT/bin/wayland_egl"

# Reduce size for the TV developer partition.
"$STRIP" --strip-unneeded "$OUT/bin/native_main" || true
"$STRIP" --strip-unneeded "$OUT/bin/wayland_rect" || true
"$STRIP" --strip-unneeded "$OUT/bin/wayland_egl" || true

echo
echo "===== ABI RESULT ====="
file "$OUT/bin/native_main" "$OUT/bin/wayland_rect" "$OUT/bin/wayland_egl"
readelf -l "$OUT/bin/native_main" | grep -i interpreter || true
readelf -l "$OUT/bin/wayland_rect" | grep -i interpreter || true
readelf -l "$OUT/bin/wayland_egl" | grep -i interpreter || true

echo
echo "===== HARD ABI GUARD ====="
if file "$OUT/bin/native_main" "$OUT/bin/wayland_rect" "$OUT/bin/wayland_egl" | grep -q 'aarch64'; then
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

echo "OK: ARM/webOS binaries generated."
ls -lh "$OUT/bin"
