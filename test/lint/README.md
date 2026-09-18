This folder contains lint scripts.

Running locally
===============

To run linters locally with the same versions as the CI environment, use the included
Dockerfile:

```sh
DOCKER_BUILDKIT=1 docker build -t quicksilver-linter --file "./ci/lint_imagefile" ./ && docker run --rm -v $(pwd):/quicksilver -it quicksilver-linter
```

Building the container can be done every time, because it is fast when the
result is cached and it prevents issues when the image changes.

test runner
===========

To run all the lint checks in the test runner outside the docker you first need
to install the rust toolchain using your package manager of choice or
[rustup](https://www.rust-lang.org/tools/install).

Then you can use:

```sh
( cd ./test/lint/test_runner/ && cargo fmt && cargo clippy && RUST_BACKTRACE=1 cargo run )
```

The checks that read the commit history (`commit_msg`, `scripted_diff`) run over a
commit range. It defaults to `<most recent merge commit>..HEAD`, which is **empty
whenever HEAD is itself a merge commit** -- the state of the tree right after any
merge. The test runner refuses to run against an empty range rather than reporting
success having read no commits, so set the range explicitly when linting a merge:

```sh
( cd ./test/lint/test_runner/ && COMMIT_RANGE=<base>..HEAD cargo run )
```

Line 1 of the output names the range and the commits in it. Check that it names the
commits you meant to lint.

If you wish to run individual lint checks, run the test_runner with
`--lint=TEST_TO_RUN` arguments. If running with `cargo run`, arguments after
`--` are passed to the binary you are running e.g.:

```sh
( cd ./test/lint/test_runner/ && RUST_BACKTRACE=1 cargo run -- --lint=doc --lint=trailing_whitespace )
```

For the Python linters, this repository currently follows the pinned lint
dependency versions in `ci/lint/04_install.sh`. Some of those pins require
Python 3.10 when running outside the Docker lint image. If `python3` is a newer
system interpreter, use:

```sh
test/lint/run-all-python-linters-py310.sh
```

To see a list of all individual lint checks available in test_runner, use `-h`
or `--help`:

```sh
( cd ./test/lint/test_runner/ && RUST_BACKTRACE=1 cargo run -- --help )
```

#### Dependencies

| Lint test | Dependency |
|-----------|:----------:|
| [`lint-python.py`](lint-python.py) | [lief](https://github.com/lief-project/LIEF)
| [`lint-python.py`](lint-python.py) | [mypy](https://github.com/python/mypy)
| [`lint-python.py`](lint-python.py) | [pyzmq](https://github.com/zeromq/pyzmq)
| [`lint-python-dead-code.py`](lint-python-dead-code.py) | [vulture](https://github.com/jendrikseipp/vulture)
| [`lint-shell.py`](lint-shell.py) | [ShellCheck](https://github.com/koalaman/shellcheck)
| [`lint-spelling.py`](lint-spelling.py) | [codespell](https://github.com/codespell-project/codespell)
| `py_lint` | [ruff](https://github.com/astral-sh/ruff)
| [`check-doc-links.py`](check-doc-links.py) | Python standard library

In use versions and install instructions are available in the [CI setup](../../ci/lint/04_install.sh).

Please be aware that on Linux distributions all dependencies are usually available as packages, but could be outdated.

#### Running the tests

Individual tests can be run by directly calling the test script, e.g.:

```
test/lint/lint-files.py
```

check-doc.py
============
Check for missing documentation of command line options.

check-doc-links.py
==================
Checks Markdown links in every tracked text file, including source comments and
repository metadata, while excluding vendored trees. Local paths must be
file-relative and point to tracked files or directories. Markdown anchors use
GitHub-style heading slugs, including duplicate-heading suffixes. External URLs
receive scheme and shape validation only; the check never fetches the network.

Every tracked Markdown file under `doc/` must also have an inbound local link
from another file. Exact exemptions for generated or intentionally untracked
targets belong in the checker's `ALLOWED_MISSING_LINKS` set, which is empty by
default.

Run directly from the repository root:

```sh
python3 test/lint/check-doc-links.py
python3 test/lint/check-doc-links.py --self-test
```

commit-script-check.sh
======================
Verification of [scripted diffs](../../doc/developer-notes.md#scripted-diffs).
Scripted diffs are only assumed to run on the latest LTS release of Ubuntu. Running them on other operating systems
might require installing GNU tools, such as GNU sed.

lint_ignore_dirs.py
===================
Add list of common directories to ignore when running tests

lint_wrapped_prose.py
=====================
Shared matcher for banned phrases that wrap across a line break. A linter that
reads `splitlines()` and matches each rule against one line at a time cannot see
a phrase that straddles a newline: `doc/design/desktop-application.md` carried
the literal phrase "agent wallets" split as `... and agent` / `wallets.` while a
rule banning `agent wallets?` had been in force for weeks.

Adopting linters run each rule a second time over the file joined into one
whitespace-normalised string, and report only matches that cross a line
boundary. Blank lines are a hard break, continuation markers (`//`, `#`, `*`,
`>`) are stripped so wrapped comments and quotes match, and exemptions stay
per-line -- feeding an allowlist normalised text lets it match more than it was
written to match, which weakens the lint instead of strengthening it.

Adopted by `lint-quicksilver-dead-surface-residue.py`,
`lint-quicksilver-fee-residue.py` and
`lint-quicksilver-source-unit-residue.py`, each of which runs this module's
self-test on every invocation. `lint-branding-residue.py` does not need it: its
`SEARCH_RE` is a single alternation of bare words, none of which can span a
boundary, and its multi-word patterns are all exemptions -- joining there would
only let them match more.

Rules that reach this matcher must bound their wildcards. `.*` spans the whole
file once the newlines are gone.

Run directly from the repository root:

```
python3 test/lint/lint_wrapped_prose.py --self-test
```

lint-cuckatoo-blake2-include.py
===============================
Checks that the vendored `blake2.h` is only included through the owned
`blake2_prelude.h` (the header that owns the MSVC C4804 sandwich), that both
vendor preludes pull that header, and that any first-party include of
`vendor/cuckatoo.h` is preceded in the same file by `vendor_prelude.h` or
`vendor_prelude_solve.h`. Skips `src/crypto/cuckatoo/vendor/` — a fix that
edits a vendored file is the wrong fix.

Run directly from the repository root:

```
python3 test/lint/lint-cuckatoo-blake2-include.py
python3 test/lint/lint-cuckatoo-blake2-include.py --self-test
```

lint-cuckatoo-source-policy.py
==============================
Checks Quicksilver-owned Cuckatoo source policy: locale-neutral bridge code,
narrow vendored solver include seams, no CUDA in the node CMake target, and the
preserved upstream `vendor/siphashxN.h` filename.

Run directly from the repository root:

```
python3 test/lint/lint-cuckatoo-source-policy.py
python3 test/lint/lint-cuckatoo-source-policy.py --self-test
```

lint-cuckatoo-solver-filenames.py
=================================
Checks that every hardcoded Cuckatoo solver filename across `src/`, `test/`,
`cmake/` and `contrib/` names a file that actually exists in
`src/crypto/cuckatoo/`, and that `gpu/qsgpusolve.cu` still documents its exit
codes -- `gpu_solver.cpp` maps exit 4 to "no CUDA device", and nothing else
records that contract.

Run directly from the repository root:

```
python3 test/lint/lint-cuckatoo-solver-filenames.py
```

lint-desktop-packaging.py
=========================
Checks that user-facing desktop packages expose `quicksilver-qt` as the primary
application while keeping daemon, command-line, agent, and test binaries out of
the desktop package root.

lint-quicksilver-dead-surface-residue.py
========================================
Checks that removed dead surfaces do not return: BIP70/IP-payment GUI objects,
`isLegacy`/`legacy_vault`, `RollingFeeUpdate`/`amount_fee` and fee-era prose,
the `tor` network alias, `-allowignoredconf`, dictionary-form RPC compatibility
help and the `outputs_is_obj` implementation path, camelCase fund and listunspent
options (`changeAddress` / `changePosition` / `lockUnspents` /
`minimumAmount` / `maximumAmount` / `maximumCount` / `minimumSumAmount`), fee-era
`GetEffectiveValue` / `m_confirm_target`, leftover Qt settings migration,
settings compatibility quirks, `Regtest` capitalization, and public-facing
`xpub`/`xprv` wording. It also guards removed pre-Quicksilver AddrMan, vault,
coins-database, duplicate-transaction, JSON-RPC, boolean-verbosity, SLIP-44,
lab-host residue, caller-less blocking vault/agent wrappers, native-segwit
product copy, global-xpub parse errors, BIP-125 replacement copy,
GetDestinationForKey helpers, remaining AgentClient
test-only wrappers, pre-segwit GBT/upgrade-test residue, CAddress V1 disk,
coin-control and BIP70 merchant copy, bitcoin-era linearize height /
bootstrap.dat, bitcoin-era getblockstats sw* keys, the decodescript
`"segwit"` object, and leftover "agent wallet" product copy.

Run directly from the repository root:

```
python3 test/lint/lint-quicksilver-dead-surface-residue.py
python3 test/lint/lint-quicksilver-dead-surface-residue.py --self-test
```
