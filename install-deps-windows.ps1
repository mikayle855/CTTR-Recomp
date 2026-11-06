#!/usr/bin/env pwsh
# Install the toolchain needed to build CTTR-Recomp natively on Windows
# (no Docker/WSL). Safe to re-run: skips anything already installed.
#
#   .\install-deps-windows.ps1             # check only
#   .\install-deps-windows.ps1 -Install   # install what's missing (winget/pip)
#
# Installs:
#   - VS Build Tools 2022 (ClangCL + Win11 SDK)          [winget]
#   - CMake >= 3.21                                       [winget]
#   - Ninja                                               [winget]
#   - Python 3                                            [winget]
#   - aqtinstall + pefile (pip)  + Qt 6.5.3 via aqtinstall
param(
    [switch]$Install,
    [switch]$RunnerOnly   # only what the game runner needs (install-bt4.ps1): no Qt (the BT3 Qt launcher), no lavapipe
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$BUILD = Join-Path $PSScriptRoot "build"
$QT_VERSION = "6.5.3"
$QT_HOST = "windows"
# Qt 6.5.x ships only the msvc2019_64 kit; it is ABI-compatible with MSVC 2022.
$QT_TARGET = "win64_msvc2019_64"   # aqtinstall arch name for the download
$QT_INSTALL_DIR = "msvc2019_64"    # directory aqt creates under <outputdir>/<version>
$QT_BASE = Join-Path $BUILD "qt"
$QT_ROOT = Join-Path $QT_BASE "$QT_VERSION\$QT_INSTALL_DIR"

function Log($msg) { Write-Host "  $msg" }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Error $msg; exit 1 }

function Test-FileCmd($name) {
    if ($PSVersionTable.PSEdition -eq "Core") { $exe = $name + (if ($IsWindows) { ".exe" } else { "" }) }
    else { $exe = $name + ".exe" }
    return [bool](Get-Command $exe -ErrorAction SilentlyContinue)
}

# Run a native command line through cmd.exe so pip/aqtinstall progress and
# warnings written to stderr do not become terminating NativeCommandError
# records under $ErrorActionPreference = "Stop".
function Invoke-CmdLine([string]$Line) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { cmd.exe /d /s /c $Line } finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

Step "Checking prerequisites"
$missing = @()

# VS Build Tools (with ClangCL)
$hasVS = $false
if (Test-Path $vswhere) {
    $foundPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $hasVS = [bool]$foundPath
}
if (-not $hasVS) { $missing += "VS Build Tools 2022 (ClangCL + Win11 SDK)" }

# CMake (>= 3.21 for DOWNLOAD_EXTRACT_TIMESTAMP)
$cmakeVersion = ""
if (Test-FileCmd "cmake") {
    $cmakeVersion = (& cmake --version | Select-Object -First 1) -replace '^cmake version (\d+)\.(\d+).*','$1.$2'
    if ([version]"$cmakeVersion" -lt [version]"3.21") { $missing += "CMake >= 3.21 (found $cmakeVersion)" }
} else { $missing += "CMake" }

# Ninja
if (-not (Test-FileCmd "ninja")) { $missing += "Ninja" }

# Python 3
if (-not (Test-FileCmd "python")) { $missing += "Python 3" }

if ($missing.Count -gt 0) {
    Write-Host ("Missing: " + ($missing -join ", ")) -ForegroundColor Yellow
} else {
    Log "All core tools present (VS, CMake $cmakeVersion, Ninja, Python)"
}

# Python packages + Qt
$pipPresent = $false
if (Test-FileCmd "python") {
    Step "Installing Python packages (aqtinstall, pefile)"
    $pipRc = Invoke-CmdLine "python -m pip install --quiet --upgrade --disable-pip-version-check --no-warn-script-location aqtinstall pefile 2>nul"
    if ($pipRc -ne 0) { Fail "pip install failed (exit $pipRc)" }
    $pipPresent = $true
    Log "aqtinstall + pefile ready"
}

$qtPresent = $RunnerOnly -or (Test-Path $QT_ROOT)
if ($pipPresent -and -not $RunnerOnly) {
    if ($qtPresent) {
        Log "Qt found at $QT_ROOT"
    } else {
        Step "Downloading Qt $QT_VERSION ($QT_HOST, $QT_TARGET) via aqtinstall"
        $aqtRc = Invoke-CmdLine "python -m aqt install-qt $QT_HOST desktop $QT_VERSION $QT_TARGET --outputdir `"$QT_BASE`" 2>nul"
        if ($aqtRc -ne 0) { Fail "aqt install-qt failed (exit $aqtRc)" }
        if (-not (Test-Path $QT_ROOT)) { Fail "Qt not found at $QT_ROOT after install" }
        Log "Qt installed: $QT_ROOT"
    }
}

# Mesa lavapipe (software Vulkan ICD) -- bundled on Windows so the launcher can
# run paraLLEl-GS when the vendor Vulkan driver is broken (the AMD proprietary
# driver access-violates inside amdvlk64.dll during shader compilation on
# Polaris/GCN parts). Fetched here so the Windows build is reproducible.
$MESA_VERSION = "26.2.0"
$MESA_DIR = Join-Path $BUILD "mesa"
$MESA_DLL = Join-Path $MESA_DIR "x64\vulkan_lvp.dll"
if ($RunnerOnly) {
    Log "Mesa lavapipe skipped (-RunnerOnly)"
} elseif (Test-Path $MESA_DLL) {
    Log "Mesa lavapipe found at $MESA_DIR"
} elseif ($Install) {
    Step "Downloading Mesa lavapipe $MESA_VERSION"
    New-Item -ItemType Directory -Force -Path $MESA_DIR | Out-Null
    $sevenZr = Join-Path $MESA_DIR "7zr.exe"
    if (-not (Test-Path $sevenZr)) {
        Invoke-WebRequest "https://www.7-zip.org/a/7zr.exe" -OutFile $sevenZr -UseBasicParsing
    }
    $mesaArchive = Join-Path $MESA_DIR "mesa.7z"
    Invoke-WebRequest "https://github.com/pal1000/mesa-dist-win/releases/download/$MESA_VERSION/mesa3d-$MESA_VERSION-release-msvc.7z" -OutFile $mesaArchive -UseBasicParsing
    & $sevenZr x $mesaArchive "-o$MESA_DIR" -y | Out-Null
    Remove-Item $mesaArchive -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path $MESA_DLL)) { Fail "lavapipe not found after extraction ($MESA_DLL)" }
    Log "Mesa lavapipe installed: $MESA_DIR"
} else {
    Log "Mesa lavapipe missing (run with -Install to fetch it)"
}

if (-not $Install) {
    if ($missing.Count -gt 0 -or -not $qtPresent) {
        Write-Host "`nRun '.\install-deps-windows.ps1 -Install' to install missing components." -ForegroundColor Yellow
    }
    exit 0
}

# ─── Install phase ─────────────────────────────────────────────────────────────
if ($missing.Count -gt 0) {
    Step "Installing missing prerequisites via winget"

    if (-not $hasVS) {
        Step "Installing VS Build Tools 2022 (this takes several minutes)"
        $override = "--quiet --wait --norestart " +
            "--add Microsoft.VisualStudio.Workload.VCTools " +
            "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang " +
            "--add Microsoft.VisualStudio.Component.VC.Tools.LLVM " +
            "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
            "--add Microsoft.VisualStudio.Component.Windows11SDK.22621 " +
            "--includeRecommended"
        winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
            --accept-source-agreements --accept-package-agreements --override $override
        if ($LASTEXITCODE -ne 0) { Fail "winget install VS Build Tools failed (exit $LASTEXITCODE)" }
        Log "VS Build Tools installed"
    }

    if (-not (Test-FileCmd "cmake")) {
        winget install -e --id Kitware.CMake --accept-source-agreements --accept-package-agreements
        $env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Links" + [System.IO.Path]::PathSeparator + $env:PATH
        if (-not (Test-FileCmd "cmake")) { Fail "cmake installed but not on PATH" }
    }

    if (-not (Test-FileCmd "ninja")) {
        winget install -e --id Ninja-build.Ninja --accept-source-agreements --accept-package-agreements
        $env:PATH = "$env:LOCALAPPDATA\Microsoft\WinGet\Links" + [System.IO.Path]::PathSeparator + $env:PATH
        if (-not (Test-FileCmd "ninja")) { Fail "ninja installed but not on PATH" }
    }

    if (-not (Test-FileCmd "python")) {
        winget install -e --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements
        $env:PATH = "$env:LOCALAPPDATA\Programs\Python\Python312\Scripts" + [System.IO.Path]::PathSeparator + $env:PATH
        if (-not (Test-FileCmd "python")) { Fail "python installed but not on PATH; open a new terminal" }
    }
}

Step "Done"
Write-Host ("  All dependencies ready." + $(if ($RunnerOnly) { "" } else { " Now run '.\build-windows.ps1'." })) -ForegroundColor Green