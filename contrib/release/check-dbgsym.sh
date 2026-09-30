#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# F-391. Install one release's .deb and .ddeb set in a fresh container of
# the same suite and refuse it when gdb cannot symbolise main. build-debs.sh
# runs this after its package-set checks. verify-release.sh does not: that
# script is the stranger's check, and it stays free of containers and root.
export LC_ALL=C
set -euo pipefail

usage() {
    echo "Usage: $0 {jammy|noble} DIRECTORY" >&2
    exit 2
}

# One gdb transcript on stdin. Print one verdict and exit 0 only for ok.
# "<line> ./<file>.cpp: No such file or directory" is gdb trying to list
# source this package does not ship. The frame above it already names the
# function and the file:line, so that line is not a failure.
judge_transcript() {
    awk '
    BEGIN {
        could = 0
        dwz = 0
        saw_main = 0
        saw_line = 0
    }
    function is_main_frame(line) {
        return line ~ /^#0[[:space:]]+main([[:space:]]|\()/ \
            || line ~ /^#0[[:space:]]+0x[0-9a-fA-F]+[[:space:]]+in[[:space:]]+main([[:space:]]|\()/
    }
    function has_file_line(line) {
        return line ~ /[[:space:]]at[[:space:]][^[:space:]]+:[0-9]+[[:space:]]*$/
    }
    {
        if (index($0, "could not find") > 0) {
            could = 1
        }
        if (index($0, "dwz file") > 0) {
            dwz = 1
        }
        if (is_main_frame($0)) {
            saw_main = 1
            if (has_file_line($0)) {
                saw_line = 1
            }
        }
    }
    END {
        if (could) {
            print "FAIL: gdb printed \"could not find\""
            exit 1
        }
        if (dwz) {
            print "FAIL: gdb printed \"dwz file\""
            exit 1
        }
        if (saw_main && saw_line) {
            print "ok"
            exit 0
        }
        if (saw_main) {
            print "FAIL: main frame has no file:line"
            exit 1
        }
        print "FAIL: gdb did not stop in main"
        exit 1
    }
    '
}

is_elf() {
    local magic
    magic=$(od -An -t x1 -N 4 -- "$1" | tr -d ' \n')
    [[ $magic == 7f454c46 ]]
}

run_inside() {
    local debs ddebs deb pkg status transcript verdict fail checked paths path
    export DEBUGINFOD_URLS=
    apt-get update
    apt-get install -y --no-install-recommends gdb
    shopt -s nullglob
    debs=(/packages/*.deb)
    ddebs=(/packages/*.ddeb)
    if [[ ${#debs[@]} -eq 0 || ${#ddebs[@]} -eq 0 ]]; then
        echo "F-391: /packages has no .deb and .ddeb pair" >&2
        exit 1
    fi
    if ! dpkg -i "${debs[@]}" "${ddebs[@]}"; then
        apt-get install -y -f --no-install-recommends
    fi

    local -a installed=()
    for deb in "${debs[@]}" "${ddebs[@]}"; do
        pkg=$(dpkg-deb -f "$deb" Package)
        status=$(dpkg-query -W -f '${Status}' "$pkg" 2>/dev/null || true)
        if [[ $status != "install ok installed" ]]; then
            echo "F-391: $pkg is not installed (${status:-absent})" >&2
            exit 1
        fi
        installed+=("$pkg")
    done

    if ! paths=$(dpkg -L "${installed[@]}"); then
        echo "F-391: dpkg -L failed" >&2
        exit 1
    fi

    fail=0
    checked=0
    while IFS= read -r path; do
        [[ $path == /usr/bin/* ]] || continue
        [[ -f $path && ! -L $path ]] || continue
        is_elf "$path" || continue
        checked=$((checked + 1))
        # Stop at main, before the program opens a display. The GUI debug
        # file is large: resolving main took about a minute, and a 60s bound
        # killed that run after the breakpoint was already placed. 180s still
        # fails a binary that never hits main, instead of hanging the build.
        # No `set complaints 0`: a dwz complaint must stay visible.
        transcript=$(timeout --kill-after=10 180 gdb -batch \
            -ex 'set pagination off' \
            -ex 'break main' \
            -ex run \
            -ex bt \
            --args "$path" -version 2>&1 || true)
        if [[ -n ${QS_DBGSYM_TRANSCRIPT_DIR:-} ]]; then
            printf '%s\n' "$transcript" >"${QS_DBGSYM_TRANSCRIPT_DIR}/$(basename -- "$path").gdb"
        fi
        if verdict=$(/check-dbgsym.sh --judge <<<"$transcript"); then
            printf '%s %s\n' "$verdict" "$path"
        else
            printf 'FAIL %s: %s (F-391)\n' "$path" "${verdict#FAIL: }"
            fail=1
        fi
    done <<<"$paths"

    if [[ $checked -eq 0 ]]; then
        echo "F-391: installed packages contain no ELF binary under /usr/bin" >&2
        exit 1
    fi
    if [[ $fail -ne 0 ]]; then
        echo "F-391: a -dbgsym package did not symbolise"
        exit 1
    fi
}

if [[ ${1:-} == --judge ]]; then
    judge_transcript
    exit
fi

if [[ ${1:-} == --inside ]]; then
    run_inside
    exit
fi

[[ $# == 2 ]] || usage
suite=$1
case "$suite" in
    jammy|noble) ;;
    *) usage ;;
esac

directory=$(realpath -m -- "$2")
if [[ ! -d $directory ]]; then
    echo "not a directory: $directory" >&2
    exit 2
fi

script=$(readlink -f -- "$0")
here=$(dirname -- "$script")
case "$suite" in
    jammy) dockerfile=jammy.Dockerfile ;;
    noble) dockerfile=noble.Dockerfile ;;
esac
from_line=$(grep -m1 '^FROM ubuntu@' "$here/$dockerfile" || true)
image=${from_line#FROM }
if [[ $image != ubuntu@sha256:* ]]; then
    echo "could not read a pinned Ubuntu digest from $dockerfile" >&2
    exit 1
fi

docker_args=(
    --rm
    --cap-add=SYS_PTRACE
    --security-opt seccomp=unconfined
    --volume "$directory:/packages:ro"
    --volume "$script:/check-dbgsym.sh:ro"
    --env DEBIAN_FRONTEND=noninteractive
    --env DEBUGINFOD_URLS=
    --entrypoint /check-dbgsym.sh
)
if [[ -n ${QS_DBGSYM_TRANSCRIPT_DIR:-} ]]; then
    QS_DBGSYM_TRANSCRIPT_DIR=$(realpath -m -- "$QS_DBGSYM_TRANSCRIPT_DIR")
    mkdir -p "$QS_DBGSYM_TRANSCRIPT_DIR"
    docker_args+=(--volume "$QS_DBGSYM_TRANSCRIPT_DIR:/transcripts")
    docker_args+=(--env QS_DBGSYM_TRANSCRIPT_DIR=/transcripts)
fi

docker run "${docker_args[@]}" "$image" --inside
