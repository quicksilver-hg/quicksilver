# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
param(
    [string]$VsInstallPath = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [string]$VcVarsVersion = '14.44',
    [string]$CudaPath = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9',
    [string]$GpuArch = 'sm_61',
    # Must match the shipped consensus graph size (chainparams nEdgeBits/nTxEdgeBits
    # on main and publictest). A solver built at any other size is rejected at
    # runtime by qsgpusolve's own compiled-size guard.
    [int]$EdgeBits = 28,
    # The gpu/ Makefile builds two binaries from the same vendored solver: the
    # miner and its calibration twin. Only the miner was reachable from Windows,
    # so a Windows card could not be measured the way the Linux ones were.
    [ValidateSet('qsgpusolve', 'qsgpucalibrate')]
    [string]$Target = 'qsgpusolve',
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

# This helper lives in contrib/windows/; the solver source is under
# src/crypto/cuckatoo/gpu/. Locate the repo root two levels up so the script
# works regardless of the current working directory.
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$gpuDir = Join-Path $repoRoot 'src\crypto\cuckatoo\gpu'
$sourceRoot = Join-Path $repoRoot 'src'
$blakeSource = Join-Path $repoRoot 'src\crypto\cuckatoo\vendor\blake2b-ref.c'
$sourceFile = "$Target.cu"
if (!$OutputPath) {
    # Default output next to the Makefile so gpu/.gitignore covers the product.
    $OutputPath = Join-Path $gpuDir "$Target.exe"
}

$devShellModule = Join-Path $VsInstallPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (!(Test-Path $devShellModule)) {
    throw "Visual Studio dev-shell module not found: $devShellModule"
}

$toolsetsPath = Join-Path $VsInstallPath 'VC\Tools\MSVC'
$toolset = Get-ChildItem -Path $toolsetsPath -Directory -ErrorAction Stop |
    Where-Object { $_.Name.StartsWith($VcVarsVersion) } |
    Select-Object -First 1
if ($null -eq $toolset) {
    throw "MSVC toolset $VcVarsVersion not found under $toolsetsPath"
}

$nvcc = Join-Path $CudaPath 'bin\nvcc.exe'
if (!(Test-Path $nvcc)) {
    throw "nvcc not found: $nvcc"
}

Import-Module $devShellModule
Enter-VsDevShell -VsInstallPath $VsInstallPath `
    -SkipAutomaticLocation `
    -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$VcVarsVersion"

Push-Location $gpuDir
try {
    $args = @(
        '-std=c++14',
        "-I$sourceRoot",
        '-o', $OutputPath,
        "-DEDGEBITS=$EdgeBits",
        '-arch', $GpuArch,
        $sourceFile,
        $blakeSource
    )
    & $nvcc @args
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "built $OutputPath"
