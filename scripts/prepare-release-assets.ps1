param(
    [string]$Version = "",
    [string]$ReleaseNotes = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$SharedRoot = Join-Path $ProjectRoot "shared\DesktopUpdateKit"
$SharedScriptPath = Join-Path $SharedRoot "tools\Prepare-ReleaseAssets.ps1"
if (-not (Test-Path -LiteralPath $SharedScriptPath)) {
    throw "DesktopUpdateKit submodule is missing. Run: git submodule update --init --recursive"
}
$SharedScript = (Resolve-Path -LiteralPath $SharedScriptPath).Path
$ConfigPath = Join-Path $ProjectRoot "release.config.json"

& $SharedScript -ProjectRoot $ProjectRoot -ConfigPath $ConfigPath -Version $Version -ReleaseNotes $ReleaseNotes
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
