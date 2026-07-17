param([switch]$Clean)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Config = Get-Content -LiteralPath (Join-Path $ProjectRoot "release.config.json") -Encoding UTF8 -Raw | ConvertFrom-Json
$CMakeFile = Get-Content -LiteralPath (Join-Path $ProjectRoot "native\CMakeLists.txt") -Encoding UTF8 -Raw
$VersionMatch = [regex]::Match($CMakeFile, 'set\(SMK_VERSION\s+"([^"]+)"\)')
if (-not $VersionMatch.Success) { throw "无法从 native\CMakeLists.txt 读取版本号。" }
$Version = $VersionMatch.Groups[1].Value

& (Join-Path $ProjectRoot "native\scripts\build-native.ps1") -Configuration RelWithDebInfo -Clean:$Clean
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$BuildBin = Join-Path $ProjectRoot "build\native\bin\RelWithDebInfo"
$SourceExe = Join-Path $BuildBin $Config.publishedExeName
$SourcePdb = [IO.Path]::ChangeExtension($SourceExe, ".pdb")
foreach ($file in @($SourceExe, $SourcePdb)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "缺少诊断构建输出：$file" }
}

$ArtifactRoot = Join-Path $ProjectRoot "artifacts"
$PackageName = "超级中键-diagnostic-$Version-r2"
$PackageDirectory = Join-Path $ArtifactRoot $PackageName
$ZipPath = Join-Path $ArtifactRoot ($PackageName + ".zip")
if (-not $PackageDirectory.StartsWith($ProjectRoot + "\", [StringComparison]::OrdinalIgnoreCase)) {
    throw "拒绝清理仓库外的诊断包目录。"
}
if (Test-Path -LiteralPath $PackageDirectory) { Remove-Item -LiteralPath $PackageDirectory -Recurse -Force }
if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
New-Item -ItemType Directory -Force -Path $PackageDirectory | Out-Null

Copy-Item -LiteralPath $SourceExe -Destination (Join-Path $PackageDirectory $Config.publishedExeName)
Copy-Item -LiteralPath $SourcePdb -Destination (Join-Path $PackageDirectory ([IO.Path]::GetFileName($SourcePdb)))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "collect-diagnostic-logs.ps1") -Destination (Join-Path $PackageDirectory "collect-diagnostic-logs.ps1")
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "collect-diagnostic-logs.cmd") -Destination (Join-Path $PackageDirectory "收集诊断日志.cmd")
Copy-Item -LiteralPath (Join-Path $ProjectRoot "docs\DIAGNOSTIC_BUILD.md") -Destination (Join-Path $PackageDirectory "使用说明.md")

Compress-Archive -Path (Join-Path $PackageDirectory "*") -DestinationPath $ZipPath -CompressionLevel Optimal
Write-Host "Diagnostic package directory: $PackageDirectory"
Write-Host "Diagnostic package zip: $ZipPath"
