#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

./scripts/build.sh
./scripts/install_tv_lowspace.sh

ssh root@192.168.2.121 '
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId "{\"id\":\"org.webosbrew.wayland\"}" >/dev/null 2>&1
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
rm -f /tmp/org.webosbrew.wayland.wayland_rect.log
rm -f /tmp/org.webosbrew.wayland.native_main.log
'

./scripts/launch_tv.sh
