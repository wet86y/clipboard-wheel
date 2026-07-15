$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Config = Get-Content -LiteralPath (Join-Path $Root "release.config.json") -Encoding UTF8 -Raw | ConvertFrom-Json

& (Join-Path $Root "native\scripts\build-native.ps1") -Configuration RelWithDebInfo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Source = Join-Path $Root ("build\native\bin\RelWithDebInfo\" + $Config.publishedExeName)
$SourcePdb = [IO.Path]::ChangeExtension($Source, ".pdb")
$Output = Join-Path $Root "build\bin\RelWithDebInfo"
if (Test-Path -LiteralPath $Output) { Remove-Item -LiteralPath $Output -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Output | Out-Null
Copy-Item -LiteralPath $Source -Destination (Join-Path $Output $Config.publishedExeName)
Copy-Item -LiteralPath $SourcePdb -Destination (Join-Path $Output ([IO.Path]::GetFileName($SourcePdb)))

$Executable = Join-Path $Output $Config.publishedExeName
Write-Host "Diagnostic executable: $Executable"
Start-Process -FilePath $Executable
