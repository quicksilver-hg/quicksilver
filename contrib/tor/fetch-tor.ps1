# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Fetch the Tor Project's Expert Bundle for the Windows build.
#
# Quicksilver does not build or vendor Tor: a vendored or self-built Tor makes
# the OpenSSL and Tor CVE stream ours for the life of the project, and buys
# reproducibility that only matters for binary distribution, which 0.1.x does
# not do. On Unix the platform's package manager delivers tor; Windows has no
# package manager to delegate to, so the build fetches it -- pinned.
#
# The SHA-256 below IS the integrity boundary. It is checked BEFORE the archive
# is opened, and a mismatch deletes the download. Do not "temporarily" skip it.
#
# To move to a newer bundle, change $Version and $Sha256 together, taking the
# digest from that release's own sha256sums-unsigned-build.txt:
#   https://dist.torproject.org/torbrowser/<version>/sha256sums-unsigned-build.txt
#
# Usage:  powershell -NoProfile -File contrib\tor\fetch-tor.ps1 [-OutDir <path>]
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

$Version = '15.0.22'
$Archive = "tor-expert-bundle-windows-x86_64-$Version.tar.gz"
$Url     = "https://dist.torproject.org/torbrowser/$Version/$Archive"
# From that release's sha256sums-unsigned-build.txt, and independently recomputed
# over the downloaded archive on 2026-09-09.
$Sha256  = '231dad6b9cb401a54c260db7046965ef04e4f72ff071b140d423fb5da281ab1e'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Download = Join-Path $OutDir $Archive

if (-not (Test-Path $Download)) {
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Download -UseBasicParsing
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

# This bundle's tor.exe is self-contained: the archive ships no DLLs anywhere in
# it (verified against 15.0.22 -- 19 archive entries, 4 directories and 15 files,
# zero *.dll; of those, 7 entries live under tor\). The pluggable transports in
# tor\pluggable_transports and the geoip files in data\ are NOT used by
# Quicksilver -- no bridges, no country selection -- so tor.exe alone is enough.
Write-Host ''
Write-Host "tor.exe is at: $TorExe"
Write-Host 'Either copy it beside quicksilver-qt.exe, or run the desktop with'
Write-Host "  -bundledtorpath=$TorExe"
exit 0
