#!/usr/bin/env bash
set -euo pipefail

APP_ID="${APP_ID:-org.webosbrew.android}"
APP_TITLE="${APP_TITLE:-Android}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

APP_ID="$APP_ID" APP_TITLE="$APP_TITLE" "$ROOT/scripts/build.sh"
