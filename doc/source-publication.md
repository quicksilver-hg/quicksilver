# Publishing the Source Tree

Quicksilver's public source release is the tracked file tree of a reviewed
commit. It is not a publication of local development history, worktrees,
reflogs, ignored planning material, or build output.

`export-public-source.sh` **initializes** a public repository and is for the
first publication only. For every publication after that, use
`publish-source.sh` and the [Publishing an update](#publishing-an-update)
section below. The two are not interchangeable: the exporter runs `git init`, so
running it again produces a fresh unrelated root, which forces the push,
destroys the public history, and orphans existing CI runs and commit references.

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

Keep the external signer OFF for a full-suite gate because the checked-in man
pages describe the shipping build, which omits `-signer`. With the signer OFF,
`vault_signer.py` and `rpc_signer.py` skip visibly. Cover those tests in a
targeted signer-ON build instead of enabling the signer for the full suite.

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

## Publishing an update

Keep one long-lived clone of the public repository -- a publication repository --
and publish from there. It contains public commits only, so no development
history can leave through it even by accident. The development checkout should
not have the public remote configured at all; a repository with no remote cannot
be mispushed.

```bash
contrib/devtools/publish-source.sh -m "MESSAGE" /path/to/publication-repo [SOURCE_COMMIT]
```

The script syncs the reviewed tree into the publication repository and commits it
onto the checked-out branch, so the push is a fast-forward and the public history
becomes a readable record of what changed. It refuses a dirty source tree, a
tracked private path, a dirty or build-dirtied publication repository, and a
branch that has diverged from its remote. Omit `-m` to stage and verify without
committing, so the diff can be reviewed first. It never pushes.

What makes an update safe is one comparison, which the script performs and
refuses to continue without:

```
staged tree hash == git rev-parse <source commit>^{tree}
```

If those match, every published byte is a byte of the reviewed commit and nothing
rode along. Do not substitute a file count or a visual review. Two earlier hand
exports produced plausible trees that this check rejects:

- `git add -A` without `--force` silently drops tracked files that the tree's own
  `.gitignore` matches -- three of them here, leaving 2600 files instead of 2603.
- `git archive` expands `export-subst`, baking a development commit hash into
  `src/clientversion.cpp` where a clone has the literal `$Format:%H$`.

`publish-source.sh` avoids both -- it stages with `--force` and materializes the
tree with `checkout-index` from a scratch index rather than `git archive` -- but
the tree check is what proves it on every run.

### Sending a change to CI without publishing it

CI runs on every pushed branch, so a branch in the publication repository gets a
full CI result, on every platform, without touching the published branch:

```bash
git -C /path/to/publication-repo push origin HEAD:ci/<name>
```

Because the commit's parent is the published branch, the public diff is exactly
the change under test, and a green branch fast-forwards into it with no forced
push.

Deleting such a branch afterwards does not unpublish it: the objects remain
fetchable by commit hash. Use this for a change intended to ship, not as a
scratch pad.
