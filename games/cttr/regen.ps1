# Regenerate the CTTR runner sources after a recompiler change (only changed files are rewritten; see regen.py).
# Loads the VS environment like build.ps1, then runs regen.py.
#   powershell -ExecutionPolicy Bypass -File games\cttr\regen.ps1
egen.ps1
$ErrorActionPreference = "Stop"
$ROOT = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$BUILD = Join-Path $ROOT "build"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
foreach ($line in (& cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set")) {
    if ($line -match "^([^=]+)=(.*)$") { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process") }
}
$env:PATH = $env:PATH + [IO.Path]::PathSeparator + (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links")

if (-not (Test-Path (Join-Path $BUILD "build.ninja"))) {
    & cmake -S $ROOT -B $BUILD -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_STUDIO=OFF -DPS2X_SHOW_WINDOWS_CONSOLE=ON
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

& python (Join-Path $PSScriptRoot "regen.py") @args
exit $LASTEXITCODE
