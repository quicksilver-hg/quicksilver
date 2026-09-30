#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# F-387. Link the GUI the way a distro packager does, then run
# quicksilver_no_qt_copy_reloc. The other CI jobs are non-LTO, so that
# check passes there without ever seeing the COPY relocation F-334 fixed.

export LC_ALL=C
set -euo pipefail

source_root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$source_root/build-lto-qt-copy-reloc"}

if ! command -v dpkg-buildflags >/dev/null 2>&1; then
    echo "F-387: dpkg-buildflags not found; install dpkg-dev" >&2
    exit 1
fi

cflags=$(dpkg-buildflags --get CFLAGS)
cxxflags=$(dpkg-buildflags --get CXXFLAGS)
ldflags=$(dpkg-buildflags --get LDFLAGS)

echo "F-387: CFLAGS=${cflags}"
echo "F-387: CXXFLAGS=${cxxflags}"
echo "F-387: LDFLAGS=${ldflags}"

# A leg that silently stops being LTO is the blindness this flag is about.
case "$cxxflags" in
    *-flto*) ;;
    *) echo "F-387: dpkg-buildflags CXXFLAGS has no -flto: ${cxxflags}" >&2; exit 1 ;;
esac
case "$ldflags" in
    *-flto*) ;;
    *) echo "F-387: dpkg-buildflags LDFLAGS has no -flto: ${ldflags}" >&2; exit 1 ;;
esac

# WERROR stays OFF: this job mirrors a packager, not the reference configuration.
# BUILD_TESTS stays at its default ON. enable_testing() runs only inside
# if(BUILD_TESTS), and the check is add_test()'d from src/qt when BUILD_GUI is
# ON. The Debian package sets BUILD_TESTS=OFF because it does not run ctest.
# CMAKE_BUILD_TYPE=None is what dh_auto_configure passes, so CMake does not add
# its own optimisation flags on top of dpkg-buildflags.
# WITH_CCACHE=OFF: the runner image ships ccache, and this job does not use it.
cmake -S "$source_root" -B "$build_dir" \
    -DBUILD_GUI=ON \
    -DWERROR=OFF \
    -DWITH_CCACHE=OFF \
    -DCMAKE_BUILD_TYPE=None \
    -DCMAKE_C_FLAGS="${cflags}" \
    -DCMAKE_CXX_FLAGS="${cxxflags}" \
    -DCMAKE_EXE_LINKER_FLAGS="${ldflags}"

cache="${build_dir}/CMakeCache.txt"
cxx_cache=$(grep -E '^CMAKE_CXX_FLAGS:' "$cache" || true)
ld_cache=$(grep -E '^CMAKE_EXE_LINKER_FLAGS:' "$cache" || true)
case "$cxx_cache" in
    *-flto*) ;;
    *) echo "F-387: CMakeCache.txt CMAKE_CXX_FLAGS has no -flto: ${cxx_cache}" >&2; exit 1 ;;
esac
case "$ld_cache" in
    *-flto*) ;;
    *) echo "F-387: CMakeCache.txt CMAKE_EXE_LINKER_FLAGS has no -flto: ${ld_cache}" >&2; exit 1 ;;
esac

# -j10 matches the workflow MAKEJOBS and the desktop cap.
cmake --build "$build_dir" --target quicksilver -j10

ctest --test-dir "$build_dir" -R '^quicksilver_no_qt_copy_reloc$' --no-tests=error --output-on-failure
