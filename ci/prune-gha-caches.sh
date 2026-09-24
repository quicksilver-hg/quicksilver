#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C
set -u -o pipefail

if [ "$#" -ne 6 ]; then
    echo "Usage: $0 <owner/repo> <ref> <key-prefix> <keep-count> <protected-key> <--apply|--dry-run>" >&2
    exit 2
fi

repository=$1
ref=$2
prefix=$3
keep_count=$4
protected_key=$5
mode=$6

case "$repository" in
    */*) ;;
    *) echo "ERROR: repository must be in owner/repo form" >&2; exit 2 ;;
esac
case "$ref" in
    refs/*) ;;
    *) echo "ERROR: ref must begin with refs/" >&2; exit 2 ;;
esac
if [ -z "$prefix" ]; then
    echo "ERROR: key prefix must not be empty" >&2
    exit 2
fi
case "$keep_count" in
    ''|*[!0-9]*|0) echo "ERROR: keep-count must be a positive integer" >&2; exit 2 ;;
esac
case "$mode" in
    --apply|--dry-run) ;;
    *) echo "ERROR: mode must be --apply or --dry-run" >&2; exit 2 ;;
esac

cache_pages=$(mktemp)
selection=$(mktemp)
trap 'rm -f "$cache_pages" "$selection"' EXIT

echo "CACHE_PRUNE list repository=$repository ref=$ref prefix=$prefix keep=$keep_count"
if ! gh api --method GET --paginate \
    -H 'Accept: application/vnd.github+json' \
    -H 'X-GitHub-Api-Version: 2022-11-28' \
    -f "ref=$ref" \
    -f "key=$prefix" \
    -f per_page=100 \
    --jq '.actions_caches[]' \
    "repos/$repository/actions/caches" > "$cache_pages"; then
    echo "CACHE_PRUNE ERROR: could not list caches; no deletion was attempted" >&2
    exit 1
fi

if ! jq --slurp --arg ref "$ref" --arg prefix "$prefix" --arg protected "$protected_key" --argjson keep "$keep_count" '
    [.[] | select(.ref == $ref and (.key | startswith($prefix)))]
    | sort_by(.created_at, .id)
    | reverse
    | to_entries
    | map(.value + {
        disposition: (if .key < $keep then "keep"
                      elif $protected != "" and .value.key == $protected then "protected"
                      else "delete" end)
      })
  ' "$cache_pages" > "$selection"; then
    echo "CACHE_PRUNE ERROR: could not select caches; no deletion was attempted" >&2
    exit 1
fi

# jq on the Windows runner is a native build that may end lines with CRLF;
# a stray \r breaks the numeric tests below.
cache_count=$(jq 'length' "$selection" | tr -d '\r')
delete_count=$(jq '[.[] | select(.disposition == "delete")] | length' "$selection" | tr -d '\r')
echo "CACHE_PRUNE matched=$cache_count delete=$delete_count"
jq -r '.[] | "CACHE_PRUNE \(.disposition | ascii_upcase) id=\(.id) created=\(.created_at) bytes=\(.size_in_bytes) key=\(.key)"' "$selection"

if [ "$delete_count" -eq 0 ]; then
    echo "CACHE_PRUNE nothing to delete"
    exit 0
fi
if [ "$mode" = "--dry-run" ]; then
    echo "CACHE_PRUNE dry run complete; no cache was deleted"
    exit 0
fi

failures=0
while IFS=$'\t' read -r cache_id cache_key; do
    if gh api --method DELETE \
        -H 'Accept: application/vnd.github+json' \
        -H 'X-GitHub-Api-Version: 2022-11-28' \
        "repos/$repository/actions/caches/$cache_id"; then
        echo "CACHE_PRUNE DELETED id=$cache_id key=$cache_key"
    else
        echo "CACHE_PRUNE ERROR: delete failed id=$cache_id key=$cache_key" >&2
        failures=$((failures + 1))
    fi
done < <(jq -r '.[] | select(.disposition == "delete") | [.id, .key] | @tsv' "$selection" | tr -d '\r')

if [ "$failures" -ne 0 ]; then
    echo "CACHE_PRUNE ERROR: $failures cache deletion(s) failed" >&2
    exit 1
fi
echo "CACHE_PRUNE deletion complete; removed=$delete_count"
