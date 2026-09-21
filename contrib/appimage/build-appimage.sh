#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Build a portable Quicksilver-Qt AppImage.
#
# One command, no system packages touched: the linuxdeployqt and appimagetool
# helpers are fetched (as their own AppImages) into a gitignored .tools/ cache.
# Qt5 development packages and a working C++ toolchain must already be present.
export LC_ALL=C
set -euo pipefail

# Run bundled helper AppImages without requiring FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"          # repository root
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-appimage}"
APPDIR="$BUILD_DIR/AppDir"
TOOLS_DIR="$SCRIPT_DIR/.tools"
ARCH="$(uname -m)"

mkdir -p "$TOOLS_DIR"

# These pins were independently downloaded and hashed on 2026-09-20. The
# linuxdeployqt numbered releases are marked "Do not use anymore", so its
# digest pins the supported continuous asset at upstream build commit
# 5af5737e72046234daa1c855f1f4397da9979a75. appimagetool publishes 1.9.1 as a
# versioned release, so both its version and digest are pinned.
#
# To re-pin, choose the official release asset, download it, run sha256sum, and
# update its URL and digest together. The SHA-256 check is the integrity
# boundary; do not "temporarily" skip it.
case "$ARCH" in
  x86_64)
    LINUXDEPLOYQT_URL="https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"
    LINUXDEPLOYQT_SHA256="974a87457ed26241b793bed7841978fcdf84158d13220e53833a06515f173b0b"
    APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
    APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
    ;;
  *)
    echo "ERROR: AppImage helpers are not pinned for architecture: $ARCH" >&2
    exit 1
    ;;
esac

fetch_tool() {
  local name="$1" url="$2" expected_sha256="$3" dest="$TOOLS_DIR/$1"
  local actual_sha256

  if [ ! -e "$dest" ]; then
    echo ">> fetching $name"
    if ! curl -fL "$url" -o "$dest"; then
      rm -f "$dest"
      echo "ERROR: could not download $name from:" >&2
      echo "       $url" >&2
      echo "Place a copy of the pinned artifact at $dest and re-run." >&2
      exit 1
    fi
  else
    echo ">> using cached $name"
  fi

  actual_sha256="$(sha256sum "$dest" | awk '{print $1}')"
  if [ "$actual_sha256" != "$expected_sha256" ]; then
    rm -f "$dest"
    echo "ERROR: SHA-256 mismatch for $dest" >&2
    echo "  expected: $expected_sha256" >&2
    echo "  actual:   $actual_sha256" >&2
    echo "The file was deleted. Re-run to fetch it again if the copy was" >&2
    echo "corrupted -- but if upstream moved the release the URL points at," >&2
    echo "every re-run fetches the same new artifact: re-pin instead." >&2
    exit 1
  fi

  echo ">> SHA-256 verified: $dest"
  chmod +x "$dest"
}

fetch_tool linuxdeployqt \
  "$LINUXDEPLOYQT_URL" "$LINUXDEPLOYQT_SHA256"
fetch_tool appimagetool \
  "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256"

echo ">> configuring (application only)"
# QS_DEVELOPER_TOOLS=OFF is what keeps the command-line tools out of the
# bundle. Turning individual BUILD_* switches off is not enough on its own:
# several of them default ON, so a future default change would quietly sweep a
# new binary into the AppImage without anyone editing this script.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GUI=ON \
  -DBUILD_DAEMON=OFF \
  -DBUILD_CLI=OFF \
  -DBUILD_TESTS=OFF \
  -DQS_DEVELOPER_TOOLS=OFF


echo ">> building quicksilver-qt and quicksilver-agent"
cmake --build "$BUILD_DIR" -j"$(nproc)" --target quicksilver-qt quicksilver-agent

echo ">> staging into AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr

echo ">> bundling Qt and building the AppImage"
cd "$BUILD_DIR"
"$TOOLS_DIR/linuxdeployqt" \
  "$APPDIR/usr/share/applications/io.github.quicksilver_hg.quicksilver_qt.desktop" \
  -bundle-non-qt-libs -no-translations -verbose=1
"$TOOLS_DIR/appimagetool" "$APPDIR"

echo ">> done: $(ls -1 "$BUILD_DIR"/*.AppImage 2>/dev/null || echo '(see appimagetool output)')"
