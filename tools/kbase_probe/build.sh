#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT:-$ROOT/build/tools/kbase_probe}"
mkdir -p "$OUT"

ARM32_CC="${ARM32_CC:-/home/pi/disk/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc}"
A64_CC="${A64_CC:-/home/pi/disk/.tools/clang18-root/usr/bin/clang-18}"

build_arm32(){
  "$ARM32_CC" -marm -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-unwind-tables -fno-asynchronous-unwind-tables -c "$ROOT/tools/kbase_probe/kbase_probe.c" \
    -o "$OUT/kbase_probe.arm32.o"
  "$ARM32_CC" -marm -c "$ROOT/tools/kbase_probe/start_arm.S" -o "$OUT/start_arm.o"
  "$ARM32_CC" -marm -nostdlib -static -Wl,-e,_start \
    "$OUT/start_arm.o" "$OUT/kbase_probe.arm32.o" -o "$OUT/kbase_probe.arm32"
}

build_aarch64(){
  "$A64_CC" --target=aarch64-linux-gnu -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-unwind-tables -fno-asynchronous-unwind-tables -c "$ROOT/tools/kbase_probe/kbase_probe.c" \
    -o "$OUT/kbase_probe.aarch64.o"
  "$A64_CC" --target=aarch64-linux-gnu -c "$ROOT/tools/kbase_probe/start_aarch64.S" -o "$OUT/start_aarch64.o"
  "$A64_CC" --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld -Wl,-e,_start \
    "$OUT/start_aarch64.o" "$OUT/kbase_probe.aarch64.o" -o "$OUT/kbase_probe.aarch64"
}

build_arm32
build_aarch64
file "$OUT/kbase_probe.arm32" "$OUT/kbase_probe.aarch64"
sha256sum "$OUT/kbase_probe.arm32" "$OUT/kbase_probe.aarch64"
