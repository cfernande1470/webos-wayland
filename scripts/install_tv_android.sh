#!/usr/bin/env bash
set -euo pipefail

APP_ID="${APP_ID:-org.webosbrew.android}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

APP_ID="$APP_ID" "$ROOT/scripts/install_tv_lowspace.sh"
