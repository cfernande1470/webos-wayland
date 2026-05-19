#!/usr/bin/env bash
set -euo pipefail

APP_ID="org.webosbrew.wayland"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"

source "$ROOT/.webos-sdk.env"

CC="$WEBOS_SDK/bin/arm-webos-linux-gnueabi-gcc"
AR="$WEBOS_SDK/bin/arm-webos-linux-gnueabi-ar"
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
test -x "$CC"
test -d "$SYSROOT"
find "$SYSROOT/usr/include" -maxdepth 3 -name 'wayland-client.h' -print | head || true
find "$SYSROOT/usr/lib" -maxdepth 2 -name 'libwayland-client*' -print | head || true

echo
echo "===== BUILD native_main ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/native_main.c" \
  -o "$OUT/bin/native_main"

echo
echo "===== BUILD wayland_rect ====="
"$CC" -O2 -Wall -Wextra \
  "$ROOT/native/wayland_rect.c" \
  -o "$OUT/bin/wayland_rect" \
  -lwayland-client -lrt

cat > "$OUT/appinfo.json" <<JSON
{
  "id": "$APP_ID",
  "version": "0.0.5",
  "vendor": "local",
  "type": "native",
  "main": "bin/native_main",
  "title": "Wayland Native Lab",
  "icon": "icon.png",
  "noSplashOnLaunch": true,
  "spinnerOnLaunch": false,
  "nativeLifeCycleInterfaceVersion": 2
}
JSON

base64 -d > "$OUT/icon.png" <<'B64'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=
B64

chmod 755 "$OUT/bin/native_main" "$OUT/bin/wayland_rect"

echo
echo "===== ABI RESULT ====="
file "$OUT/bin/native_main" "$OUT/bin/wayland_rect"
readelf -l "$OUT/bin/native_main" | grep -i interpreter || true
readelf -l "$OUT/bin/wayland_rect" | grep -i interpreter || true

echo
echo "===== HARD ABI GUARD ====="
if file "$OUT/bin/native_main" | grep -q 'aarch64'; then
  echo "ERROR: sigue siendo aarch64. NO instalar."
  exit 99
fi

if ! file "$OUT/bin/native_main" | grep -q 'ELF 32-bit.*ARM'; then
  echo "ERROR: no es ELF 32-bit ARM. NO instalar."
  exit 98
fi

echo "OK: binarios webOS ARM 32-bit generados."
