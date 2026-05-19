#!/usr/bin/env bash
set -euo pipefail
TV="${TV:-root@192.168.2.121}"

ssh "$TV" '
set +e
killall native_main 2>/dev/null
killall wayland_rect 2>/dev/null
sync
reboot
'
