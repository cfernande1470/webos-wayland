#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"
BIN="/media/developer/apps/usr/palm/applications/${APP_ID}/bin/wayland_egl_stress"
SWEEP="${SWEEP:-quick}"
REPEAT="${STRESS_REPEAT:-${SWEEP_REPEAT:-1}}"
if [ "$SWEEP" = "full" ] || [ "$SWEEP" = "production" ]; then
  DURATION_MS="${SWEEP_DURATION_MS:-2500}"
  WARMUP_MS="${SWEEP_WARMUP_MS:-500}"
else
  DURATION_MS="${SWEEP_DURATION_MS:-1200}"
  WARMUP_MS="${SWEEP_WARMUP_MS:-250}"
fi

case "$SWEEP" in
  quick|production|full) ;;
  *) echo "ERROR: SWEEP must be quick, production, or full" >&2; exit 2 ;;
esac

case "$REPEAT" in
  ''|*[!0-9]*|0) echo "ERROR: STRESS_REPEAT/SWEEP_REPEAT must be a positive integer" >&2; exit 2 ;;
esac
[ "$REPEAT" -le 20 ] || { echo "ERROR: repeat count is capped at 20" >&2; exit 2; }

echo "# GPU sweep mode=$SWEEP repeat=$REPEAT tv=$TV duration_ms=$DURATION_MS warmup_ms=$WARMUP_MS" >&2

run_case() {
  local label="$1"
  shift
  local repeat
  for repeat in $(seq 1 "$REPEAT"); do
    echo "# case=$label repeat=$repeat" >&2
    local envs="APP_ID=$APP_ID XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 STRESS_OUTPUT=jsonl STRESS_PACING=pbuffer STRESS_DURATION_MS=$DURATION_MS STRESS_WARMUP_MS=$WARMUP_MS STRESS_TIMER_SLOTS=${STRESS_TIMER_SLOTS:-16} STRESS_REPEAT_INDEX=$repeat"
    local value
    for value in "$@"; do envs="$envs $value"; done
    ssh "$TV" "$envs $BIN" 2>/dev/null || echo "# case_failed=$label repeat=$repeat" >&2
  done
}

if [ "$SWEEP" = "production" ]; then
  resolutions=(1080p)
else
  resolutions=(720p 1080p)
fi
for resolution in "${resolutions[@]}"; do
  run_case "fill-$resolution" STRESS_WORKLOAD=fill STRESS_RESOLUTION="$resolution"
done
if [ "$SWEEP" = "full" ]; then
  run_case "fill-1440p" STRESS_WORKLOAD=fill STRESS_RESOLUTION=1440p
  run_case "fill-4k" STRESS_WORKLOAD=fill STRESS_RESOLUTION=4k
else
  run_case "fill-4k" STRESS_WORKLOAD=fill STRESS_RESOLUTION=4k
fi

if [ "$SWEEP" = "full" ]; then
  alu_iters=(1 2 4 8 16 32 64)
  sfu_iters=(1 2 4 8 16 32)
  texture_sizes=(256 512 1024 2048 4096)
  texture_patterns=(coherent stride randomish)
else
  alu_iters=(1 16 64)
  sfu_iters=(1 16)
  texture_sizes=(256 1024 2048)
  texture_patterns=(coherent randomish)
fi

for precision in highp mediump; do
  for iters in "${alu_iters[@]}"; do
    run_case "alu-${precision}-${iters}" STRESS_WORKLOAD=alu STRESS_PRECISION="$precision" STRESS_ITERS="$iters" STRESS_RESOLUTION=1080p
  done
done

for iters in "${sfu_iters[@]}"; do
  run_case "sfu-${iters}" STRESS_WORKLOAD=sfu STRESS_PRECISION=mediump STRESS_ITERS="$iters" STRESS_RESOLUTION=1080p
done

for size in "${texture_sizes[@]}"; do
  for pattern in "${texture_patterns[@]}"; do
    run_case "bandwidth-${size}-${pattern}" STRESS_WORKLOAD=bandwidth STRESS_PRECISION=mediump STRESS_ITERS=4 STRESS_TEXTURE_SIZE="$size" STRESS_TEXTURE_PATTERN="$pattern" STRESS_RESOLUTION=1080p
  done
done

if [ "$SWEEP" = "production" ] || [ "$SWEEP" = "full" ]; then
  for layers in 1 2 4 8; do
    run_case "overdraw-opaque-$layers" STRESS_WORKLOAD=overdraw STRESS_BLEND=none STRESS_LAYERS="$layers" STRESS_RESOLUTION=1080p
    run_case "overdraw-alpha-$layers" STRESS_WORKLOAD=overdraw STRESS_BLEND=alpha STRESS_LAYERS="$layers" STRESS_RESOLUTION=1080p
    if [ "$SWEEP" = "full" ]; then
      run_case "overdraw-additive-$layers" STRESS_WORKLOAD=overdraw STRESS_BLEND=additive STRESS_LAYERS="$layers" STRESS_RESOLUTION=1080p
    fi
  done
  for passes in 1 2 4; do
    run_case "multipass-$passes" STRESS_WORKLOAD=multipass STRESS_PASSES="$passes" STRESS_RESOLUTION=1080p
  done
  for taps in 3 5 9; do
    run_case "blur-$taps" STRESS_WORKLOAD=blur STRESS_BLUR_TAPS="$taps" STRESS_RESOLUTION=1080p
  done
  for draws in 1 100 500; do
    run_case "drawcalls-$draws" STRESS_WORKLOAD=drawcalls STRESS_DRAWS="$draws" STRESS_RESOLUTION=1080p
  done
  run_case "drawcalls-batched" STRESS_WORKLOAD=drawcalls STRESS_DRAWS=500 STRESS_BATCH=1 STRESS_RESOLUTION=1080p
  if [ "$SWEEP" = "full" ]; then
    for samples in 1 4 8; do
      run_case "texture-samples-$samples" STRESS_WORKLOAD=bandwidth STRESS_TEXTURE_SAMPLES="$samples" STRESS_TEXTURE_SIZE=1024 STRESS_TEXTURE_PATTERN=coherent STRESS_RESOLUTION=1080p
    done
  fi
fi

run_case "alu-window-fbo" STRESS_PACING=offscreen STRESS_WORKLOAD=alu STRESS_PRECISION=mediump STRESS_ITERS=4 STRESS_RESOLUTION=1080p
run_case "alu-surfaceless" STRESS_PACING=pbuffer STRESS_WORKLOAD=alu STRESS_PRECISION=mediump STRESS_ITERS=4 STRESS_RESOLUTION=1080p
