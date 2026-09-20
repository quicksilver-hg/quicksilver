#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# Update an existing public source repository from a reviewed development
# commit. export-public-source.sh initialises a public repository; this script
# is the one to use for every publication after that.
#
# They are not interchangeable. The exporter runs `git init`, so using it to
# publish an update produces a fresh unrelated root every time: each push has to
# be forced, the public history is destroyed, and existing CI runs and commit
# references are orphaned. This script commits onto the published branch instead,
# so pushes fast-forward and the public history is a readable record.
#
# What is published is a tree, never history. The publication repository holds
# only public commits; the development checkout's branches are never pushed and
# it should not even have the public remote configured.

export LC_ALL=C
set -euo pipefail

message=""
while getopts ":m:" opt; do
    case "$opt" in
        m) message=$OPTARG ;;
        :) echo "Option -$OPTARG requires an argument." >&2; exit 1 ;;
        \?) echo "Unknown option: -$OPTARG" >&2; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

# ------------------------------------------------------------- message checks
# A published commit is the project's own work, authored by The Quicksilver
# developers. Assistant attribution belongs in the development history, which is
# never pushed. A trailer that crosses this boundary puts a tool's name on the
# public record permanently: the branch is published, so removing it afterwards
# means rewriting history every clone already has.
#
# Names are deliberately NOT what is matched. The genesis mark itself names the
# agents (src/kernel/chainparams.cpp), and a legitimate message may quote it.
# These patterns are attribution markers only.
#
# Checked before anything else so a bad message costs nothing: neither
# repository has been touched at this point.
attribution_patterns=(
    '^[[:space:]]*co-authored-by:'
    '^[[:space:]]*(signed-off-by|assisted-by|generated-by):.*(anthropic|openai|claude|codex|grok)'
    'noreply@anthropic\.com'
    'claude\.com/claude-code'
    'generated with \[?claude'
)

if [[ -n "$message" ]]; then
    # One alternation rather than a loop, so a line matching two patterns is
    # reported once.
    attribution_re=$(IFS='|'; echo "${attribution_patterns[*]}")
    offending=$(printf '%s\n' "$message" | grep -inE "$attribution_re" || true)
    if [[ -n "$offending" ]]; then
        echo "Refusing to publish: the commit message carries assistant attribution." >&2
        printf '%s\n' "$offending" >&2
        echo "Published commits are authored by The Quicksilver developers alone." >&2
        echo "Strip the trailer and retry. Nothing has been touched." >&2
        exit 1
    fi
fi

if [[ $# -lt 1 || $# -gt 2 ]]; then
    cat >&2 <<'USAGE'
Usage: publish-source.sh [-m MESSAGE] PUBLICATION_REPO [SOURCE_COMMIT]

  PUBLICATION_REPO  an existing clone of the public repository, checked out on
                    the branch to publish onto
  SOURCE_COMMIT     the reviewed development commit to publish (default: HEAD)

Without -m the tree is staged and verified but not committed, so the diff can be
reviewed first. Nothing is ever pushed; the command to run is printed at the end.

A message given with -m is refused if it carries assistant attribution: published
commits are authored by The Quicksilver developers alone, and a trailer on a
pushed branch cannot be removed without rewriting published history.
USAGE
    exit 1
fi

publication=$(realpath -- "$1")
source_commit=${2:-HEAD}

source_root=$(realpath -- "$(git rev-parse --show-toplevel)")

if [[ ! -d "$publication/.git" ]]; then
    echo "Not a git repository: $publication" >&2
    exit 1
fi

case "$publication/" in
    "$source_root/"*)
        echo "Refusing to publish into a directory inside the source tree." >&2
        exit 1
        ;;
esac

# ---------------------------------------------------------------- source checks
if [[ -n $(git -C "$source_root" status --porcelain) ]]; then
    echo "Refusing to publish from a dirty source tree. Commit or remove every change first." >&2
    exit 1
fi

source_sha=$(git -C "$source_root" rev-parse --verify "$source_commit^{commit}")
source_tree=$(git -C "$source_root" rev-parse --verify "$source_commit^{tree}")

# The same guard list as export-public-source.sh. A private path that became
# tracked must never reach the public repository, on any commit.
while IFS= read -r tracked_path; do
    case "$tracked_path" in
        .agents|.agents/*|.claude|.claude/*|.codex|.codex/*|.grok|.grok/*|\
        .remember|.remember/*|.superpowers|.superpowers/*|.worktrees|.worktrees/*|\
        docs/superpowers|docs/superpowers/*)
            echo "Refusing to publish tracked private path: $tracked_path" >&2
            exit 1
            ;;
    esac
done < <(git -C "$source_root" ls-tree -r --name-only "$source_sha")

# ----------------------------------------------------------- publication checks
publication_branch=$(git -C "$publication" symbolic-ref --quiet --short HEAD || true)
if [[ -z "$publication_branch" ]]; then
    echo "The publication repository has a detached HEAD. Check out the branch to publish onto." >&2
    exit 1
fi

if [[ -n $(git -C "$publication" status --porcelain) ]]; then
    echo "Refusing to publish into a dirty publication repository." >&2
    exit 1
fi

# Ignored leftovers would survive the sync and could be committed by `add -f`.
# A publication repository is not a build directory.
if [[ -n $(git -C "$publication" clean -ndx) ]]; then
    echo "Refusing to publish: the publication repository holds untracked or ignored files." >&2
    git -C "$publication" clean -ndx >&2
    echo "Remove them (git clean -fdx) and retry." >&2
    exit 1
fi

# Build on what is actually published, so the push is a fast-forward.
upstream="refs/remotes/origin/$publication_branch"
if git -C "$publication" rev-parse --verify --quiet "$upstream" >/dev/null; then
    if ! git -C "$publication" merge-base --is-ancestor "$upstream" HEAD; then
        echo "The publication branch has diverged from $upstream. Fetch and reconcile first." >&2
        exit 1
    fi
else
    echo "No $upstream yet; publishing onto a branch that does not exist on the remote."
fi

# ------------------------------------------------------------------- sync tree
# Remove the tracked set first so deletions propagate, then write the reviewed
# blobs. checkout-index rather than `git archive`: archive expands export-subst
# and would bake a development commit hash into src/clientversion.cpp, where a
# real clone has the literal placeholder.
git -C "$publication" ls-files -z | (cd "$publication" && xargs -0 --no-run-if-empty rm -f)
find "$publication" -mindepth 1 -path "$publication/.git" -prune -o -type d -empty -print0 |
    xargs -0 --no-run-if-empty rmdir --ignore-fail-on-non-empty

# checkout-index reads an index, not a commit, so give it a temporary one holding
# exactly the tree being published. This also keeps the development index and
# working tree untouched.
scratch_index=$(mktemp)
trap 'rm -f -- "$scratch_index"' EXIT
GIT_INDEX_FILE="$scratch_index" git -C "$source_root" read-tree "$source_tree"
GIT_INDEX_FILE="$scratch_index" git -C "$source_root" \
    checkout-index --all --force --prefix="$publication/"

# `add -A` alone silently drops tracked files that the tree's own .gitignore
# matches. --force is what makes the staged set the whole tracked set.
git -C "$publication" add -A --force

# ---------------------------------------------------------------- the invariant
# One comparison decides whether this is safe: if the staged tree is the reviewed
# tree, then every published byte is a reviewed byte, and nothing else came along.
# Never check this by file count or by eye.
staged_tree=$(git -C "$publication" write-tree)
if [[ "$staged_tree" != "$source_tree" ]]; then
    echo "Staged tree $staged_tree does not match source tree $source_tree." >&2
    echo "Refusing to publish a tree that is not the reviewed one." >&2
    exit 1
fi
echo "Tree verified: $staged_tree == $source_commit^{tree}"

if [[ -z "$message" ]]; then
    echo
    echo "Staged onto '$publication_branch' in $publication (not committed)."
    echo "Review with: git -C $publication diff --cached"
    echo "Then commit, and push with:"
    echo "  git -C $publication push origin $publication_branch"
    echo
    echo "⚠ Committing by hand skips the message check this script runs for -m."
    echo "  A published commit carries no Co-Authored-By trailer and no assistant"
    echo "  attribution; strip both before committing."
    exit 0
fi

git -C "$publication" commit --quiet -m "$message"
published_sha=$(git -C "$publication" rev-parse HEAD)

# The commit must carry the tree unchanged.
if [[ "$(git -C "$publication" rev-parse "HEAD^{tree}")" != "$source_tree" ]]; then
    echo "Committed tree does not match the source tree. Do not push." >&2
    exit 1
fi

echo "Committed $published_sha on '$publication_branch'"
echo "Source commit: $source_sha"
echo
echo "Nothing has been pushed. To publish:"
echo "  git -C $publication push origin $publication_branch"
echo "To send it to CI on a branch instead, without touching the published branch:"
echo "  git -C $publication push origin HEAD:ci/<name>"
