#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

export LC_ALL=C
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 NEW_EMPTY_DESTINATION" >&2
    exit 1
fi

source_root=$(git rev-parse --show-toplevel)
source_root=$(realpath -- "$source_root")
destination=$(realpath -m -- "$1")

if [[ -e "$destination" ]]; then
    echo "Refusing to reuse existing destination: $destination" >&2
    exit 1
fi

case "$destination/" in
    "$source_root/"*)
        echo "Refusing to create the public repository inside the source tree." >&2
        exit 1
        ;;
esac

if [[ -n $(git -C "$source_root" status --porcelain) ]]; then
    echo "Refusing to export a dirty source tree. Commit or remove every change first." >&2
    exit 1
fi

while IFS= read -r tracked_path; do
    case "$tracked_path" in
        .agents|.agents/*|.claude|.claude/*|.codex|.codex/*|.grok|.grok/*|\
        .remember|.remember/*|.superpowers|.superpowers/*|.worktrees|.worktrees/*|\
        docs/superpowers|docs/superpowers/*)
            echo "Refusing to export tracked private path: $tracked_path" >&2
            exit 1
            ;;
    esac
done < <(git -C "$source_root" ls-files)

mkdir -p -- "$(dirname -- "$destination")"
mkdir -- "$destination"
git -C "$source_root" archive --format=tar HEAD | tar -xf - -C "$destination"
# `git archive` expands export-subst attributes. Overlay the clean index so the
# public root contains the exact reviewed blobs instead of embedding an
# unpublished development commit hash in src/clientversion.cpp.
git -C "$source_root" checkout-index --all --force --prefix="$destination/"

git -C "$destination" init -b main

# The fresh repository inherits nothing, so a development checkout that sets its
# identity in .git/config leaves the documented root commit failing with
# "Author identity unknown". Carry the source repository's effective identity
# forward; a source repository with no identity configured is left as-is.
for identity_key in user.name user.email; do
    if identity_value=$(git -C "$source_root" config --get "$identity_key"); then
        git -C "$destination" config "$identity_key" "$identity_value"
        echo "Carried $identity_key forward: $identity_value"
    else
        echo "Source repository sets no $identity_key; set one in the public repository before committing." >&2
    fi
done

git -C "$destination" add -f --all

source_index=$(mktemp)
destination_index=$(mktemp)
trap 'rm -f -- "$source_index" "$destination_index"' EXIT
git -C "$source_root" ls-files --stage >"$source_index"
git -C "$destination" ls-files --stage >"$destination_index"

if ! diff -u "$source_index" "$destination_index"; then
    echo "Exported file content or modes differ from the source HEAD." >&2
    exit 1
fi

echo "Staged a fresh public source tree at: $destination"
echo "Source commit: $(git -C "$source_root" rev-parse HEAD)"
echo "Review the staged diff, run the release gates, then create the root commit."
