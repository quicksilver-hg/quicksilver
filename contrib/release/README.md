# Release checksums

`gen-sha256sums.sh` writes a `SHA256SUMS` file for one directory of release
artifacts, in the form `sha256sum -c` reads. It does not sign anything.

There is no reproducible build. `contrib/guix` was removed on purpose and
stays removed. A checksum does not prove that a binary came from a given
tree on someone else's machine. It names the file this project published,
and the record below names the machine and the toolchain that produced it.
Trust sits with the publisher.

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
