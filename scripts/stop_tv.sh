#!/usr/bin/env bash
set -euo pipefail
TV="${TV:-root@192.168.2.121}"
APP_ID="${APP_ID:-org.webosbrew.wayland}"
REMOTE="/media/developer/apps/usr/palm/applications/$APP_ID"

case "$APP_ID" in
  ""|*[!A-Za-z0-9._-]*)
    echo "ERROR: invalid APP_ID: $APP_ID" >&2
    exit 2
    ;;
esac

ssh "$TV" "
set +e
echo '===== CLOSE APP ====='
luna-send -n 1 -f luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$APP_ID\"}'
sleep 1
for proc_dir in /proc/[0-9]*; do
  exe=\$(readlink \"\$proc_dir/exe\" 2>/dev/null)
  case \"\$exe\" in
    '$REMOTE/bin/'*) kill \"\${proc_dir##*/}\" 2>/dev/null ;;
  esac
done

echo
echo '===== PROCS AFTER STOP ====='
for proc_dir in /proc/[0-9]*; do
  exe=\$(readlink \"\$proc_dir/exe\" 2>/dev/null)
  case \"\$exe\" in
    '$REMOTE/bin/'*) echo \"pid=\${proc_dir##*/} exe=\$exe\" ;;
  esac
done
"
