# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Fetch the Tor Project's Expert Bundle for the Windows build.
#
# Quicksilver does not build Tor. On Unix the platform's package manager
# delivers tor; Windows has no package manager to delegate to, so the build
# fetches the Tor Project's own build -- pinned -- and the Windows installer
# ships its tor.exe unmodified beside quicksilver.exe (F-441: the desktop starts
# its own Tor, and someone who downloaded an installer cannot be assumed to
# have installed one).
#
# Shipping it has a cost that building from source alone did not: Tor's and
# OpenSSL's security releases are now this project's to ship. Before every
# release, check https://dist.torproject.org/torbrowser/ for a newer Expert
# Bundle and move the pins (both here and QUICKSILVER_BUNDLED_TOR_SHA256 in
# cmake/module/Maintenance.cmake).
#
# The SHA-256 below IS the integrity boundary. It is checked BEFORE the archive
# is opened, and a mismatch deletes the download. Do not "temporarily" skip it.
#
# To move to a newer bundle, change $Version, $Sha256 and $TorExeSha256
# together. Take the archive digest from that release's own
# sha256sums-unsigned-build.txt, check the archive's .asc against the Tor
# Browser Developers signing key (EF6E 286D DA85 EA2A 4BA7  DE68 4E2C 6E87 9329 8290),
# then hash tor\tor.exe inside it:
#   https://dist.torproject.org/torbrowser/<version>/sha256sums-unsigned-build.txt
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File contrib\tor\fetch-tor.ps1 [-OutDir <path>]
#
# Windows PowerShell 5.1 is enough -- pwsh is NOT required. But it must be run
# as a FILE: piped in, or passed with -Command, $PSScriptRoot is empty and the
# -OutDir default cannot resolve. That failure names Join-Path, not the
# transport, which is how it gets misread as a version problem.
#
# $PSScriptRoot is ALSO empty inside a param() default -- even with -File, and
# even on 5.1 where the body sees it fine. So the default is resolved in the
# body below, not in the param block. Measured on 5.1.19041.6456: a param
# default reads '' while the body reads the script's directory.

[CmdletBinding()]
param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Resolved here, not in param(): see the note above.
# GetFullPath collapses the '..' so the path this script PRINTS is one a user
# can paste into -bundledtorpath without it reading like a mistake.
if (-not $OutDir) { $OutDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\build\tor')) }

$Version = '15.0.24'
$Archive = "tor-expert-bundle-windows-x86_64-$Version.tar.gz"
$DistUrl = "https://dist.torproject.org/torbrowser/$Version/$Archive"
$ArchiveUrl = "https://archive.torproject.org/tor-package-archive/torbrowser/$Version/$Archive"
# From that release's sha256sums-unsigned-build.txt; the archive's signature was
# checked against the Tor Browser Developers key on 2026-10-08.
$Sha256  = 'e9dc6ccc93cd6afa507193f4de284d6424233ff5102155cd2c94b259e8a22b65'
# tor\tor.exe inside that archive (tor 0.4.9.13). The installer build checks the
# same digest, as QUICKSILVER_BUNDLED_TOR_SHA256.
$TorExeSha256 = '90bbdcafd586feea608a5e9b7d3959f4ee194f7770755cfde8fab240e9773ad1'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Download = Join-Path $OutDir $Archive

if (-not (Test-Path $Download)) {
    try {
        Write-Host "Downloading $DistUrl"
        Invoke-WebRequest -Uri $DistUrl -OutFile $Download -UseBasicParsing
    } catch {
        # Tor rotates old releases from dist to archive. The pinned digest below
        # is the integrity boundary, so using the official archive as a mirror
        # does not weaken verification.
        Write-Warning "The release is no longer available from dist; trying $ArchiveUrl"
        Remove-Item -Force $Download -ErrorAction SilentlyContinue
        Invoke-WebRequest -Uri $ArchiveUrl -OutFile $Download -UseBasicParsing
    }
}

$Actual = (Get-FileHash -Path $Download -Algorithm SHA256).Hash.ToLowerInvariant()
if ($Actual -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $Download
    throw "SHA-256 mismatch for $Archive`n  expected $Sha256`n  actual   $Actual`nThe download was deleted."
}
Write-Host "SHA-256 verified: $Actual"

# tar is present on every supported Windows build (bsdtar since 1803).
tar -xzf $Download -C $OutDir
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

$TorExe = Join-Path $OutDir 'tor\tor.exe'
if (-not (Test-Path $TorExe)) { throw "tor.exe not found at $TorExe after extraction" }
$TorExeActual = (Get-FileHash -Path $TorExe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($TorExeActual -ne $TorExeSha256.ToLowerInvariant()) {
    throw "SHA-256 mismatch for $TorExe`n  expected $TorExeSha256`n  actual   $TorExeActual"
}
Write-Host "tor.exe SHA-256 verified: $TorExeActual"

# This bundle's tor.exe is self-contained: the archive ships no DLLs anywhere in
# it (verified against 15.0.24 -- 19 archive entries, 4 directories and 15 files,
# zero *.dll; of those, 7 entries live under tor\). The pluggable transports in
# tor\pluggable_transports and the geoip files in data\ are NOT used by
# Quicksilver -- no bridges, no country selection -- so tor.exe alone is enough.
Write-Host ''
Write-Host "tor.exe is at: $TorExe"
Write-Host 'Either copy it beside quicksilver.exe, or run the desktop with'
Write-Host "  -bundledtorpath=$TorExe"
exit 0
