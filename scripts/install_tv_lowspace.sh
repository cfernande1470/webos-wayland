#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"
REMOTE="/media/developer/apps/usr/palm/applications/$APP_ID"
TMP="/tmp/webos-wayland-upload-$APP_ID"

test -x "$OUT/bin/native_main"
test -x "$OUT/bin/wayland_rect"
test -x "$OUT/bin/wayland_egl"

echo "===== LOCAL SIZE/ABI ====="
ls -lh "$OUT/bin"
file "$OUT/bin/"* || true

ssh "$TV" "
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}' >/dev/null 2>&1
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
sleep 1

rm -rf '$TMP'
mkdir -p '$TMP'
mkdir -p '$REMOTE/bin'

rm -f "/tmp/${APP_ID}.native_main.log"
rm -f "/tmp/${APP_ID}.client.log"
rm -f "/tmp/${APP_ID}.wayland_rect.log"
"

scp "$OUT/appinfo.json" "$TV:$TMP/appinfo.json"
scp "$OUT/icon.png" "$TV:$TMP/icon.png"
scp "$OUT/bin/native_main" "$TV:$TMP/native_main"
scp "$OUT/bin/wayland_rect" "$TV:$TMP/wayland_rect"
scp "$OUT/bin/wayland_egl" "$TV:$TMP/wayland_egl"
scp "$OUT/bin/android_backend" "$TV:$TMP/android_backend"

if [ -x "$OUT/bin/wayland_egl_stress" ]; then
  scp "$OUT/bin/wayland_egl_stress" "$TV:$TMP/wayland_egl_stress"
fi

ssh "$TV" "
set -e

REMOTE='$REMOTE'
TMP='$TMP'

set +e
killall wayland_egl_stress 2>/dev/null
killall wayland_egl 2>/dev/null
killall wayland_rect 2>/dev/null
killall native_main 2>/dev/null
sleep 1
set -e

rm -f '$REMOTE/bin/native_main'
rm -f '$REMOTE/bin/wayland_rect'
rm -f '$REMOTE/bin/wayland_egl'
rm -f '$REMOTE/bin/wayland_egl_stress'
rm -f '$REMOTE/bin/client'
rm -f '$REMOTE/bin/android_backend'
rm -f '$REMOTE/appinfo.json'
rm -f '$REMOTE/icon.png'
sync

cp '$TMP/native_main' '$REMOTE/bin/native_main'
cp '$TMP/wayland_rect' '$REMOTE/bin/wayland_rect'
cp '$TMP/wayland_egl' '$REMOTE/bin/wayland_egl'
cp '$TMP/android_backend' '$REMOTE/bin/android_backend'

if [ -f '$TMP/wayland_egl_stress' ]; then
  cp '$TMP/wayland_egl_stress' '$REMOTE/bin/wayland_egl_stress'
  ln -sf wayland_egl_stress '$REMOTE/bin/client'
else
  ln -sf wayland_egl '$REMOTE/bin/client'
fi

cp '$TMP/appinfo.json' '$REMOTE/appinfo.json'
cp '$TMP/icon.png' '$REMOTE/icon.png'

chmod 755 '$REMOTE/bin/native_main'
chmod 755 '$REMOTE/bin/wayland_rect'
chmod 755 '$REMOTE/bin/wayland_egl'
chmod 755 '$REMOTE/bin/android_backend'
[ -f '$REMOTE/bin/wayland_egl_stress' ] && chmod 755 '$REMOTE/bin/wayland_egl_stress'
chmod 644 '$REMOTE/appinfo.json' '$REMOTE/icon.png'

rm -rf '$TMP'
sync

echo '===== INSTALLED ====='
ls -lh '$REMOTE/bin'
echo
echo 'client symlink:'
ls -l '$REMOTE/bin/client'
echo
file '$REMOTE/bin/'* 2>/dev/null || true
echo
df -h /media/developer /tmp 2>/dev/null || df -h
"
