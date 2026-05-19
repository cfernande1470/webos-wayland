#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="org.webosbrew.wayland"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"
REMOTE="/media/developer/apps/usr/palm/applications/$APP_ID"

test -x "$OUT/bin/native_main"
test -x "$OUT/bin/wayland_rect"

echo "===== LOCAL ABI ====="
file "$OUT/bin/native_main" "$OUT/bin/wayland_rect" || true
readelf -l "$OUT/bin/native_main" | grep -i interpreter || true
readelf -l "$OUT/bin/wayland_rect" | grep -i interpreter || true

ssh "$TV" "
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}' >/dev/null 2>&1
killall native_main 2>/dev/null
killall wayland_rect 2>/dev/null
set -e
mkdir -p '$REMOTE/bin'
"

scp "$OUT/appinfo.json" "$TV:$REMOTE/appinfo.json"
scp "$OUT/icon.png" "$TV:$REMOTE/icon.png"
scp "$OUT/bin/native_main" "$TV:$REMOTE/bin/native_main"
scp "$OUT/bin/wayland_rect" "$TV:$REMOTE/bin/wayland_rect"

ssh "$TV" "
set -e
chmod 755 '$REMOTE/bin/native_main' '$REMOTE/bin/wayland_rect'
chmod 644 '$REMOTE/appinfo.json' '$REMOTE/icon.png'

echo '===== REMOTE FILES ====='
find '$REMOTE' -maxdepth 3 -type f -print

echo
echo '===== REMOTE STATUS ====='
luna-send -n 1 -f luna://com.webos.applicationManager/getAppLoadStatus '{\"appId\":\"$APP_ID\"}'
"
