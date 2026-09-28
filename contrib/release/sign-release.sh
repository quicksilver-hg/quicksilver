#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Make detached armored signatures for a staged release directory.
# Signs INDEX and every SHA256SUMS. Refuses a short key id, a fingerprint
# that is not the secret key's own, and any signature file that already
# exists. Verifies each signature it just wrote, and pins the VALIDSIG
# primary fingerprint, before exiting 0. Does not upload.
#
# QS_GPG_PASSPHRASE, when set (including to the empty string), is passed
# to gpg with a loopback pinentry. Leave it unset for a real project key
# so gpg-agent handles the passphrase. Tests set it only for a throwaway
# key they create in a temporary GNUPGHOME.

export LC_ALL=C
set -euo pipefail

die() {
    echo "ERROR: $*" >&2
    exit 1
}

if [[ $# -ne 2 ]]; then
    echo "Usage: sign-release.sh STAGED_DIR FINGERPRINT" >&2
    exit 2
fi

[[ -d $1 ]] || die "not a directory: $1"
dir=$(realpath -- "$1")
fpr=$2
# [[:xdigit:]] rather than a letter range: under LC_ALL=C the range a-F is empty.
[[ "$fpr" =~ ^[[:xdigit:]]{40}$ ]] || die "fingerprint must be the full 40 hex digits, not a short key id"
expected=${fpr^^}

listed=$(gpg --batch --with-colons --list-secret-keys "$expected" 2>/dev/null | awk -F: '$1 == "fpr" { print $10; exit }' || true)
[[ -n $listed ]] || die "no secret key with fingerprint $expected"
[[ ${listed^^} == "$expected" ]] || die "key fingerprint ${listed^^} does not match $expected"

[[ -f $dir/INDEX ]] || die "missing INDEX"
[[ -f $dir/SHA256SUMS ]] || die "missing SHA256SUMS"

targets=("$dir/INDEX" "$dir/SHA256SUMS")
platforms=()
while IFS= read -r -d '' sub; do
    platforms+=("$sub")
done < <(find "$dir" -mindepth 1 -maxdepth 1 -type d -print0 | sort -z)
[[ ${#platforms[@]} -gt 0 ]] || die "no platform directories"
for sub in "${platforms[@]}"; do
    [[ -f $sub/SHA256SUMS ]] || die "missing SHA256SUMS in ${sub#"$dir"/}"
    targets+=("$sub/SHA256SUMS")
done

for target in "${targets[@]}"; do
    [[ ! -e ${target}.asc ]] || die "signature file already exists: ${target#"$dir"/}.asc"
done

gpg_args=(--batch --yes --armor --detach-sign --local-user "$expected")
if [[ -v QS_GPG_PASSPHRASE ]]; then
    gpg_args+=(--pinentry-mode loopback --passphrase "$QS_GPG_PASSPHRASE")
fi

created=()
cleanup_sigs() {
    local signature
    for signature in "${created[@]}"; do
        rm -f -- "$signature"
    done
}

verify_sig() {
    local signature=$1 file=$2
    local status primary
    if ! status=$(gpg --batch --status-fd 1 --verify -- "$signature" "$file" 2>/dev/null); then
        echo "ERROR: signature verification failed for ${file#"$dir"/}" >&2
        return 1
    fi
    primary=$(printf '%s\n' "$status" | awk '/^\[GNUPG:\] VALIDSIG / { fingerprint = $NF } END { print fingerprint }')
    if [[ -z $primary ]]; then
        echo "ERROR: no VALIDSIG status for ${file#"$dir"/}; refusing an unpinned signature" >&2
        return 1
    fi
    if [[ ${primary^^} != "$expected" ]]; then
        echo "ERROR: ${file#"$dir"/} is signed by ${primary^^}, not $expected" >&2
        return 1
    fi
}

for target in "${targets[@]}"; do
    created+=("${target}.asc")
    if ! gpg "${gpg_args[@]}" --output "${target}.asc" -- "$target"; then
        cleanup_sigs
        die "gpg failed while signing ${target#"$dir"/}"
    fi
done

for target in "${targets[@]}"; do
    if ! verify_sig "${target}.asc" "$target"; then
        cleanup_sigs
        exit 1
    fi
done
