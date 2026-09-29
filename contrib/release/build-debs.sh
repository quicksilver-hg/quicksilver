#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
export LC_ALL=C
set -euo pipefail

usage() {
    echo "Usage: $0 {jammy|noble} OUTPUT_DIRECTORY" >&2
    exit 2
}

configure_suite() {
    case "$1" in
        jammy)
            platform=ubuntu-22.04
            userland='Ubuntu 22.04 (Jammy container)'
            dockerfile=jammy.Dockerfile
            image=quicksilver-jammy-build:local
            ;;
        noble)
            platform=ubuntu-24.04
            userland='Ubuntu 24.04 (Noble container)'
            dockerfile=noble.Dockerfile
            image=quicksilver-noble-build:local
            ;;
        *)
            echo "Unsupported Ubuntu suite: $1" >&2
            exit 2
            ;;
    esac
    suite=$1
}

check_build_stamp() {
    local source=$1 header=$2 tag commit
    tag=$(git -C "$source" describe --exact-match --tags HEAD 2>/dev/null || true)
    if [[ -n $tag ]]; then
        grep -Fqx "#define BUILD_GIT_TAG \"${tag}\"" "$header"
    else
        commit=$(git -C "$source" rev-parse --short=12 HEAD)
        grep -Fqx "#define BUILD_GIT_COMMIT \"${commit}\"" "$header"
    fi
}

write_file_records() {
    find "$1" -maxdepth 1 -type f ! -name PROVENANCE -printf '%f\n' | sort \
        | while IFS= read -r name; do
            printf 'file=%s\n' "$name"
        done
}

if [[ ${1:-} == --check-stamp ]]; then
    [[ $# == 3 ]] || usage
    check_build_stamp "$2" "$3"
    exit
fi

if [[ ${1:-} == --list-provenance-files ]]; then
    [[ $# == 2 ]] || usage
    write_file_records "$2"
    exit
fi

if [[ ${1:-} == --inside ]]; then
    [[ $# == 2 ]] || usage
    configure_suite "$2"
    cd /work/source
    git config --system --add safe.directory /work/source
    git diff-index --quiet HEAD --
    ln -sfn contrib/debian debian
    dpkg-checkbuilddeps
    dpkg-buildpackage -us -uc -b -j6
    # The build can succeed with an unstamped binary; inspect its actual header.
    check_build_stamp /work/source \
        obj-x86_64-linux-gnu/src/quicksilver-build-info.h
    exit
fi

[[ $# == 2 ]] || usage
configure_suite "$1"

repo=$(git -C "$(dirname "$0")/../.." rev-parse --show-toplevel)
git -C "$repo" diff-index --quiet HEAD --
[[ -z $(git -C "$repo" ls-files --others --exclude-standard) ]] || {
    echo 'Source checkout has untracked files' >&2
    exit 1
}
output=$(realpath -m "$2")
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
            "$image" -rf /work/source || :
    fi
    rm -rf "$scratch" || :
    return "$status"
}
trap cleanup EXIT
git clone --quiet --local --no-hardlinks "$repo" "$scratch/source"
[[ $(git -C "$scratch/source" rev-parse HEAD) == $(git -C "$repo" rev-parse HEAD) ]]

docker build --file "$repo/contrib/release/$dockerfile" \
    --tag "$image" "$repo"
docker run --rm --volume "$scratch:/work" --workdir /work/source "$image"

shopt -s nullglob
debs=("$scratch"/*.deb)
buildinfos=("$scratch"/*.buildinfo)
changes=("$scratch"/*.changes)
[[ ${#debs[@]} == 2 && ${#buildinfos[@]} == 1 && ${#changes[@]} == 1 ]]
mv -- "$scratch"/*.deb "$scratch"/*.ddeb "$scratch"/*.buildinfo \
    "$scratch"/*.changes "$output"/
package_version=$(dpkg-parsechangelog -l"$repo/contrib/debian/changelog" -SVersion)
[[ -f "$output/quicksilver_${package_version}_amd64.deb" && \
   -f "$output/quicksilver-devtools_${package_version}_amd64.deb" ]]

# A release build can leave the provenance record stage-release.py consumes.
# The release procedure supplies the development SHA and captured version line,
# which do not exist in the public source tree.
if [[ -n ${QS_DEVELOPMENT_SHA:-} && -n ${QS_VERSION_LINE:-} ]]; then
    public_sha=$(git -C "$repo" rev-parse HEAD)
    public_tag=$(git -C "$repo" describe --exact-match --tags HEAD 2>/dev/null || true)
    image_line=$(grep -m1 '^FROM ubuntu@' "$repo/contrib/release/$dockerfile" || true)
    image_digest=${image_line#FROM ubuntu@}
    dev_sha=$(printf '%s' "$QS_DEVELOPMENT_SHA" | tr '[:upper:]' '[:lower:]')
    case "$QS_VERSION_LINE" in
        *$'\n'*) version_line_ok=0 ;;
        *) version_line_ok=1 ;;
    esac
    if [[ $public_tag =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(rc([1-9][0-9]*))?$ \
        && $public_sha =~ ^[0-9a-f]{40}$ \
        && $dev_sha =~ ^[0-9a-f]{40}$ \
        && $image_digest == sha256:* \
        && $version_line_ok == 1 ]]; then
        {
            printf 'platform=%s\n' "$platform"
            printf 'public_tag=%s\n' "$public_tag"
            printf 'public_sha=%s\n' "$public_sha"
            printf 'development_sha=%s\n' "$dev_sha"
            printf 'builder_host=%s\n' "$(hostname)"
            printf 'os_userland=%s\n' "$userland"
            printf 'compiler=g++ from build-essential in the pinned %s image\n' "$suite"
            printf 'image_digest=%s\n' "$image_digest"
            printf 'build_command=dpkg-buildpackage -us -uc -b -j6\n'
            printf 'version_line=%s\n' "$QS_VERSION_LINE"
            write_file_records "$output"
        } >"$output/PROVENANCE"
    else
        echo 'provenance record not written: public tag, development SHA, image digest, or version line is missing or not in the required form' >&2
    fi
fi
