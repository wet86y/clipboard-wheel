$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$ProjectDir = Join-Path $Root "src\ClipboardWheel"
Set-Location $ProjectDir

dotnet restore
dotnet run --project (Join-Path $ProjectDir "ClipboardWheel.csproj") -c Release
