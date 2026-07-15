param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$NativeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $NativeRoot "..")).Path
$BuildRoot = Join-Path $RepoRoot "build\native"

& (Join-Path $PSScriptRoot "build-native.ps1") -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$VisualStudioRoot = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$CTest = Join-Path $VisualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

& $CTest --test-dir $BuildRoot -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$NativeBin = Join-Path $BuildRoot "bin\$Configuration"
$NativeAppName = (-join [char[]]@(0x8D85, 0x7EA7, 0x4E2D, 0x952E)) + ".exe"
$NativeExePath = Join-Path $NativeBin $NativeAppName
if (-not (Test-Path -LiteralPath $NativeExePath -PathType Leaf)) {
    throw "Native application executable was not produced: $NativeExePath"
}
$NativeExe = Get-Item -LiteralPath $NativeExePath

$VersionInfo = $NativeExe.VersionInfo
if ($VersionInfo.FileDescription -ne $NativeExe.BaseName) {
    throw "Native FileDescription does not match the executable name: $($VersionInfo.FileDescription)"
}
if ($VersionInfo.ProductName -ne $NativeExe.BaseName) {
    throw "Native ProductName does not match the executable name: $($VersionInfo.ProductName)"
}
if ($VersionInfo.OriginalFilename -ne $NativeExe.Name) {
    throw "Native OriginalFilename does not match the executable name: $($VersionInfo.OriginalFilename)"
}

if ($Configuration -eq "RelWithDebInfo") {
    $Pdb = Join-Path $NativeBin ($NativeExe.BaseName + ".pdb")
    if (-not (Test-Path -LiteralPath $Pdb)) {
        throw "RelWithDebInfo did not produce the required PDB: $Pdb"
    }
    Write-Host "Diagnostic symbols: $Pdb"
}

Write-Host "Native self-check passed: $($NativeExe.FullName)"
