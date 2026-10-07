# Release procedure

## Manual pages before packaging

Manual pages are part of the tagged source. Prepare and prove them in this
order; do not package before the final verification succeeds:

1. Bump the version in `CMakeLists.txt`, set `CLIENT_VERSION_IS_RELEASE` to
   `true`, and commit that change.
2. From the clean commit, configure and build every program in
   `contrib/devtools/gen-manpages.py`'s `BINARIES` list.
3. Run `contrib/devtools/gen-manpages.py --release-version vX.Y.Z`. Release
   candidates use `vX.Y.ZrcN`. The value
   must match `CMakeLists.txt`, and the binaries must identify the current
   untagged `HEAD` exactly.
4. Run `contrib/devtools/gen-manpages.py --check`.
5. Review and commit the regenerated `doc/man/*.1` files.
6. Publish that commit from the development checkout with
   `contrib/devtools/publish-source.sh` into the publication clone. Record the
   development SHA and the public SHA. The trees match. The commits do not:
   the development history is never pushed, and the public repository has no
   shared ancestry with it. See `doc/source-publication.md`.
7. Place the signed `vX.Y.Z` tag on the public commit, in the publication
   clone. Do not tag the development checkout.
8. In a clean clone of that public tag, configure and rebuild every program,
   confirm each `--version` reports exactly the tag, then run
   `contrib/devtools/gen-manpages.py --verify`. This regenerates into a
   disposable directory and diffs every page against the committed copy.
9. Only after verification passes, build the release packages. Every builder
   starts from a clean clone of the public tag, not from the development
   checkout.

Both generation paths pin help2man's date by setting `SOURCE_DATE_EPOCH` to
the timestamp of the last commit that changed `CMakeLists.txt`. The version
bump therefore establishes one date input that remains stable through the page
commit and tag.

If post-tag verification fails, never move a published tag: fix the problem
and cut the next version.

## Mainnet chain transaction data before tagging

Sync progress is `ChainstateManager::GuessVerificationProgress`. It estimates
how many transactions the chain holds from mainnet `chainTxData` in
`src/kernel/chainparams.cpp`. With `nTime`, `tx_count` and `dTxRate` all zero,
every tip, including genesis, is reported as 100%. Refresh those three fields
from the chain before every release. Do it before the version bump above, and
commit it before the source is published.

1. Use a synced mainnet node. Run `quicksilver-cli getchaintxstats 4096`.
   The window of 4096 blocks is the one upstream's release process uses.
2. A node with an `rpcwhitelist` that does not name `getchaintxstats` refuses
   the call. The desktop node's whitelist refuses it. Take the reading on a
   node that allows the RPC. Do not take it from the desktop.
3. Copy `time` to `nTime`, `txcount` to `tx_count`, and `txrate` to `dTxRate`
   for mainnet only.
4. Replace the comment above mainnet `chainTxData` with the RPC, the window,
   the full `window_final_block_hash`, and the height:
   `// Data from RPC: getchaintxstats 4096 <64 hex>` and `// height <n>`.
5. Commit that change. The hash and the height belong in the comment, not
   only in the commit message.
6. A reviewer repeats `quicksilver-cli getchaintxstats 4096 <hash>` on a
   synced node that accepts the call. `time`, `txcount` and `txrate` must
   match the three committed fields, and the height must match the comment.

Public test and sandbox stay all zeros. There is no public-test reading to
commit. Sandbox is local and mockable, as upstream regtest is, and zeros are
the right data there. Both still report verification progress of 1.0. Do not
invent a reading for either, and do not add a height fallback in
`GuessVerificationProgress` to hide the zero data.

## Build each leg from the public tag

One public tag covers Ubuntu 22.04, Ubuntu 24.04, and Windows x64. The binary
release waits until all three have passed. Each builder is a clean clone of
that tag.

- Ubuntu 22.04: `contrib/release/build-jammy-debs.sh`, below.
- Ubuntu 24.04: `contrib/release/build-noble-debs.sh`, below.
- Windows x64: the MSVC installer build. Copy the installer off the build
  host and hash it before and after the copy. Keep the two hashes. Staging
  checks the same thing again.

A Windows installer is not opened during staging. Its provenance record and
the transfer hash are the checks for that file. A `.deb` is extracted,
because the packaged programs can be executed on Linux. Each must identify
as exactly the tag. The upstream part of the Debian version is taken from
that tag: `X.Y.Z`, or `X.Y.Z~rcN` for an RC. The revision may be any `-N`,
`N` a positive integer with no leading zero, because a packaging-only
rebuild of the same tag is `-2` and should still stage. Staging runs from
builder output and has no source tree, so it does not consult
`debian/changelog`. Every `.deb` in the run must use the same revision; a
mix such as `-1` and `-2` is refused and the message names both. A suffix
such as `vX.Y.Z-<12 hex>` is an untagged build and is refused.

## Stage

`contrib/release/stage-release.py` copies an explicit file list into a new
tree `<output>/<tag>/<platform>/`. It refuses an existing `<output>/<tag>`.
It does not move or edit the builder's outputs. It hashes every file before
and after the copy and stops if they differ.

Each input directory carries a `PROVENANCE` file. Blank lines and `#`
comments are ignored. Other lines are `key=value`. Required once:

```
platform=ubuntu-22.04
public_tag=vX.Y.Z
public_sha=<40 hex>
development_sha=<40 hex>
builder_host=<hostname>
os_userland=<distribution and version>
compiler=<compiler and version>
image_digest=<sha256:...> or none
build_command=<command that produced the files>
version_line=<the -version line a built binary printed>
file=<basename>
```

`file` repeats, once per public file. `image_digest` is `none` when that
build had no image. The script copies those basenames and nothing else in
the directory, so a log left beside the artifacts is not signed. The three
platform names above are the 1.0 matrix. The script does not hardcode them:
another platform is another `--platform` argument and another input
directory.

```
contrib/release/stage-release.py \
  --version vX.Y.Z \
  --public-sha <public SHA> \
  --development-sha <development SHA> \
  --output /path/to/staging \
  --platform ubuntu-22.04=/path/to/jammy-output \
  --platform ubuntu-24.04=/path/to/noble-output \
  --platform windows-x64=/path/to/windows-output
```

Every platform's tag and both SHAs must match the arguments. One tag for
the whole release is enforced here.

The pre-hash signing slot is `--sign-hook PLATFORM=EXECUTABLE`. After the
copy and the transfer-hash check, and before `gen-sha256sums.sh`, the
executable is run with the staged platform directory as its only argument.
It may rewrite bytes in place. It may not add, remove, or rename a file; a
change of the file set is refused and the name is reported. Omit the option
and that platform is hashed as copied. Any platform may set a hook. The
flow does not branch on the platform name.

`gen-sha256sums.sh` then writes `SHA256SUMS` in each platform directory.
The script also writes `INDEX` at the top of `vX.Y.Z`, one record per
platform: directory, provenance fields, and each file's name, size, and
SHA-256. It writes a top-level `SHA256SUMS` over each platform's
`SHA256SUMS` and over `INDEX`.

## Sign

`contrib/release/sign-release.sh <staged vX.Y.Z> FINGERPRINT` writes a
detached armored signature for `INDEX` and for every `SHA256SUMS`. The
fingerprint is the full 40 hex digits. A short key id is refused, as is a
fingerprint that is not exactly the secret key's own, as is a signature
file that already exists. The script verifies each signature it wrote and
requires the `VALIDSIG` primary fingerprint to match. It does not upload.

Leave `QS_GPG_PASSPHRASE` unset for the project key and let `gpg-agent`
handle the passphrase. That variable exists so a test can pass an empty
passphrase to a throwaway key in a temporary `GNUPGHOME`. Do not set it for
the project key.

## Verify

`contrib/release/verify-release.sh <directory> FINGERPRINT` is the check a
stranger runs, and the check to run again after a Release is downloaded.
It verifies every signature by parsing `VALIDSIG`, and it fails when the
primary fingerprint is not the one on the command line. `gpg --verify`
alone is not the check: it accepts any key in the keyring. The script then
runs `sha256sum -c` in the top directory and in each platform directory,
rejects an extra or missing file, and checks `INDEX` against the files.

## GitHub Release

GitHub Release assets are a flat list of file names. Each platform
directory contains `SHA256SUMS` and `SHA256SUMS.asc`, so uploading those
files directly would collide. Ship one tar per platform directory, which
keeps the signed directory intact and keeps the platforms apart, and ship
the four top-level files beside the archives. Their names are unique. The
signed hashes are of the unpacked tree, not of the tar bytes.

Run this from the publication clone, after staging, signing, and
`verify-release.sh` have succeeded. Do not run it from the development
checkout, and do not run it as part of preparing the tooling.

```bash
VERSION=vX.Y.Z
STAGE=/path/to/staging/$VERSION
DIST=$(mktemp -d)
tar -C "$STAGE" -cf "$DIST/ubuntu-22.04.tar" ubuntu-22.04
tar -C "$STAGE" -cf "$DIST/ubuntu-24.04.tar" ubuntu-24.04
tar -C "$STAGE" -cf "$DIST/windows-x64.tar" windows-x64
cp "$STAGE/INDEX" "$STAGE/INDEX.asc" "$STAGE/SHA256SUMS" "$STAGE/SHA256SUMS.asc" "$DIST/"
gh release create "$VERSION" --repo quicksilver-hg/quicksilver \
  --title "$VERSION" \
  --notes-file /path/to/release-notes \
  "$DIST/INDEX" "$DIST/INDEX.asc" "$DIST/SHA256SUMS" "$DIST/SHA256SUMS.asc" \
  "$DIST/ubuntu-22.04.tar" "$DIST/ubuntu-24.04.tar" "$DIST/windows-x64.tar"
```

The notes file carries the key fingerprint and a pointer to
`doc/release-verification.md`. The fingerprint is published there and in
that document, not only as a file beside the binaries.

## Verify the downloaded assets

Download the assets, rebuild the tree, and run the verifier again. This is
the readback. The fingerprint is the published one.

```bash
mkdir "$VERSION"
tar -C "$VERSION" -xf ubuntu-22.04.tar
tar -C "$VERSION" -xf ubuntu-24.04.tar
tar -C "$VERSION" -xf windows-x64.tar
cp INDEX INDEX.asc SHA256SUMS SHA256SUMS.asc "$VERSION/"
contrib/release/verify-release.sh "$VERSION" FINGERPRINT
```

## Key custody

Create the release key offline, on a machine that is not the one that will
build or upload the artifacts:

```bash
gpg --quick-gen-key "Quicksilver Release <address the owner chooses>" ed25519 cert never
```

GnuPG writes a revocation certificate under `openpgp-revocs.d` in that
`GNUPGHOME` when the key is created. Store that certificate, and an
encrypted backup of the secret key (`gpg --export-secret-keys --armor
FINGERPRINT`), offline. Do not put either in the repository or in a
Release.

Publish the public key (`gpg --export --armor FINGERPRINT`) and its
fingerprint in the release notes and in `doc/release-verification.md`. A
copy of the public key next to the binaries is not the publication
channel: anyone who can replace the binaries can replace that copy.

To rotate the key, generate the new one the same way, with its own
revocation certificate. Sign the new fingerprint with the old key and
publish that signature in the release notes and in
`doc/release-verification.md` before any release is signed by the new key.
The last release signed by the old key can carry the announcement. Later
releases are signed by the new key. If the old key is compromised, publish
its revocation certificate through that same channel and stop trusting
signatures from it. Do not replace the artifacts of a tag that was already
published. A later release gets a new tag.

## Authenticode later

No code-signing certificate is held, so the Windows installer is not
Authenticode-signed and SmartScreen may warn. The public integrity check
is the GPG signature on `SHA256SUMS` and the checksum of the installer.
Authenticode is not part of this release.

Authenticode changes the installer bytes, so it has to run before those
bytes are hashed. That is the pre-hash slot:

```bash
--sign-hook windows-x64=/path/to/authenticode-sign
```

The hook receives the staged `windows-x64` directory. A later certificate
plugs in there and rewrites the installer in place. The hash and the
signature are taken after it returns. Nothing else in the flow changes,
and a platform with no hook is unchanged.

`gen-sha256sums.sh` writes a `SHA256SUMS` file for one directory of release
artifacts, in the form `sha256sum -c` reads. It does not sign anything.

There is no reproducible build. `contrib/guix` was removed on purpose and
stays removed. A checksum does not prove that a binary came from a given
tree on someone else's machine. It names the file this project published,
and the record below names the machine and the toolchain that produced it.
Trust sits with the publisher.

## Ubuntu package builders

The Ubuntu packages are built in pinned suite containers described by
`jammy.Dockerfile` and `noble.Dockerfile`. From separate clean checkouts of
the public tag, run:

```
contrib/release/build-jammy-debs.sh /path/outside/the/checkout/for/jammy-artifacts
contrib/release/build-noble-debs.sh /path/outside/the/checkout/for/noble-artifacts
```

The wrappers select the suite in the shared `build-debs.sh` driver. Each output
directory must be empty. The driver clones the checked-out commit, builds both
Debian packages at `-j6`, checks the generated exact-tag or commit stamp, and
copies the `.deb`, debug packages, `.buildinfo` and `.changes` files into that
directory. It then runs `check-dbgsym.sh` on that directory: a fresh container
of the same suite installs the packages with their `-dbgsym` debug packages,
and the build fails if gdb cannot symbolise `main` with a file and a line.
Each Dockerfile pins its Ubuntu image digest. Updating a pin requires a new
package build, dependency inspection, and a fresh install and version check in
that suite. The image itself is a build host, not a release artifact.

When this checkout is the signed public tag, the script can also write a
`PROVENANCE` record into that output directory. Export `QS_DEVELOPMENT_SHA`
(the reviewed development commit, 40 hex digits) and `QS_VERSION_LINE` (a
`-version` line captured from an installed binary) before the run. The
public tree does not contain the development SHA, and the script does not
execute the packaged binaries, so it cannot invent those two values. If
either is unset, or `HEAD` is not exactly `vX.Y.Z` or `vX.Y.ZrcN`, the
script writes no record and its result is unchanged.

The Ubuntu 24.04 packages declare `t64` library names and
`libstdc++6 (>= 13.1)`, which Ubuntu 22.04 cannot satisfy. Building the
packages in their own userlands yields installable dependencies for each
distribution. A container shares the host kernel, so these checks cover the
Jammy and Noble userlands and package dependencies, not separate hardware.

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

`debian/rules` passed `DEB_BUILD_MAINT_OPTIONS=hardening=+all optimize=-lto`
and configures CMake as debhelper does (`-DCMAKE_BUILD_TYPE=None`, prefix
`/usr`) plus:

```
-DBUILD_GUI=ON -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DBUILD_AGENT=ON
-DBUILD_TX=ON -DBUILD_UTIL=ON -DBUILD_VAULT_TOOL=ON
-DBUILD_TESTS=OFF -DQS_DEVELOPER_TOOLS=ON
```

That build passed `optimize=-lto` because Ubuntu's default `-flto=auto`
made `quicksilver-qt` define `QGuiApplication::staticMetaObject` itself, and
the xcb plugin then crashed in `QGuiApplication::screenAdded` before
`-version` could run. F-334 links the Qt executables `-fPIC` after `-fPIE`,
so that copy relocation is gone, and later packages do not pass
`optimize=-lto`.

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
