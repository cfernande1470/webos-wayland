#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"

ssh "$TV" '
set +e
echo "===== GPU STATUS (read-only) ====="
date

echo "===== DEVFREQ ====="
found_devfreq=0
for node in /sys/class/devfreq/*; do
  [ -e "$node" ] || continue
  found_devfreq=1
  echo "DEVFREQ_NODE=$node"
  for field in name cur_freq min_freq max_freq governor available_frequencies available_governors busy_time total_time; do
    if [ -r "$node/$field" ]; then
      printf "%s=" "$field"
      cat "$node/$field" 2>/dev/null || true
      echo
    fi
  done
done
if [ "$found_devfreq" -eq 0 ]; then
  echo "DEVFREQ_NODE=(none)"
fi

echo "===== MALI PLATFORM ====="
mali=/sys/devices/platform/mali.0
if [ -d "$mali" ]; then
  echo "MALI_PATH=$mali"
  for field in gpuinfo core_availability_policy core_mask power_policy dvfs_period \
               js_scheduling_period js_timeouts mem_pool_size mem_pool_max_size \
               lp_mem_pool_size lp_mem_pool_max_size; do
    if [ -r "$mali/$field" ]; then
      printf "%s=" "$field"
      cat "$mali/$field" 2>/dev/null || true
      echo
    fi
  done
else
  echo "MALI_PATH=(not found)"
fi

echo "===== RUNTIME POWER ====="
for node in /sys/devices/platform/mali.0/misc/mali0/power \
            /sys/devices/platform/mali.0/power; do
  [ -d "$node" ] || continue
  echo "POWER_PATH=$node"
  for field in control runtime_status runtime_active_time runtime_suspended_time \
               runtime_usage autosuspend_delay_ms; do
    if [ -r "$node/$field" ]; then
      printf "%s=" "$field"
      cat "$node/$field" 2>/dev/null || true
      echo
    fi
  done
done

echo "===== GALCORE PARAMETERS ====="
for field in /sys/module/galcore/parameters/*; do
  [ -r "$field" ] || continue
  printf "%s=" "$field"
  cat "$field" 2>/dev/null || true
  echo
done

echo "===== THERMAL ZONES ====="
found_thermal=0
for zone in /sys/class/thermal/thermal_zone*; do
  [ -d "$zone" ] || continue
  found_thermal=1
  type="(unknown)"
  temp="(unknown)"
  [ -r "$zone/type" ] && type=$(cat "$zone/type")
  [ -r "$zone/temp" ] && temp=$(cat "$zone/temp")
  echo "THERMAL_ZONE=$zone type=$type temp=$temp"
done
if [ "$found_thermal" -eq 0 ]; then
  echo "THERMAL_ZONE=(none)"
fi

echo "===== EXPOSED CLOCK/FREQUENCY NODES ====="
find /sys/devices -maxdepth 6 -type f \( -iname "*freq*" -o -iname "*clock*" -o -iname "utilization" -o -iname "busy_time" -o -iname "total_time" \) 2>/dev/null | while read -r node; do
  case "$node" in
    *mali*|*gpu*|*clock*|*/devfreq/*)
      printf "%s=" "$node"
      cat "$node" 2>/dev/null || true
      echo
      ;;
  esac
done
'
