#!/usr/bin/env bash
set -euo pipefail
TV="${TV:-root@192.168.2.121}"
APP_ID="org.webosbrew.wayland"

ssh "$TV" "
set +e
echo '===== CLOSE APP ====='
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}'
sleep 1
killall native_main 2>/dev/null
killall wayland_rect 2>/dev/null

echo
echo '===== PROCS AFTER STOP ====='
ps -ef | grep -E '$APP_ID|native_main|wayland_rect' | grep -v grep || true
"
