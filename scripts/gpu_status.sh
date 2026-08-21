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

echo "===== MALI MODULES ====="
for module in /sys/module/mali_kbase /sys/module/mali; do
  [ -d "$module" ] || continue
  echo "MALI_MODULE=$module"
  find "$module" -maxdepth 3 -type f -readable 2>/dev/null | sort | while read -r node; do
    case "$node" in
      */parameters/*|*/uevent|*/refcnt)
        printf "%s=" "$node"
        cat "$node" 2>/dev/null || true
        echo
        ;;
    esac
  done
done

echo "===== DEBUGFS MALI (read-only, if already mounted) ====="
if [ -d /sys/kernel/debug ]; then
  debug_nodes=$(find /sys/kernel/debug -maxdepth 3 -iname "*mali*" -print 2>/dev/null || true)
  if [ -n "$debug_nodes" ]; then
    printf "%s\n" "$debug_nodes"
    printf "%s\n" "$debug_nodes" | while read -r node; do
      [ -f "$node" ] || continue
      [ -r "$node" ] || continue
      size=$(wc -c < "$node" 2>/dev/null || echo 0)
      [ "$size" -le 65536 ] || continue
      echo "DEBUG_NODE=$node"
      head -c 4096 "$node" 2>/dev/null || true
      echo
    done
  else
    echo "DEBUGFS_MALI=(none visible; debugfs was not mounted by this script)"
  fi
else
  echo "DEBUGFS=(not mounted)"
fi

echo "===== PROC GPU/MALI REFERENCES ====="
for node in /proc/driver/* /proc/*mali* /proc/*gpu*; do
  [ -f "$node" ] || continue
  case "$node" in
    *mali*|*gpu*|*galcore*)
      echo "PROC_NODE=$node"
      head -c 4096 "$node" 2>/dev/null || true
      echo
      ;;
  esac
done
if [ -r /proc/modules ]; then
  grep -iE "mali|galcore|gpu" /proc/modules || true
fi

echo "===== MALI COUNTER-LIKE NODES ====="
find /sys/devices/platform/mali.0 /sys/class /sys/kernel/debug -maxdepth 8 -type f \
  \( -iname "*util*" -o -iname "*busy*" -o -iname "*counter*" -o -iname "*occup*" \
     -o -iname "*tiler*" -o -iname "*shader*" -o -iname "*l2*" -o -iname "*fault*" \
     -o -iname "*stall*" -o -iname "*opp*" \) -readable 2>/dev/null | sort -u | \
  while read -r node; do
    case "$node" in
      *mali*|*gpu*|*/debug/*)
        printf "%s=" "$node"
        head -c 4096 "$node" 2>/dev/null || true
        echo
        ;;
    esac
  done

if [ "${GPU_STATUS_DMESG:-0}" = "1" ]; then
  echo "===== KERNEL GPU/THERMAL LOG REFERENCES ====="
  dmesg 2>/dev/null | grep -iE "mali|gpu|thermal|thrott|devfreq|galcore" | tail -200 || true
fi

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
