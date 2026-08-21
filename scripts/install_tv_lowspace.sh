#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"
RENDERER="${RENDERER:-wayland_egl}"
INCLUDE_STRESS="${INCLUDE_STRESS:-0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"
REMOTE="/media/developer/apps/usr/palm/applications/$APP_ID"
TMP="/tmp/webos-wayland-upload-$APP_ID"

case "$APP_ID" in
  ""|*[!A-Za-z0-9._-]*)
    echo "ERROR: invalid APP_ID: $APP_ID" >&2
    exit 2
    ;;
esac

case "$RENDERER" in
  wayland_egl|wayland_rect) ;;
  wayland_egl_stress)
    if [ "$INCLUDE_STRESS" != "1" ]; then
      echo "ERROR: set INCLUDE_STRESS=1 to install the stress renderer." >&2
      exit 2
    fi
    ;;
  *)
    echo "ERROR: unsupported RENDERER: $RENDERER" >&2
    exit 2
    ;;
esac

test -x "$OUT/bin/native_main"
test -x "$OUT/bin/wayland_rect"
test -x "$OUT/bin/wayland_egl"
test -f "$OUT/appinfo.json"
test -f "$OUT/icon.png"

if [ "$INCLUDE_STRESS" = "1" ]; then
  test -x "$OUT/bin/wayland_egl_stress"
fi

echo "===== LOCAL SIZE/ABI ====="
ls -lh "$OUT/bin"
file "$OUT/bin/"* || true

ssh "$TV" "
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}' >/dev/null 2>&1
for proc_dir in /proc/[0-9]*; do
  exe=\$(readlink \"\$proc_dir/exe\" 2>/dev/null)
  case \"\$exe\" in
    '$REMOTE/bin/'*) kill \"\${proc_dir##*/}\" 2>/dev/null ;;
  esac
done
sleep 1
rm -rf '$TMP'
mkdir -p '$TMP' '$REMOTE/bin'
"

scp "$OUT/appinfo.json" "$TV:$TMP/appinfo.json"
scp "$OUT/icon.png" "$TV:$TMP/icon.png"
scp "$OUT/bin/native_main" "$TV:$TMP/native_main"
scp "$OUT/bin/wayland_rect" "$TV:$TMP/wayland_rect"
scp "$OUT/bin/wayland_egl" "$TV:$TMP/wayland_egl"

if [ "$INCLUDE_STRESS" = "1" ]; then
  scp "$OUT/bin/wayland_egl_stress" "$TV:$TMP/wayland_egl_stress"
fi

ssh "$TV" "
set -e

rm -f '$REMOTE/bin/native_main.new' \
      '$REMOTE/bin/wayland_rect.new' \
      '$REMOTE/bin/wayland_egl.new' \
      '$REMOTE/bin/wayland_egl_stress.new' \
      '$REMOTE/appinfo.json.new' \
      '$REMOTE/icon.png.new'

cp '$TMP/native_main' '$REMOTE/bin/native_main.new'
cp '$TMP/wayland_rect' '$REMOTE/bin/wayland_rect.new'
cp '$TMP/wayland_egl' '$REMOTE/bin/wayland_egl.new'
cp '$TMP/appinfo.json' '$REMOTE/appinfo.json.new'
cp '$TMP/icon.png' '$REMOTE/icon.png.new'

if [ '$INCLUDE_STRESS' = '1' ]; then
  cp '$TMP/wayland_egl_stress' '$REMOTE/bin/wayland_egl_stress.new'
fi

chmod 755 '$REMOTE/bin/native_main.new' '$REMOTE/bin/wayland_rect.new' '$REMOTE/bin/wayland_egl.new'
chmod 644 '$REMOTE/appinfo.json.new' '$REMOTE/icon.png.new'
if [ -f '$REMOTE/bin/wayland_egl_stress.new' ]; then
  chmod 755 '$REMOTE/bin/wayland_egl_stress.new'
fi

mv -f '$REMOTE/bin/native_main.new' '$REMOTE/bin/native_main'
mv -f '$REMOTE/bin/wayland_rect.new' '$REMOTE/bin/wayland_rect'
mv -f '$REMOTE/bin/wayland_egl.new' '$REMOTE/bin/wayland_egl'
mv -f '$REMOTE/appinfo.json.new' '$REMOTE/appinfo.json'
mv -f '$REMOTE/icon.png.new' '$REMOTE/icon.png'
if [ -f '$REMOTE/bin/wayland_egl_stress.new' ]; then
  mv -f '$REMOTE/bin/wayland_egl_stress.new' '$REMOTE/bin/wayland_egl_stress'
else
  rm -f '$REMOTE/bin/wayland_egl_stress'
fi

rm -f '$REMOTE/bin/client' '$REMOTE/bin/android_backend'
ln -sf '$RENDERER' '$REMOTE/bin/client'
if [ '$APP_ID' = 'org.webosbrew.android' ]; then
  ln -sf wayland_rect '$REMOTE/bin/android_backend'
fi

rm -rf '$TMP'
sync

echo '===== INSTALLED ====='
ls -lh '$REMOTE/bin'
echo 'renderer:'
ls -l '$REMOTE/bin/client'
df -h /media/developer /tmp 2>/dev/null || df -h
"
