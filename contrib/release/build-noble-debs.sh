#!/usr/bin/env bash
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
export LC_ALL=C
set -euo pipefail

exec "$(dirname "$0")/build-debs.sh" noble "$@"
