#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

./scripts/build.sh
./scripts/install_tv_lowspace.sh
./scripts/launch_tv.sh
