param(
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$NativeRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $NativeRoot "..")).Path
$DiagnosticBin = Join-Path $RepoRoot "build\native\bin\RelWithDebInfo"

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build-native.ps1") -Configuration RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$ExecutablePath = Join-Path $DiagnosticBin "超级中键.exe"
$Executable = if (Test-Path -LiteralPath $ExecutablePath) {
    Get-Item -LiteralPath $ExecutablePath
} else {
    $null
}
if ($null -eq $Executable) {
    throw "Diagnostic executable was not found under: $DiagnosticBin"
}

$LogDirectory = Join-Path (Join-Path $env:LOCALAPPDATA $Executable.BaseName) "logs"
$Process = Start-Process -FilePath $Executable.FullName -PassThru
Start-Sleep -Milliseconds 800
$Log = Get-ChildItem -LiteralPath $LogDirectory -Filter "native-diagnostic-*.log" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

Write-Host "Diagnostic process: $($Process.Id)"
Write-Host "Executable: $($Executable.FullName)"
if ($null -ne $Log) {
    Write-Host "Log: $($Log.FullName)"
} else {
    Write-Warning "The diagnostic process started, but its log has not appeared yet."
}
