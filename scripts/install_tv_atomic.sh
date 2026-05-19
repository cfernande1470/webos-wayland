#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="org.webosbrew.wayland"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"
REMOTE="/media/developer/apps/usr/palm/applications/$APP_ID"
TMP="/tmp/webos-wayland-upload-$APP_ID"

test -x "$OUT/bin/native_main"
test -x "$OUT/bin/wayland_rect"

echo "===== LOCAL ABI ====="
file "$OUT/bin/native_main" "$OUT/bin/wayland_rect" || true
readelf -l "$OUT/bin/native_main" | grep -i interpreter || true
readelf -l "$OUT/bin/wayland_rect" | grep -i interpreter || true

echo
echo "===== PREPARE REMOTE TMP ====="
ssh "$TV" "
set +e
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
set -e
rm -rf '$TMP'
mkdir -p '$TMP'
mkdir -p '$REMOTE/bin'
"

echo
echo "===== UPLOAD TO /tmp FIRST ====="
scp "$OUT/appinfo.json" "$TV:$TMP/appinfo.json"
scp "$OUT/icon.png" "$TV:$TMP/icon.png"
scp "$OUT/bin/native_main" "$TV:$TMP/native_main"
scp "$OUT/bin/wayland_rect" "$TV:$TMP/wayland_rect"

echo
echo "===== ATOMIC INSTALL ====="
ssh "$TV" "
set -e

APP_ID='$APP_ID'
REMOTE='$REMOTE'
TMP='$TMP'

set +e
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
sleep 1
set -e

cp '$TMP/appinfo.json' '$REMOTE/appinfo.json.new'
cp '$TMP/icon.png' '$REMOTE/icon.png.new'
cp '$TMP/native_main' '$REMOTE/bin/native_main.new'
cp '$TMP/wayland_rect' '$REMOTE/bin/wayland_rect.new'

chmod 644 '$REMOTE/appinfo.json.new' '$REMOTE/icon.png.new'
chmod 755 '$REMOTE/bin/native_main.new' '$REMOTE/bin/wayland_rect.new'

mv -f '$REMOTE/appinfo.json.new' '$REMOTE/appinfo.json'
mv -f '$REMOTE/icon.png.new' '$REMOTE/icon.png'
mv -f '$REMOTE/bin/native_main.new' '$REMOTE/bin/native_main'
mv -f '$REMOTE/bin/wayland_rect.new' '$REMOTE/bin/wayland_rect'

rm -rf '$TMP'

echo '===== INSTALLED ====='
ls -l '$REMOTE/bin'
file '$REMOTE/bin/native_main' '$REMOTE/bin/wayland_rect' 2>/dev/null || true

echo
echo '===== STATUS ====='
luna-send -n 1 -f luna://com.webos.applicationManager/getAppLoadStatus '{\"appId\":\"'$APP_ID'\"}' || true
"
