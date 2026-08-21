#!/usr/bin/env bash
set -euo pipefail

APP_ID="${APP_ID:-org.webosbrew.wayland}"
APP_VERSION="${APP_VERSION:-0.1.0}"
INCLUDE_STRESS="${INCLUDE_STRESS:-0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ROOT/dist/$APP_ID"
OUT_DIR="$ROOT/dist/ipk"
IPK="$OUT_DIR/${APP_ID}_${APP_VERSION}_arm.ipk"
PACKAGE_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/webos-wayland-ipk.XXXXXX")"

cleanup() {
  rm -rf "$PACKAGE_ROOT"
}
trap cleanup EXIT

case "$APP_ID" in
  ""|*[!A-Za-z0-9._-]*)
    echo "ERROR: invalid APP_ID: $APP_ID" >&2
    exit 2
    ;;
esac

case "$APP_VERSION" in
  ""|*[!A-Za-z0-9._-]*)
    echo "ERROR: invalid APP_VERSION: $APP_VERSION" >&2
    exit 2
    ;;
esac

test -f "$APP_DIR/appinfo.json"
test -f "$APP_DIR/packageinfo.json"
test -f "$APP_DIR/icon.png"
test -x "$APP_DIR/bin/native_main"
test -x "$APP_DIR/bin/wayland_egl"
test -x "$APP_DIR/bin/wayland_rect"

case "$INCLUDE_STRESS" in
  0|1) ;;
  *)
    echo "ERROR: INCLUDE_STRESS must be 0 or 1." >&2
    exit 2
    ;;
esac

if [ "$INCLUDE_STRESS" = "1" ]; then
  test -x "$APP_DIR/bin/wayland_egl_stress"
fi

mkdir -p \
  "$PACKAGE_ROOT/control" \
  "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/bin" \
  "$PACKAGE_ROOT/data/usr/palm/packages/$APP_ID" \
  "$OUT_DIR"

cp "$APP_DIR/appinfo.json" "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/appinfo.json"
cp "$APP_DIR/icon.png" "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/icon.png"
cp "$APP_DIR/bin/native_main" "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/bin/native_main"
cp "$APP_DIR/bin/wayland_egl" "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/bin/wayland_egl"
cp "$APP_DIR/bin/wayland_rect" "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/bin/wayland_rect"
cp -a "$APP_DIR/bin/client" "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/bin/client"
if [ "$INCLUDE_STRESS" = "1" ]; then
  cp "$APP_DIR/bin/wayland_egl_stress" \
    "$PACKAGE_ROOT/data/usr/palm/applications/$APP_ID/bin/wayland_egl_stress"
fi

cp "$APP_DIR/packageinfo.json" \
  "$PACKAGE_ROOT/data/usr/palm/packages/$APP_ID/packageinfo.json"

installed_size="$(du -sb "$PACKAGE_ROOT/data" | awk '{print $1}')"
cat > "$PACKAGE_ROOT/control/control" <<CONTROL
Package: $APP_ID
Version: $APP_VERSION
Section: misc
Priority: optional
Architecture: arm
Installed-Size: $installed_size
Maintainer: webosbrew <nobody@example.com>
Description: Native Wayland EGL test application for rooted webOS TVs.
webOS-Package-Format-Version: 2
webOS-Packager-Version: webos-wayland
CONTROL

tar --owner=0 --group=0 --numeric-owner \
  -czf "$PACKAGE_ROOT/control.tar.gz" -C "$PACKAGE_ROOT/control" control
tar --owner=0 --group=0 --numeric-owner \
  -czf "$PACKAGE_ROOT/data.tar.gz" -C "$PACKAGE_ROOT/data" .
printf '2.0\n' > "$PACKAGE_ROOT/debian-binary"

rm -f "$IPK"
ar rcs "$IPK" \
  "$PACKAGE_ROOT/debian-binary" \
  "$PACKAGE_ROOT/control.tar.gz" \
  "$PACKAGE_ROOT/data.tar.gz"

echo "Built $IPK"
sha256sum "$IPK"
ar t "$IPK"
