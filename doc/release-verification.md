# Verifying a Quicksilver release

Binary releases are published as GitHub Releases on the public repository,
[quicksilver-hg/quicksilver](https://github.com/quicksilver-hg/quicksilver/releases).
There is no separate download service.

One tag, `vX.Y.Z`, covers Ubuntu 22.04, Ubuntu 24.04, and Windows x64. The
Release carries:

- `INDEX`, `INDEX.asc`, `SHA256SUMS`, and `SHA256SUMS.asc`
- `ubuntu-22.04.tar`, `ubuntu-24.04.tar`, and `windows-x64.tar`

Each archive is one platform directory. The Ubuntu directories contain that
release's `.deb` packages, the debug `.ddeb`, and the `.buildinfo` and
`.changes` files. The Windows directory contains the installer. `SHA256SUMS`
at the top level names `INDEX` and each platform's own `SHA256SUMS`. It does
not name the archives. Unpack first, then check.

```bash
mkdir vX.Y.Z
tar -C vX.Y.Z -xf ubuntu-22.04.tar
tar -C vX.Y.Z -xf ubuntu-24.04.tar
tar -C vX.Y.Z -xf windows-x64.tar
cp INDEX INDEX.asc SHA256SUMS SHA256SUMS.asc vX.Y.Z/
```

Install the `.deb` packages from the Ubuntu directory for the system you
run. On Windows, run the installer in `windows-x64`.

## What a passing check means

There is no reproducible build. A checksum does not prove that a binary came
from a given tree on someone else's machine. It names the file this project
published, and the release index names the machine and the toolchain that
produced it. Trust sits with the publisher.

The project signs `INDEX` and every `SHA256SUMS` with a GPG key. `INDEX`
records the public tag, the public commit, the reviewed development commit,
and each file's name, size, and SHA-256.

## The signing key

The key fingerprint is published with the first signed release, in that
release's notes and in this document. Compare the fingerprint `gpg` reports
with that published value. A signature that checks under some other key is
not a Quicksilver release signature. This document does not carry a
fingerprint until that first signed release exists.

## Linux

From a source checkout, after the tree above has been rebuilt:

```bash
contrib/release/verify-release.sh vX.Y.Z FINGERPRINT
```

`FINGERPRINT` is the published fingerprint, the full 40 hex digits. The
script refuses a short key id. It also rejects a missing file, an extra
file, a file whose bytes do not match, and a signature whose key is not the
published one.

The same checks by hand, from inside `vX.Y.Z`:

```bash
gpg --verify INDEX.asc INDEX
gpg --verify SHA256SUMS.asc SHA256SUMS
gpg --verify ubuntu-22.04/SHA256SUMS.asc ubuntu-22.04/SHA256SUMS
gpg --verify ubuntu-24.04/SHA256SUMS.asc ubuntu-24.04/SHA256SUMS
gpg --verify windows-x64/SHA256SUMS.asc windows-x64/SHA256SUMS
sha256sum -c SHA256SUMS
(cd ubuntu-22.04 && sha256sum -c SHA256SUMS)
(cd ubuntu-24.04 && sha256sum -c SHA256SUMS)
(cd windows-x64 && sha256sum -c SHA256SUMS)
```

`gpg --verify` prints `Good signature` for any key in your keyring. Read the
fingerprint it prints. It must be the published fingerprint.

## Windows

`gpg --verify SHA256SUMS.asc SHA256SUMS` is the same check when GnuPG is
installed. Read the fingerprint it prints.

PowerShell does not print checksums in the `sha256sum` layout. After
unpacking `windows-x64.tar`, hash each file the manifest names and compare
`Hash` with the hex in `SHA256SUMS`, ignoring case:

```powershell
Get-FileHash -Algorithm SHA256 -Path .\the-file
```

## Windows SmartScreen

The Windows installer is not Authenticode-signed. No code-signing
certificate is held, so Windows has no Authenticode publisher for that file
and SmartScreen may warn. The check that the file is the one this project
published is the GPG signature on its `SHA256SUMS` and the checksum of the
installer.
