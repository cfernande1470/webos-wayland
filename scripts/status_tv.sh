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
echo '===== APP STATUS ====='
luna-send -n 1 -f luna://com.webos.applicationManager/getAppLoadStatus '{\"appId\":\"$APP_ID\"}'

echo
echo '===== PROCS ====='
for proc_dir in /proc/[0-9]*; do
  exe=\$(readlink \"\$proc_dir/exe\" 2>/dev/null)
  case \"\$exe\" in
    '$REMOTE/bin/'*)
      printf 'pid=%s exe=%s cmd=' \"\${proc_dir##*/}\" \"\$exe\"
      tr '\\000' ' ' < \"\$proc_dir/cmdline\"
      echo
      ;;
  esac
done

echo
echo '===== native_main log ====='
cat "/tmp/${APP_ID}.native_main.log" 2>/dev/null || true

echo
echo '===== client log ====='
tail -120 "/tmp/${APP_ID}.client.log" 2>/dev/null || true
"
