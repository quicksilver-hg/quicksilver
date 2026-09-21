#!/usr/bin/env bash
# Regenerate the Quicksilver brand raster set from the single source SVG.
#
# Source of truth: src/qt/res/src/quicksilver.svg (☿ on a Cinnabar disc). Every
# raster below is derived from it, so edit the SVG and re-run this script rather
# than touching the generated PNG/ICO/ICNS/BMP files by hand.
#
# Requires: rsvg-convert (librsvg), ImageMagick `convert`, and python3 (for the
# self-contained .icns packer — ImageMagick has no ICNS coder here).
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

# Qt in-app app icon (1024) + Windows/macOS bundles
rsvg-convert -w 1024 -h 1024 "$SVG" -o "$ICONS/quicksilver.png"
convert -background none "$ICONS/quicksilver.png" -define icon:auto-resize=256,128,64,48,32,16 "$ICONS/quicksilver.ico"

# macOS .icns: ImageMagick has no ICNS coder here, so render each size and pack
# the PNGs into a real ICNS container (macOS 10.7+ reads PNG-based entries).
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
declare -A ICNS_TYPES=( [16]=icp4 [32]=icp5 [128]=ic07 [256]=ic08 [512]=ic09 [1024]=ic10 )
icns_type_arg=""
for s in 16 32 128 256 512 1024; do
  rsvg-convert -w "$s" -h "$s" "$SVG" -o "$TMP/$s.png"
  icns_type_arg+="${s}:${ICNS_TYPES[$s]},"
done
python3 - "$ICONS/quicksilver.icns" "$TMP" "$icns_type_arg" <<'PY'
import struct, sys
out, tmp, type_arg = sys.argv[1], sys.argv[2], sys.argv[3]
types = {}
for entry in type_arg.rstrip(',').split(','):
    size, ostype = entry.split(':', 1)
    types[int(size)] = ostype.encode('ascii')
body = b''
for size, ostype in types.items():
    with open(f"{tmp}/{size}.png","rb") as f: data = f.read()
    body += ostype + struct.pack(">I", len(data)+8) + data
with open(out,"wb") as f:
    f.write(b'icns' + struct.pack(">I", len(body)+8) + body)
PY
# Public-test tint (hue-rotate the disc for network distinction)
convert "$ICONS/quicksilver.png" -modulate 100,100,60  -define icon:auto-resize=256,128,64,48,32,16 "$ICONS/quicksilver_publictest.ico"

# Linux desktop pixmaps
for s in 16 32 64 128 256; do rsvg-convert -w "$s" -h "$s" "$SVG" -o "$PIX/quicksilver${s}.png"; done
convert -background none "$PIX/quicksilver256.png" -define icon:auto-resize=256,128,64,48,32,16 "$PIX/quicksilver.ico"

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
