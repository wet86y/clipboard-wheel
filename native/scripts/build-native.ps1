param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$NativeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $NativeRoot "..")).Path
$BuildRoot = Join-Path $RepoRoot "build\native"
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw "Visual Studio Installer (vswhere.exe) was not found."
}

$VisualStudioRoot = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    throw "The MSVC x64 toolchain is not installed."
}

$CMake = Join-Path $VisualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $CMake)) {
    throw "Visual Studio's bundled CMake was not found."
}

if ($Clean -and (Test-Path -LiteralPath $BuildRoot)) {
    $resolvedBuild = (Resolve-Path -LiteralPath $BuildRoot).Path
    $resolvedRepo = (Resolve-Path -LiteralPath $RepoRoot).Path
    if (-not $resolvedBuild.StartsWith($resolvedRepo + "\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository."
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

& $CMake -S $NativeRoot -B $BuildRoot -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $CMake --build $BuildRoot --config $Configuration --parallel
exit $LASTEXITCODE
