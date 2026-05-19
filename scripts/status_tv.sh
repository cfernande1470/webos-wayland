#!/usr/bin/env bash
set -euo pipefail
TV="${TV:-root@192.168.2.121}"
APP_ID="org.webosbrew.wayland"

ssh "$TV" "
echo '===== APP STATUS ====='
luna-send -n 1 -f luna://com.webos.applicationManager/getAppLoadStatus '{\"appId\":\"$APP_ID\"}'

echo
echo '===== PROCS ====='
ps -ef | grep -E '$APP_ID|native_main|wayland_rect' | grep -v grep || true

echo
echo '===== native_main log ====='
cat /tmp/org.webosbrew.wayland.native_main.log 2>/dev/null || true

echo
echo '===== wayland_rect log ====='
tail -80 /tmp/org.webosbrew.wayland.wayland_rect.log 2>/dev/null || true
"
