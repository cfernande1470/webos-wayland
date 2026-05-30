#!/usr/bin/env bash
set -euo pipefail
TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"

ssh "$TV" "
set -e
echo '===== CLOSE OLD ====='
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}' || true
killall wayland_rect 2>/dev/null || true
sleep 1

echo '===== LAUNCH ====='
luna-send -n 1 -f luna://com.webos.applicationManager/launch '{\"id\":\"$APP_ID\"}'

sleep 3
echo
echo '===== PROCS ====='
ps -ef | grep -E '$APP_ID|native_main|wayland_rect' | grep -v grep || true

echo
echo '===== native_main log ====='
cat "/tmp/${APP_ID}.native_main.log" 2>/dev/null || true

echo
echo '===== wayland_rect log ====='
tail -80 "/tmp/${APP_ID}.wayland_rect.log" 2>/dev/null || true

echo
echo '===== client log ====='
tail -120 "/tmp/${APP_ID}.client.log" 2>/dev/null || true
"
