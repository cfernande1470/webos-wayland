#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"
BIN="/media/developer/apps/usr/palm/applications/${APP_ID}/bin/wayland_egl_stress"
WORKLOAD="${BUDGET_WORKLOAD:-alu}"
PRECISION="${BUDGET_PRECISION:-mediump}"
RESOLUTION="${BUDGET_RESOLUTION:-1080p}"
VALUES_CSV="${BUDGET_VALUES:-1,2,4,8,16,32,64}"
DURATION_MS="${BUDGET_DURATION_MS:-1200}"
WARMUP_MS="${BUDGET_WARMUP_MS:-250}"
LIMITS_CSV="${BUDGET_LIMITS:-12,14,16}"

IFS=',' read -r -a VALUES <<< "$VALUES_CSV"
IFS=',' read -r -a LIMITS <<< "$LIMITS_CSV"

declare -a RESULTS=()
echo "# GPU budget workload=$WORKLOAD precision=$PRECISION resolution=$RESOLUTION" >&2

for value in "${VALUES[@]}"; do
  parameter="STRESS_ITERS=$value"
  case "$WORKLOAD" in
    overdraw) parameter="STRESS_LAYERS=$value" ;;
    multipass|blur) parameter="STRESS_PASSES=$value" ;;
  esac
  echo "# measure value=$value" >&2
  result=$(ssh "$TV" "APP_ID=$APP_ID XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 STRESS_PACING=pbuffer STRESS_OUTPUT=jsonl STRESS_WORKLOAD=$WORKLOAD STRESS_PRECISION=$PRECISION STRESS_RESOLUTION=$RESOLUTION STRESS_DURATION_MS=$DURATION_MS STRESS_WARMUP_MS=$WARMUP_MS $parameter $BIN" 2>/dev/null | grep '^{' | tail -1 || true)
  p95=$(printf '%s\n' "$result" | sed -n 's/.*"gpu_p95_ms":\([0-9.]*\).*/\1/p')
  if [ -n "$p95" ]; then
    RESULTS+=("$value:$p95")
    printf 'RESULT workload=%s value=%s gpu_p95_ms=%s\n' "$WORKLOAD" "$value" "$p95"
  else
    echo "RESULT workload=$WORKLOAD value=$value gpu_p95_ms=NA" >&2
  fi
done

for limit in "${LIMITS[@]}"; do
  best="none"
  for pair in "${RESULTS[@]}"; do
    value="${pair%%:*}"
    p95="${pair##*:}"
    if awk -v actual="$p95" -v budget="$limit" 'BEGIN { exit !(actual < budget) }'; then
      if [ "$best" = "none" ] || [ "$value" -gt "$best" ]; then best="$value"; fi
    fi
  done
  printf 'BUDGET workload=%s p95_limit_ms=%s max_value=%s\n' "$WORKLOAD" "$limit" "$best"
done
