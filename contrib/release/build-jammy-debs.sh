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

# A release build can leave the provenance record stage-release.py consumes.
# This checkout can name its own tag, commit, image, and build command. The
# reviewed development SHA and the installed binary's -version line are not
# in the public tree; the release procedure exports them. When either is
# unset, this block writes nothing and the build result is unchanged.
if [[ -n ${QS_DEVELOPMENT_SHA:-} && -n ${QS_VERSION_LINE:-} ]]; then
    public_sha=$(git -C "$repo" rev-parse HEAD)
    public_tag=$(git -C "$repo" describe --exact-match --tags HEAD 2>/dev/null || true)
    image_line=$(grep -m1 '^FROM ubuntu@' "$repo/contrib/release/jammy.Dockerfile" || true)
    image_digest=${image_line#FROM ubuntu@}
    dev_sha=$(printf '%s' "$QS_DEVELOPMENT_SHA" | tr '[:upper:]' '[:lower:]')
    case "$QS_VERSION_LINE" in
        *$'\n'*) version_line_ok=0 ;;
        *) version_line_ok=1 ;;
    esac
    if [[ $public_tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ \
        && $public_sha =~ ^[0-9a-f]{40}$ \
        && $dev_sha =~ ^[0-9a-f]{40}$ \
        && $image_digest == sha256:* \
        && $version_line_ok == 1 ]]; then
        {
            printf 'platform=ubuntu-22.04\n'
            printf 'public_tag=%s\n' "$public_tag"
            printf 'public_sha=%s\n' "$public_sha"
            printf 'development_sha=%s\n' "$dev_sha"
            printf 'builder_host=%s\n' "$(hostname)"
            printf 'os_userland=Ubuntu 22.04 (Jammy container)\n'
            printf 'compiler=g++ from build-essential in the pinned Jammy image\n'
            printf 'image_digest=%s\n' "$image_digest"
            printf 'build_command=dpkg-buildpackage -us -uc -b -j6\n'
            printf 'version_line=%s\n' "$QS_VERSION_LINE"
            find "$output" -maxdepth 1 -type f -printf '%f\n' | sort | while IFS= read -r name; do
                printf 'file=%s\n' "$name"
            done
        } >"$output/PROVENANCE"
    else
        echo 'provenance record not written: public tag, development SHA, image digest, or version line is missing or not in the required form' >&2
    fi
fi
