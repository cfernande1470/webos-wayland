#!/usr/bin/env bash
set -euo pipefail
TV="${TV:-root@192.168.2.121}"

ssh "$TV" '
sync
reboot
'
