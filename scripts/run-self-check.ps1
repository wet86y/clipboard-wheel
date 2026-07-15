$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Config = Get-Content -LiteralPath (Join-Path $Root "release.config.json") -Encoding UTF8 -Raw | ConvertFrom-Json

foreach ($configuration in @("Release", "RelWithDebInfo")) {
    & (Join-Path $Root "native\scripts\run-native-self-check.ps1") -Configuration $configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& (Join-Path $PSScriptRoot "build-release.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$DiagnosticBin = Join-Path $Root "build\native\bin\RelWithDebInfo"
$DiagnosticExe = Join-Path $DiagnosticBin $Config.publishedExeName
$DiagnosticPdb = [IO.Path]::ChangeExtension($DiagnosticExe, ".pdb")
if (-not (Test-Path -LiteralPath $DiagnosticExe -PathType Leaf) -or
    -not (Test-Path -LiteralPath $DiagnosticPdb -PathType Leaf)) {
    throw "RelWithDebInfo must produce the application EXE and PDB."
}

$PublishedDirectory = Join-Path $Root $Config.artifactsDirectory
$PublishedFiles = @(Get-ChildItem -LiteralPath $PublishedDirectory -File)
if ($PublishedFiles.Count -ne 1 -or $PublishedFiles[0].Name -ne $Config.publishedExeName) {
    throw "The Release artifact directory contains unexpected files."
}

Write-Host "Repository self-check passed."
