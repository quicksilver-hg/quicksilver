Contents
========
This directory contains tools for developers working on this repository.

export-public-source.sh
=======================

Exports a clean reviewed `HEAD` into a new repository with a fresh `main`
history. The script refuses to reuse a destination, checks for tracked private
workspace paths, stages the exact reviewed file contents and modes without
archive substitutions, and stops before the root commit so the export can be
reviewed. See
[`doc/source-publication.md`](../../doc/source-publication.md).

deterministic-fuzz-coverage
===========================

A tool to check for non-determinism in fuzz coverage. To get the help, run:

```
RUST_BACKTRACE=1 cargo run --manifest-path ./contrib/devtools/deterministic-fuzz-coverage/Cargo.toml -- --help
```

To execute the tool, compilation has to be done with the build options
`-DCMAKE_C_COMPILER='clang' -DCMAKE_CXX_COMPILER='clang++'
-DBUILD_FOR_FUZZING=ON -DCMAKE_CXX_FLAGS='-fPIC -fprofile-instr-generate
-fcoverage-mapping'`. Both llvm-profdata and llvm-cov must be installed. Also,
the qa-assets repository must have been cloned. Finally, a fuzz target has to
be picked before running the tool:

```
RUST_BACKTRACE=1 cargo run --manifest-path ./contrib/devtools/deterministic-fuzz-coverage/Cargo.toml -- $PWD/build_dir $PWD/qa-assets/corpora-dir fuzz_target_name
```

clang-thread-safety.py
======================

Replays a clang `compile_commands.json` with `-fsyntax-only
-Werror=thread-safety`.

g++ expands the annotations in `src/threadsafety.h` to empty macros, and a
default build does not pass `-Werror`, so a green local build has not run
this analysis. The script refuses a database whose compiler is not clang, a
missing database, and a selection that matches no translation unit.

Generate the headers the build produces before running it: `generate_build_info`,
the Qt `*_autogen` targets when the GUI is configured, and the embedded
raw/JSON headers (`target_raw_data_sources` / `target_json_data_sources`,
produced when the bench, test, and univalue test targets are built). A
generated source that is not on disk is named in the output and the script
exits non-zero.

```
contrib/devtools/clang-thread-safety.py --build-dir build --whole-tree
contrib/devtools/clang-thread-safety.py --build-dir build --changed HEAD~1
```

`--whole-tree` checks every translation unit in the database.
`--changed REVSPEC` checks only those whose source file `git diff REVSPEC`
names; it prints header paths it did not follow. `--jobs` defaults to 10.

clang-format-diff.py
===================

A script to format unified git diffs according to [.clang-format](../../src/.clang-format).

Requires `clang-format`, installed e.g. via `brew install clang-format` on macOS,
or `sudo apt install clang-format` on Debian/Ubuntu.

For instance, to format the last commit with 0 lines of context,
the script should be called from the git root folder as follows.

```
git diff -U0 HEAD~1.. | ./contrib/devtools/clang-format-diff.py -p1 -i -v
```

copyright\_header.py
====================

Provides utilities for managing copyright headers of `The Bitcoin Core
developers` and `The Quicksilver developers` in repository source files. It has
five subcommands:

```
$ ./copyright_header.py report <base_directory> [verbose]
$ ./copyright_header.py update <base_directory>
$ ./copyright_header.py insert <file>
$ ./copyright_header.py add-quicksilver <base_directory>
$ ./copyright_header.py verify <base_directory>
```
Running these subcommands without arguments displays a usage string.

copyright\_header.py report \<base\_directory\> [verbose]
---------------------------------------------------------

Produces a report of all copyright header notices found inside the source files
of a repository. Useful to quickly visualize the state of the headers.
Specifying `verbose` will list the full filenames of files of each category.

copyright\_header.py update \<base\_directory\> [verbose]
---------------------------------------------------------
Updates all the copyright headers of `The Bitcoin Core developers` which were
changed in a year more recent than is listed. For example:
```
// Copyright (c) <firstYear>-<lastYear> The Bitcoin Core developers
```
will be updated to:
```
// Copyright (c) <firstYear>-<lastModifiedYear> The Bitcoin Core developers
```
where `<lastModifiedYear>` is obtained from the `git log` history.

This subcommand also handles copyright headers that have only a single year. In
those cases:
```
// Copyright (c) <year> The Bitcoin Core developers
```
will be updated to:
```
// Copyright (c) <year>-<lastModifiedYear> The Bitcoin Core developers
```
where the update is appropriate.

copyright\_header.py insert \<file\>
------------------------------------
Inserts a copyright header for `The Quicksilver developers` at the top of the
file in either Python or C++ style as determined by the file extension. If the
file is a Python file and it has  `#!` starting the first line, the header is
inserted in the line below it.

The copyright dates will be set to be `<year_introduced>-<current_year>` where
`<year_introduced>` is according to the `git log` history. If
`<year_introduced>` is equal to `<current_year>`, it will be set as a single
year rather than two hyphenated years.

copyright\_header.py add-quicksilver \<base\_directory\>
-------------------------------------------------------
Adds a `Copyright (c) 2026 The Quicksilver developers` line directly below the
retained `The Bitcoin Core developers` attribution in every in-scope source file
that carries it, mirroring the file's comment prefix (`//`, `#`, or plain text):
```
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
```
Upstream Bitcoin Core attribution is never removed — the addition is purely
additive. The subcommand is idempotent: files already carrying the Quicksilver
line are left untouched.

copyright\_header.py verify \<base\_directory\>
-----------------------------------------------
Exits non-zero on any of four conditions. This is the check enforced in CI by
`test/lint/lint-copyright.py`.

1. An in-scope file carries a `The Bitcoin Core developers` copyright but is
   missing the `The Quicksilver developers` line.
2. A first-party file (see `QUICKSILVER_HEADER_REQUIRED_PREFIXES`) has no
   copyright header at all.
3. A file carries a Quicksilver line and **no** upstream holder line — a claim
   of sole authorship — without being declared in
   `test/lint/first-party-files.txt`. A file entering this state has usually
   had its upstream attribution stripped by accident; the fix is to restore the
   upstream line, not to add the path to the list.
4. A path declared in `test/lint/first-party-files.txt` is stale: it no longer
   claims sole authorship, or is no longer tracked. (Checked only when
   `<base_directory>` is the repository root, since a narrower scan cannot tell
   a stale entry from one it simply did not visit.)

Conditions 1 and 3 are deliberate mirrors. Checking only the first lets a
derived file quietly lose its Bitcoin Core attribution, which is exactly what
happened to `src/util/transaction_identifier.h` and fourteen of its neighbours.

gen-manpages.py
===============

A small script to automatically create manpages in ../../doc/man by running the release binaries with the -help option.
This requires help2man which can be found at: https://www.gnu.org/software/help2man/

This script assumes a build directory named `build` as suggested by example build documentation.
To use it with a different build directory, set `BUILDDIR`.
For example:

```bash
BUILDDIR=$PWD/my-build-dir contrib/devtools/gen-manpages.py
```

headerssync-params.py
=====================

A script to generate optimal parameters for the headerssync module (src/headerssync.cpp). It takes no command-line
options, as all its configuration is set at the top of the file. It runs many times faster inside PyPy. Invocation:

```bash
pypy3 contrib/devtools/headerssync-params.py
```

gen-quicksilver-conf.sh
===================

Generates a quicksilver.conf file in `share/examples/` by parsing the output from `quicksilverd --help`. This script is run during the
release process to include a quicksilver.conf with the release binaries and can also be run by users to generate a file locally.
When generating a file as part of the release process, make sure to commit the changes after running the script.

This script assumes a build directory named `build` as suggested by example build documentation.
To use it with a different build directory, set `BUILDDIR`.
For example:

```bash
BUILDDIR=$PWD/my-build-dir contrib/devtools/gen-quicksilver-conf.sh
```

security-check.py
=================

Perform basic security checks on a series of executables.

symbol-check.py
===============

A script to check that release executables only contain
certain symbols and are only linked against allowed libraries.

For Linux this means checking for allowed gcc, glibc and libstdc++ version symbols.
This makes sure they are still compatible with the minimum supported distribution versions.

For macOS and Windows we check that the executables are only linked against libraries we allow.

Example usage:

    find ../path/to/executables -type f -executable | xargs python3 contrib/devtools/symbol-check.py

If no errors occur the return value will be 0 and the output will be empty.

If there are any errors the return value will be 1 and output like this will be printed:

    .../64/test_quicksilver: symbol memcpy from unsupported version GLIBC_2.14
    .../64/test_quicksilver: symbol __fdelt_chk from unsupported version GLIBC_2.15
    .../64/test_quicksilver: symbol std::out_of_range::~out_of_range() from unsupported version GLIBCXX_3.4.15
    .../64/test_quicksilver: symbol _ZNSt8__detail15_List_nod from unsupported version GLIBCXX_3.4.15

circular-dependencies.py
========================

Run this script from the root of the source tree (`src/`) to find circular dependencies in the source code.
This looks only at which files include other files, treating the `.cpp` and `.h` file as one unit.

Example usage:

    cd .../src
    ../contrib/devtools/circular-dependencies.py {*,*/*,*/*/*}.{h,cpp}
