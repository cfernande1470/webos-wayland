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

echo "===== LOCAL SIZE/ABI ====="
ls -lh "$OUT/bin"
file "$OUT/bin/native_main" "$OUT/bin/wayland_rect" "$OUT/bin/wayland_egl" 2>/dev/null || true

echo
echo "===== REMOTE PREP ====="
ssh "$TV" "
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}' >/dev/null 2>&1
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
sleep 1

rm -rf '$TMP'
mkdir -p '$TMP'
mkdir -p '$REMOTE/bin'

rm -f /tmp/org.webosbrew.wayland.native_main.log
rm -f /tmp/org.webosbrew.wayland.wayland_rect.log
rm -f /tmp/org.webosbrew.wayland.client.log

echo 'DF before:'
df -h /media/developer /tmp 2>/dev/null || df -h
"

echo
echo "===== UPLOAD TO /tmp ====="
scp "$OUT/appinfo.json" "$TV:$TMP/appinfo.json"
scp "$OUT/icon.png" "$TV:$TMP/icon.png"
scp "$OUT/bin/native_main" "$TV:$TMP/native_main"
scp "$OUT/bin/wayland_rect" "$TV:$TMP/wayland_rect"

if [ -x "$OUT/bin/wayland_egl" ]; then
  scp "$OUT/bin/wayland_egl" "$TV:$TMP/wayland_egl"
fi

echo
echo "===== LOW SPACE INSTALL ====="
ssh "$TV" "
set -e

REMOTE='$REMOTE'
TMP='$TMP'

set +e
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
sleep 1
set -e

rm -f '$REMOTE/bin/native_main'
rm -f '$REMOTE/bin/wayland_rect'
rm -f '$REMOTE/bin/wayland_egl'
rm -f '$REMOTE/bin/client'
rm -f '$REMOTE/appinfo.json'
rm -f '$REMOTE/icon.png'
sync

cp '$TMP/native_main' '$REMOTE/bin/native_main'
cp '$TMP/wayland_rect' '$REMOTE/bin/wayland_rect'

if [ -f '$TMP/wayland_egl' ]; then
  cp '$TMP/wayland_egl' '$REMOTE/bin/wayland_egl'
  ln -sf wayland_egl '$REMOTE/bin/client'
else
  ln -sf wayland_rect '$REMOTE/bin/client'
fi

cp '$TMP/appinfo.json' '$REMOTE/appinfo.json'
cp '$TMP/icon.png' '$REMOTE/icon.png'

chmod 755 '$REMOTE/bin/native_main' '$REMOTE/bin/wayland_rect'
[ -f '$REMOTE/bin/wayland_egl' ] && chmod 755 '$REMOTE/bin/wayland_egl'
chmod 644 '$REMOTE/appinfo.json' '$REMOTE/icon.png'

rm -rf '$TMP'
sync

echo '===== INSTALLED ====='
ls -lh '$REMOTE/bin'
file '$REMOTE/bin/native_main' '$REMOTE/bin/wayland_rect' '$REMOTE/bin/wayland_egl' 2>/dev/null || true
echo 'client symlink:'
ls -l '$REMOTE/bin/client' || true

echo
echo 'DF after:'
df -h /media/developer /tmp 2>/dev/null || df -h

echo
echo '===== STATUS ====='
luna-send -n 1 -f luna://com.webos.applicationManager/getAppLoadStatus '{\"appId\":\"org.webosbrew.wayland\"}' || true
"
