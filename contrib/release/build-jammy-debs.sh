#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
export LC_ALL=C
set -euo pipefail

if [[ ${1:-} == --inside ]]; then
    [[ $# == 1 ]]
    cd /work/source
    git config --system --add safe.directory /work/source
    git diff-index --quiet HEAD --
    commit=$(git rev-parse --short=12 HEAD)
    ln -sfn contrib/debian debian
    dpkg-checkbuilddeps
    dpkg-buildpackage -us -uc -b -j6
    # The build can succeed with an unstamped binary; inspect its actual header.
    grep -Fqx "#define BUILD_GIT_COMMIT \"${commit}\"" \
        obj-x86_64-linux-gnu/src/quicksilver-build-info.h
    exit
fi

if [[ $# != 1 ]]; then
    echo "Usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi

repo=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
git -C "$repo" diff-index --quiet HEAD --
[[ -z $(git -C "$repo" ls-files --others --exclude-standard) ]] || {
    echo 'Source checkout has untracked files' >&2
    exit 1
}
output=$(realpath -m "$1")
case "$output/" in
    "$repo/"*) echo 'Output directory must be outside the checkout' >&2; exit 2 ;;
esac
mkdir -p "$output"
[[ -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] || {
    echo 'Output directory must be empty' >&2
    exit 2
}

scratch=$(mktemp -d)
cleanup() {
    status=$?
    if [[ -d "$scratch/source" ]]; then
        docker run --rm --volume "$scratch:/work" --entrypoint rm \
            quicksilver-jammy-build:local -rf /work/source || :
    fi
    rm -rf "$scratch" || :
    return "$status"
}
trap cleanup EXIT
git clone --quiet --local --no-hardlinks "$repo" "$scratch/source"
[[ $(git -C "$scratch/source" rev-parse HEAD) == $(git -C "$repo" rev-parse HEAD) ]]

docker build --file "$repo/contrib/release/jammy.Dockerfile" \
    --tag quicksilver-jammy-build:local "$repo"
docker run --rm --volume "$scratch:/work" --workdir /work/source \
    quicksilver-jammy-build:local

shopt -s nullglob
debs=("$scratch"/*.deb)
buildinfos=("$scratch"/*.buildinfo)
changes=("$scratch"/*.changes)
[[ ${#debs[@]} == 2 && ${#buildinfos[@]} == 1 && ${#changes[@]} == 1 ]]
mv -- "$scratch"/*.deb "$scratch"/*.ddeb "$scratch"/*.buildinfo \
    "$scratch"/*.changes "$output"/
[[ -f "$output/quicksilver_0.1.1-1_amd64.deb" && \
   -f "$output/quicksilver-devtools_0.1.1-1_amd64.deb" ]]
