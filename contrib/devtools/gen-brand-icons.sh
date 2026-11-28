#!/usr/bin/env bash
# Regenerate the Quicksilver brand raster set from the single source SVG.
#
# Source of truth: src/qt/res/src/quicksilver.svg (☿ on a silver disc, Cinnabar ring). Every
# raster below is derived from it, so edit the SVG and re-run this script rather
# than touching the generated PNG/ICO/BMP/XPM files by hand.
#
# Other sources in src/qt/res/src/:
#   hud_*.svg               written by contrib/devtools/gen-qt-hud-icons.py (run below)
#   connect-*.svg, network_disabled.svg, mine.svg, proxy.svg, clock_*.svg,
#   transaction0.svg
#                           upstream sources with no export recipe; their PNGs are
#                           committed as they were drawn, not regenerated here.
#
# Requires: rsvg-convert (librsvg), ImageMagick `convert`, and python3 (for
# contrib/devtools/gen-qt-hud-icons.py).
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying file COPYING
# or http://www.opensource.org/licenses/mit-license.php.
export LC_ALL=C
set -euo pipefail
cd "$(dirname "$0")/../.."          # -> repository root
SVG=src/qt/res/src/quicksilver.svg
ICONS=src/qt/res/icons
PIX=share/pixmaps
DOCDIR=doc

# Qt in-app app icon (1024) + Windows .ico. This .ico is the one the Windows
# executables embed (src/qt/res/quicksilver-res.rc); share/pixmaps/quicksilver.ico
# below is the NSIS installer icon (cmake/module/GenerateSetupNsi.cmake). Both
# are kept: each is resized from a different master (1024 vs 256 px PNG), so
# their bytes differ though they carry the same sizes.
rsvg-convert -w 1024 -h 1024 "$SVG" -o "$ICONS/quicksilver.png"
convert -background none "$ICONS/quicksilver.png" -define icon:auto-resize=256,128,64,48,32,16 "$ICONS/quicksilver.ico"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Linux desktop pixmaps
for s in 16 32 64 128 256; do rsvg-convert -w "$s" -h "$s" "$SVG" -o "$PIX/quicksilver${s}.png"; done
convert -background none "$PIX/quicksilver256.png" -define icon:auto-resize=256,128,64,48,32,16 "$PIX/quicksilver.ico"
for s in 16 32 64 128 256; do convert "$PIX/quicksilver${s}.png" "$PIX/quicksilver${s}.xpm"; done

# Doxygen logo, constrained by Doxyfile.in's 55px maximum height.
rsvg-convert -w 55 -h 55 "$SVG" -o "$DOCDIR/quicksilver_logo_doxygen.png"
python3 contrib/devtools/gen-qt-hud-icons.py

# NSIS installer bitmaps (MUI fixes both sizes; 24-bit uncompressed BMP3, no alpha)
rsvg-convert -w 47 -h 47 "$SVG" -o "$TMP/nsis-mark-47.png"
convert -size 150x57 xc:white "$TMP/nsis-mark-47.png" -gravity east -geometry +6+0 \
        -composite -alpha remove -alpha off -type TrueColor "BMP3:$PIX/nsis-header.bmp"
rsvg-convert -w 120 -h 120 "$SVG" -o "$TMP/nsis-mark-120.png"
convert -size 164x314 xc:white "$TMP/nsis-mark-120.png" -gravity north -geometry +0+40 \
        -composite -alpha remove -alpha off -type TrueColor "BMP3:$PIX/nsis-wizard.bmp"

echo "brand icons regenerated"
