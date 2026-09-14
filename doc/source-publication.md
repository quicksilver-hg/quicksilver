# Publishing the Source Tree

Quicksilver's public source release is the tracked file tree of a reviewed
commit. It is not a publication of local development history, worktrees,
reflogs, ignored planning material, or build output.

Do not push an existing development checkout to initialize the public
repository. Create a fresh root history from `git archive` instead:

```bash
contrib/devtools/export-public-source.sh /path/to/new/quicksilver-public
```

The exporter requires a clean source checkout and a destination that does not
exist. It refuses to place the destination inside the source tree, checks that
known private workspace paths are not tracked, initializes the public branch as
`main`, force-stages every archived file despite ignore rules, suppresses
archive substitutions that would expose the development commit identifier, and
verifies that staged paths, contents, and modes exactly match the source commit.

It also copies the source repository's effective `user.name` and `user.email`
into the new repository and prints each value it carried. A fresh repository
inherits no identity, so without this the root commit below fails with *"Author
identity unknown"* whenever the development checkout sets its identity in
`.git/config` rather than in `~/.gitconfig`. If the exporter instead reports
that the source repository sets no identity, configure one in the new
repository before committing:

```bash
git config user.name "Your Name"
git config user.email "you@example.com"
```

The script deliberately stops before committing or configuring a remote. In the
new repository, review the staged tree and create a local root commit so every
version and lint check has a real `HEAD`:

```bash
git diff --cached --stat
git status --short
git commit -m "Initial Quicksilver source release"
```

Then run the source-release gates against that commit:

Every optional feature is off unless asked for, and a feature that is off both
skips its own tests and changes what the binaries document. `-DWITH_ZMQ=ON` is
required: the checked-in `doc/man/*.1` pages are generated from a ZMQ-enabled
build, so without it `feature_generated_docs.py` correctly fails with ten
`zmqpub*` options the binary no longer accepts. `-DWITH_USDT=ON` registers no
command-line options and cannot affect the man pages, but without it the four
`interface_usdt_*.py` tests skip silently. Do not drop either flag to make a
gate quieter -- a skipped test is not a passing one.

```bash
cmake -B build -DWITH_ZMQ=ON -DWITH_USDT=ON -DBUILD_GUI=ON -DBUILD_TESTS=ON \
      -DBUILD_BENCH=ON -DBUILD_FUZZ_BINARY=ON
cmake --build build
ctest --test-dir build
build/test/functional/test_runner.py --extended --ci --coverage
(cd test/lint/test_runner && env COMMIT_RANGE=HEAD cargo run --quiet)
```

If a gate requires a source change, make and review that change in the
development checkout, commit it there, and create a new export. Do not patch the
public copy independently. Only the fresh repository's tested `main` root
commit is published; the development checkout remains an unpublished
implementation workspace.

## Replacing a placeholder repository

If the public repository was initialized with a placeholder commit, the fresh
root cannot fast-forward it. Fetch and inspect that remote branch from the
fresh repository, record the exact commit being replaced, and use a
lease-protected update:

```bash
git remote add origin git@github.com:quicksilver-hg/quicksilver.git
git fetch origin main
git log --oneline --decorate --graph --all
expected_remote_main=$(git rev-parse refs/remotes/origin/main)
git push --force-with-lease=refs/heads/main:"$expected_remote_main" origin main:main
```

The lease makes the update fail if the remote changed after inspection. Never
push the development checkout or its branches to the public remote.
