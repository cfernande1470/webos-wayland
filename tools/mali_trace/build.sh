#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="${CC:-/home/pi/disk/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc}"
OUT="${OUT:-$ROOT/build/tools/mali_trace}"
mkdir -p "$OUT"

"$CC" -O2 -fPIC -shared -Wall -Wextra -Wno-unused-parameter \
  -Wl,-soname,libmali_trace.so \
  "$ROOT/tools/mali_trace/mali_trace.c" -ldl -o "$OUT/libmali_trace.so"
file "$OUT/libmali_trace.so"
sha256sum "$OUT/libmali_trace.so"
