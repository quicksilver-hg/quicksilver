#!/usr/bin/env bash
#
# Copyright (c) 2018-present The Bitcoin Core developers
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -ex

if [ -n "$CIRRUS_PR" ]; then
  export COMMIT_RANGE="HEAD~..HEAD"
  if [ "$(git rev-list -1 HEAD)" != "$(git rev-list -1 --merges HEAD)" ]; then
    echo "Error: The top commit must be a merge commit, usually the remote 'pull/${PR_NUMBER}/merge' branch."
    false
  fi
fi

if [ -z "$COMMIT_RANGE" ] && [ "$(git rev-list -1 HEAD)" = "$(git rev-list -1 --merges HEAD)" ]; then
  # HEAD is a merge commit, so the test runner's default range (last merge..HEAD) would be
  # empty, and it now refuses to lint an empty range instead of passing having read nothing.
  # Lint what the merge brought in.
  export COMMIT_RANGE="HEAD^..HEAD"
fi

RUST_BACKTRACE=1 "${LINT_RUNNER_PATH}/test_runner"
