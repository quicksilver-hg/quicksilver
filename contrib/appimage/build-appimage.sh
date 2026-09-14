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

fetch_tool() {
  local name="$1" url="$2" dest="$TOOLS_DIR/$1"
  if [ ! -x "$dest" ]; then
    echo ">> fetching $name"
    if ! curl -fL "$url" -o "$dest"; then
      echo "ERROR: could not download $name from:" >&2
      echo "       $url" >&2
      echo "Place an executable copy at $dest and re-run." >&2
      exit 1
    fi
    chmod +x "$dest"
  fi
}

fetch_tool linuxdeployqt \
  "https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-${ARCH}.AppImage"
fetch_tool appimagetool \
  "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage"

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
