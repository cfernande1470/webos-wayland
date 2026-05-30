#!/usr/bin/env bash
set -euo pipefail

TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/$APP_ID"
REMOTE="/media/developer/apps/usr/palm/applications/$APP_ID"

test -x "$OUT/bin/native_main"
test -x "$OUT/bin/wayland_rect"
test -e "$OUT/bin/android_backend"

ssh "$TV" "
set +e
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}' >/dev/null 2>&1
killall native_main wayland_rect 2>/dev/null
set -e
rm -rf '$REMOTE'
mkdir -p '$REMOTE'
"

scp -r "$OUT/"* "$TV:$REMOTE/"

ssh "$TV" "
set -e
chmod 755 '$REMOTE/bin/native_main' '$REMOTE/bin/wayland_rect'
chmod 755 '$REMOTE/bin/android_backend' 2>/dev/null || true
chmod 644 '$REMOTE/appinfo.json' '$REMOTE/icon.png'

echo '===== INSTALLED FILES ====='
find '$REMOTE' -maxdepth 3 -type f -print -exec ls -l {} \;

echo '===== INSTALLED; NOT RESTARTING SAM ====='
echo 'App copied to:' '$REMOTE'
echo 'Now reboot the TV manually or run scripts/reboot_tv.sh if you want SAM to rescan safely.'
"
