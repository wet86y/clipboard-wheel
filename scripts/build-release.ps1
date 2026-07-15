param([switch]$Clean)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Config = Get-Content -LiteralPath (Join-Path $ProjectRoot "release.config.json") -Encoding UTF8 -Raw | ConvertFrom-Json
$BuildScript = Join-Path $ProjectRoot "native\scripts\build-native.ps1"

& $BuildScript -Configuration Release -Clean:$Clean
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$NativeExe = Join-Path $ProjectRoot ("build\native\bin\Release\" + $Config.publishedExeName)
if (-not (Test-Path -LiteralPath $NativeExe -PathType Leaf)) {
    throw "Native Release executable was not produced: $NativeExe"
}

$LocalOutput = Join-Path $ProjectRoot "build\bin\Release"
$Artifacts = Join-Path $ProjectRoot $Config.artifactsDirectory
foreach ($directory in @($LocalOutput, $Artifacts)) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

$LocalExe = Join-Path $LocalOutput $Config.publishedExeName
$PublishedExe = Join-Path $Artifacts $Config.publishedExeName
Copy-Item -LiteralPath $NativeExe -Destination $LocalExe
Copy-Item -LiteralPath $NativeExe -Destination $PublishedExe

foreach ($executable in @($LocalExe, $PublishedExe)) {
    foreach ($argument in @($Config.releaseVerificationArguments)) {
        $process = Start-Process -FilePath $executable -ArgumentList ([string]$argument) -PassThru
        if (-not $process.WaitForExit(30000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "Release verification timed out: $executable $argument"
        }
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            throw "Release verification failed with exit code $($process.ExitCode): $executable $argument"
        }
    }
}

$ArtifactFiles = @(Get-ChildItem -LiteralPath $Artifacts -File)
if ($ArtifactFiles.Count -ne 1 -or $ArtifactFiles[0].Name -ne $Config.publishedExeName) {
    throw "The public artifact directory must contain only $($Config.publishedExeName)."
}

Write-Host "Native Release package: $PublishedExe"
