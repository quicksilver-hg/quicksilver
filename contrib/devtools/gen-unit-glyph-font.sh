#!/usr/bin/env bash
# Build the one-glyph face used to draw U+1F713.
#
# Source is Noto Sans Symbols Regular. The recorded SHA-256 is the Debian copy
# this script was written against. A different file stops the script.
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying file COPYING
# or http://www.opensource.org/licenses/mit-license.php.
export LC_ALL=C
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: gen-unit-glyph-font.sh SOURCE.ttf OUT.ttf" >&2
    exit 2
fi

SOURCE=$1
OUT=$2
EXPECTED=24b615ceb7d50a2c09060664ca9e24a160c9beb6bfca0563e8359369a4bc0993
ACTUAL=$(sha256sum "$SOURCE" | awk '{print $1}')
if [[ "$ACTUAL" != "$EXPECTED" ]]; then
    echo "source hash ${ACTUAL} does not match ${EXPECTED}" >&2
    exit 1
fi

pyftsubset "$SOURCE" \
    --unicodes=U+1F713 \
    --output-file="$OUT" \
    --name-IDs='*' \
    --layout-features= \
    --drop-tables=GDEF,GPOS,GSUB,DSIG

python3 - "$OUT" <<'PY'
import sys
from fontTools.ttLib import TTFont

path = sys.argv[1]
font = TTFont(path)
cmap = font.getBestCmap()
glyph_name = cmap.get(0x1F713)
if glyph_name is None or glyph_name == ".notdef":
    raise SystemExit("U+1F713 is missing or maps to .notdef")
glyph = font["glyf"][glyph_name]
if getattr(glyph, "numberOfContours", 0) < 1:
    raise SystemExit("U+1F713 outline is empty")

name = font["name"]
for rec in name.names:
    if "Reserved Font Name" in rec.toUnicode():
        raise SystemExit(f"Reserved Font Name in nameID {rec.nameID}")

def set_name(name_id, text):
    name.setName(text, name_id, 3, 1, 0x409)

set_name(1, "Quicksilver Unit Glyphs")
set_name(4, "Quicksilver Unit Glyphs Regular")
set_name(6, "QuicksilverUnitGlyphs-Regular")
set_name(16, "Quicksilver Unit Glyphs")
set_name(3, "QuicksilverUnitGlyphs-Regular; U+1F713")
font.save(path)

font = TTFont(path)
print(f"bytes {path} {__import__('os').path.getsize(path)}")
for rec in font["name"].names:
    if rec.nameID in (0, 1, 3, 4, 6, 13, 14, 16):
        print(f"name {rec.nameID} plat={rec.platformID} enc={rec.platEncID} lang={rec.langID} {rec.toUnicode()}")
    if "Reserved Font Name" in rec.toUnicode():
        raise SystemExit("Reserved Font Name survived the rename")
PY
