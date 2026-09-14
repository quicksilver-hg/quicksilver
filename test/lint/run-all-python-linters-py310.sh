#!/usr/bin/env bash
#
# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: test/lint/run-all-python-linters-py310.sh [TEST_RUNNER_ARGS...]

Run the all_python_linters lint target with python3.10 first on PATH.
This preserves the system python3 while matching the Python version needed by
the pinned lint dependencies in ci/lint/04_install.sh.

Environment:
  PYTHON310  Python 3.10 executable to use. Defaults to python3.10.
EOF
}

case "${1:-}" in
  -h|--help)
    usage
    exit 0
    ;;
esac

PYTHON310="${PYTHON310:-python3.10}"
if ! PYTHON310_PATH="$(command -v "$PYTHON310")"; then
  echo "error: python3.10 is required to run all_python_linters locally." >&2
  echo "Install Python 3.10 and the pinned lint dependencies from ci/lint/04_install.sh." >&2
  exit 127
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
TMP_PYTHON_PATH="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP_PYTHON_PATH"
}
trap cleanup EXIT

ln -s "$PYTHON310_PATH" "$TMP_PYTHON_PATH/python3"
export PATH="$TMP_PYTHON_PATH:$HOME/.local/bin:$PATH"

(
  cd "$REPO_ROOT/test/lint/test_runner"
  RUST_BACKTRACE="${RUST_BACKTRACE:-1}" cargo run --quiet -- --lint=all_python_linters "$@"
)
