#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Stranger's check for a staged release directory, and the same check after
# a GitHub Release has been unpacked back into that layout.
#
# Every signature is checked with gpg --status-fd. A VALIDSIG line whose
# primary fingerprint is not the fingerprint on the command line is a
# failure, including when the signature is valid for some other key.
# Exit status alone is not the check: gpg accepts any key in the keyring.

export LC_ALL=C
set -euo pipefail

die() {
    echo "ERROR: $*" >&2
    exit 1
}

if [[ $# -ne 2 ]]; then
    echo "Usage: verify-release.sh DIRECTORY FINGERPRINT" >&2
    exit 2
fi

[[ -d $1 ]] || die "not a directory: $1"
dir=$(realpath -- "$1")
fpr=$2
# [[:xdigit:]] rather than a letter range: under LC_ALL=C the range a-F is empty.
[[ "$fpr" =~ ^[[:xdigit:]]{40}$ ]] || die "fingerprint must be the full 40 hex digits, not a short key id"
expected=${fpr^^}

verify_sig() {
    local signature=$1 file=$2
    local status primary
    if [[ ! -f $signature ]]; then
        die "missing signature ${signature#"$dir"/}"
    fi
    if [[ ! -f $file ]]; then
        die "missing signed file ${file#"$dir"/}"
    fi
    if ! status=$(gpg --batch --status-fd 1 --verify -- "$signature" "$file" 2>/dev/null); then
        die "signature verification failed for ${file#"$dir"/}"
    fi
    primary=$(printf '%s\n' "$status" | awk '/^\[GNUPG:\] VALIDSIG / { fingerprint = $NF } END { print fingerprint }')
    [[ -n $primary ]] || die "no VALIDSIG status for ${file#"$dir"/}; refusing an unpinned signature"
    [[ ${primary^^} == "$expected" ]] || die "${file#"$dir"/} is signed by ${primary^^}, not $expected"
}

[[ -f $dir/INDEX ]] || die "missing INDEX"
[[ -f $dir/SHA256SUMS ]] || die "missing SHA256SUMS"
verify_sig "$dir/INDEX.asc" "$dir/INDEX"
verify_sig "$dir/SHA256SUMS.asc" "$dir/SHA256SUMS"

version=""
public_tag=""
public_sha=""
development_sha=""
current=""
platforms=()
records=()
declare -A seen_field=()

while IFS= read -r line || [[ -n $line ]]; do
    if [[ -z $line ]]; then
        continue
    fi
    if [[ $line =~ ^file=([^[:space:]]+)\ size=([0-9]+)\ sha256=([0-9a-f]{64})$ ]]; then
        [[ -n $current ]] || die "index lists a file before a platform: $line"
        records+=("${current}"$'\t'"${BASH_REMATCH[1]}"$'\t'"${BASH_REMATCH[2]}"$'\t'"${BASH_REMATCH[3]}")
        continue
    fi
    [[ $line == *=* ]] || die "bad index line: $line"
    key=${line%%=*}
    val=${line#*=}
    [[ -n $key && -n $val ]] || die "bad index line: $line"
    case $key in
        version)
            [[ -z $version ]] || die "duplicate index field version"
            version=$val
            ;;
        public_tag)
            [[ -z $public_tag ]] || die "duplicate index field public_tag"
            public_tag=$val
            ;;
        public_sha)
            [[ -z $public_sha ]] || die "duplicate index field public_sha"
            public_sha=$val
            ;;
        development_sha)
            [[ -z $development_sha ]] || die "duplicate index field development_sha"
            development_sha=$val
            ;;
        platform)
            current=$val
            platforms+=("$val")
            ;;
        directory | builder_host | os_userland | compiler | image_digest | build_command | version_line)
            [[ -n $current ]] || die "index field $key before a platform"
            token="$current $key"
            [[ -z ${seen_field[$token]:-} ]] || die "duplicate index field $key for $current"
            seen_field[$token]=$val
            ;;
        *)
            die "unknown index field $key"
            ;;
    esac
done <"$dir/INDEX"

[[ -n $version ]] || die "index missing version"
[[ -n $public_tag ]] || die "index missing public_tag"
[[ -n $public_sha ]] || die "index missing public_sha"
[[ -n $development_sha ]] || die "index missing development_sha"
[[ $version == "$public_tag" ]] || die "index version $version is not public_tag $public_tag"
[[ $version =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || die "index version is not vX.Y.Z: $version"
[[ $public_sha =~ ^[0-9a-f]{40}$ ]] || die "index public_sha is not 40 hex digits"
[[ $development_sha =~ ^[0-9a-f]{40}$ ]] || die "index development_sha is not 40 hex digits"
[[ ${#platforms[@]} -gt 0 ]] || die "index lists no platform"

declare -A platform_once=()
for platform in "${platforms[@]}"; do
    [[ -z ${platform_once[$platform]:-} ]] || die "duplicate platform $platform in index"
    platform_once[$platform]=1
    for key in directory builder_host os_userland compiler image_digest build_command version_line; do
        [[ -n ${seen_field[$platform $key]:-} ]] || die "index missing $key for $platform"
    done
    [[ ${seen_field[$platform directory]} == "$platform" ]] || die "index directory for $platform is not $platform"
    [[ -d $dir/$platform ]] || die "missing platform directory $platform"
    [[ -f $dir/$platform/SHA256SUMS ]] || die "missing SHA256SUMS in $platform"
    verify_sig "$dir/$platform/SHA256SUMS.asc" "$dir/$platform/SHA256SUMS"
done

check_sums() {
    local where=$1
    local output
    if ! output=$(cd -- "$where" && sha256sum --check SHA256SUMS 2>&1); then
        die "checksum mismatch in ${where#"$dir"}: $output"
    fi
}

check_sums "$dir"
for platform in "${platforms[@]}"; do
    check_sums "$dir/$platform"
done

expected_top=$(
    {
        printf '%s\n' INDEX
        for platform in "${platforms[@]}"; do
            printf '%s\n' "$platform/SHA256SUMS"
        done
    } | sort
)
actual_top=$(awk '{ print $2 }' "$dir/SHA256SUMS" | sort)
[[ $expected_top == "$actual_top" ]] || die "top-level SHA256SUMS is not the platform manifests plus INDEX"

for platform in "${platforms[@]}"; do
    sums_list=$(awk '{ print $2 }' "$dir/$platform/SHA256SUMS" | sort)
    index_list=$(
        for record in "${records[@]}"; do
            IFS=$'\t' read -r record_platform record_name _record_size _record_hash <<<"$record"
            if [[ $record_platform == "$platform" ]]; then
                printf '%s\n' "$record_name"
            fi
        done | sort
    )
    [[ $sums_list == "$index_list" ]] || die "index and SHA256SUMS name different files in $platform"

    while IFS= read -r -d '' name; do
        case $name in
            SHA256SUMS | SHA256SUMS.asc) continue ;;
        esac
        if ! grep -Fxq -- "$name" <<<"$sums_list"; then
            die "extra file in $platform: $name"
        fi
    done < <(find "$dir/$platform" -maxdepth 1 -type f -printf '%f\0')

    while IFS= read -r -d '' extra; do
        die "extra path in $platform: ${extra#"$dir/$platform"/}"
    done < <(find "$dir/$platform" -mindepth 1 \( -type d -o -type l \) -print0)
done

for record in "${records[@]}"; do
    IFS=$'\t' read -r platform name size hash <<<"$record"
    path=$dir/$platform/$name
    [[ -f $path ]] || die "missing file $platform/$name"
    actual_size=$(stat -c %s -- "$path")
    [[ $actual_size == "$size" ]] || die "size mismatch for $platform/$name: index $size, file $actual_size"
    actual_hash=$(sha256sum -- "$path" | awk '{ print $1 }')
    [[ $actual_hash == "$hash" ]] || die "index hash mismatch for $platform/$name"
done

while IFS= read -r -d '' entry; do
    base=$(basename -- "$entry")
    if [[ -d $entry && ! -L $entry ]]; then
        [[ -n ${platform_once[$base]:-} ]] || die "extra directory in release: $base"
    elif [[ -f $entry && ! -L $entry ]]; then
        case $base in
            INDEX | INDEX.asc | SHA256SUMS | SHA256SUMS.asc) ;;
            *) die "extra file in release: $base" ;;
        esac
    else
        die "extra path in release: $base"
    fi
done < <(find "$dir" -mindepth 1 -maxdepth 1 -print0)
