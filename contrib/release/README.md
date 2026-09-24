# Release checksums

## Manual pages before packaging

Manual pages are part of the tagged source. Prepare and prove them in this
order; do not package before the final verification succeeds:

1. Bump the version in `CMakeLists.txt`, set `CLIENT_VERSION_IS_RELEASE` to
   `true`, and commit that change.
2. From the clean commit, configure and build every program in
   `contrib/devtools/gen-manpages.py`'s `BINARIES` list.
3. Run `contrib/devtools/gen-manpages.py --release-version vX.Y.Z`. The value
   must match `CMakeLists.txt`, and the binaries must identify the current
   untagged `HEAD` exactly.
4. Run `contrib/devtools/gen-manpages.py --check`.
5. Review and commit the regenerated `doc/man/*.1` files.
6. Place the signed `vX.Y.Z` tag on that commit.
7. In a clean checkout of that exact tag, configure and rebuild every program,
   confirm each `--version` reports exactly `vX.Y.Z`, then run
   `contrib/devtools/gen-manpages.py --verify`. This regenerates into a
   disposable directory and diffs every page against the committed copy.
8. Only after verification passes, build the release packages.

Both generation paths pin help2man's date by setting `SOURCE_DATE_EPOCH` to
the timestamp of the last commit that changed `CMakeLists.txt`. The version
bump therefore establishes one date input that remains stable through the page
commit and tag.

If post-tag verification fails, never move a published tag: fix the problem
and cut the next version.

`gen-sha256sums.sh` writes a `SHA256SUMS` file for one directory of release
artifacts, in the form `sha256sum -c` reads. It does not sign anything.

There is no reproducible build. `contrib/guix` was removed on purpose and
stays removed. A checksum does not prove that a binary came from a given
tree on someone else's machine. It names the file this project published,
and the record below names the machine and the toolchain that produced it.
Trust sits with the publisher.

## Ubuntu 22.04 package builder

The Ubuntu 22.04 packages are built in the pinned Jammy container described by
`jammy.Dockerfile`. From a clean checkout, run:

```
contrib/release/build-jammy-debs.sh /path/outside/the/checkout/for/artifacts
```

The output directory must be empty. The script clones the checked-out commit,
builds both Debian packages at `-j6`, checks the generated Git commit stamp,
and copies the `.deb`, debug packages, `.buildinfo` and `.changes` files into
that directory. Its Dockerfile pins the Ubuntu image digest. Updating that pin
requires a new package build, dependency inspection, and a fresh Jammy install
and version check. The image itself is a build host, not a release artifact.

The Ubuntu 24.04 packages declare `t64` library names and
`libstdc++6 (>= 13.1)`, which Ubuntu 22.04 cannot satisfy. Building the
22.04 packages in its own userland yields installable dependencies for that
distribution. A container shares the host kernel, so the check covers the
Jammy userland and package dependencies, not separate 22.04 hardware.

### Jammy validation build, 2026-09-23

The pinned base image was
`ubuntu@sha256:b8b6ee6aa931ecd9d0d952abc34dc0e5f7c6a30c6bb71b079fe399fde0329c02`.
It carried CMake 3.22.1, GCC 11.4.0, glibc 2.35, Qt 5.15.3 and debhelper
13.6ubuntu1. A clean clone of source commit
`49744b1c7920aba02e3a37e2c950ecf733da6d45` produced these validation
artifacts under `f345-jammy-recipe/validation-49744b1c/`:

```
756ade445f0e63885e0cb75a6777c6875ba4bd0efe630ecd34a040d08d93a319  quicksilver-devtools-dbgsym_0.1.1-1_amd64.ddeb
8d67bec72c338034c775baf56ccdf4802e2796aef05a20d226bd76d8cca48a51  quicksilver-devtools_0.1.1-1_amd64.deb
22f12949788fad1cc0fb7873497a374b06f8af8cf5ea932f0f0fdc78de0e9be3  quicksilver-qt-dbgsym_0.1.1-1_amd64.ddeb
38af6695e73b6eb56a98dcbe37ea278e6b41bafbcd6f6813c51cbcc7020253b0  quicksilver-qt_0.1.1-1_amd64.deb
636aea2f09b75ce278ae67d9d3359d68d426680d6e55f10ccc222cb25a3dfca4  quicksilver_0.1.1-1_amd64.buildinfo
f56172a069652f4a539809b5b086135875a3c48e9992c8a688574fee50611a63  quicksilver_0.1.1-1_amd64.changes
```

Both packages installed in a fresh Jammy container. The installed desktop
printed `Quicksilver version v0.1.1-49744b1c7920`. Both package dependency
lists require `libstdc++6 (>= 12)` and contain no `t64` library. The driver
returned an error after exporting these files because it could not remove
root-owned temporary build files; that cleanup was repaired in the subsequent
commit. These are validation artifacts, not published packages.

## Run

From the repository root, on a directory that contains the artifacts and
does not already contain `SHA256SUMS`:

```
contrib/release/gen-sha256sums.sh /path/to/artifacts
( cd /path/to/artifacts && sha256sum -c SHA256SUMS )
```

The script refuses an empty directory and refuses to overwrite an existing
`SHA256SUMS`. It hashes regular files in that directory only, not
subdirectories.

## How these artifacts were produced

Built on `precision`, Linux Mint 22.3 (Ubuntu 24.04 userland), Linux
6.17.0-42-generic x86_64, glibc 2.39 (`ldd (Ubuntu GLIBC 2.39-0ubuntu8.9)`).
Compiler `g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`. CMake 3.28.3.
debhelper 13.14.1ubuntu5. The source commit is `0a1a39be7eeaae3e878ffc46fbf6f452641d89d7`
on `build/g7-linux-release-artifacts`, which is `085d5090` plus the packaging
commits on this branch. `git diff-index --quiet HEAD` was 0 when the package
build started. The binaries identify themselves as `v0.1.1-0a1a39be7eea`.

The two `.deb` files were built with:

```
dpkg-buildpackage -us -uc -b -j10
```

`debian/rules` passes `DEB_BUILD_MAINT_OPTIONS=hardening=+all optimize=-lto`
and configures CMake as debhelper does (`-DCMAKE_BUILD_TYPE=None`, prefix
`/usr`) plus:

```
-DBUILD_GUI=ON -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DBUILD_AGENT=ON
-DBUILD_TX=ON -DBUILD_UTIL=ON -DBUILD_VAULT_TOOL=ON
-DBUILD_TESTS=OFF -DQS_DEVELOPER_TOOLS=ON
```

`optimize=-lto` is required on this toolchain. Ubuntu's default
`-flto=auto` made `quicksilver-qt` define `QGuiApplication::staticMetaObject`
itself, and the xcb plugin then crashed in `QGuiApplication::screenAdded`
before `-version` could run. The same objects linked with `-fno-lto` leave
that symbol to `libQt5Gui` and `-version` prints.

No AppImage was produced. The former AppImage builder configured
and built (CMake `Release`, `-DBUILD_GUI=ON -DBUILD_DAEMON=OFF
-DBUILD_CLI=OFF -DBUILD_TESTS=OFF -DQS_DEVELOPER_TOOLS=OFF`, `-j10`) and
installed `quicksilver-qt` and `quicksilver-agent` into its AppDir. The
pinned `linuxdeployqt` then stopped:

```
ERROR: The host system is too new.
Please run on a system with a glibc version no newer than what comes with
the oldest currently supported mainstream distribution (Ubuntu Jammy
Jellyfish), which is glibc 2.35.
```

This machine's glibc is 2.39. The helper's own SHA-256 check had already
passed (`974a87457ed26241b793bed7841978fcdf84158d13220e53833a06515f173b0b`
for linuxdeployqt, `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`
for appimagetool 1.9.1). Nothing was signed.

The AppImage was subsequently dropped from the release matrix. Its reach is
limited by the build host's glibc version, so bundling on this host would not
produce an artifact compatible with older Linux distributions.
