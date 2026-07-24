param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [switch]$RequireLatest
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# This is a host-agnostic post-upload check. It does not invoke, replace, or
# validate the updater implementation itself; Super Middle Key continues to
# use its Native C++ DesktopUpdateKit build and --verify-release gate.
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ConfigPath = Join-Path $ProjectRoot "release.config.json"
$Config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$Version = $Version.TrimStart('v', 'V')
$TagName = "v$Version"
$Repository = [string]$Config.repository
$AssetDirectory = Join-Path $ProjectRoot "build\release-assets\$TagName"

if (-not (Test-Path -LiteralPath $AssetDirectory -PathType Container)) {
    throw "Prepared release asset directory not found: $AssetDirectory"
}

function Invoke-GhJson {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "GitHub CLI command failed: gh $($Arguments -join ' ')"
    }

    return ($output | Out-String | ConvertFrom-Json)
}

function Get-WebResponseText {
    param(
        [Parameter(Mandatory = $true)]
        $Response
    )

    if ($Response.Content -is [byte[]]) {
        return [Text.Encoding]::UTF8.GetString([byte[]]$Response.Content).TrimStart([char]0xFEFF)
    }

    return ([string]$Response.Content).TrimStart([char]0xFEFF)
}

$Release = Invoke-GhJson @(
    "release", "view", $TagName,
    "--repo", $Repository,
    "--json", "tagName,isDraft,isPrerelease,targetCommitish,url,assets"
)

if ([string]$Release.tagName -ne $TagName) {
    throw "GitHub Release tag mismatch: expected $TagName, got $($Release.tagName)."
}

if ($RequireLatest -and ($Release.isDraft -or $Release.isPrerelease)) {
    throw "The latest stable release must not be a draft or prerelease."
}

$ExpectedAssetNames = @(
    [string]$Config.releaseAssetName,
    "$($Config.releaseAssetName).sha256",
    "update.json",
    "LICENSE",
    "NOTICE",
    "THIRD-PARTY-NOTICES.md",
    "DesktopUpdateKit-LICENSE.txt"
)

$RemoteAssets = @($Release.assets)
if ($RemoteAssets.Count -ne $ExpectedAssetNames.Count) {
    throw "GitHub Release asset count mismatch: expected $($ExpectedAssetNames.Count), got $($RemoteAssets.Count)."
}

foreach ($assetName in $ExpectedAssetNames) {
    $localPath = Join-Path $AssetDirectory $assetName
    if (-not (Test-Path -LiteralPath $localPath -PathType Leaf)) {
        throw "Prepared release asset not found: $localPath"
    }

    $matches = @($RemoteAssets | Where-Object { [string]$_.name -eq $assetName })
    if ($matches.Count -ne 1) {
        throw "GitHub Release must contain exactly one asset named '$assetName'."
    }

    $remoteAsset = $matches[0]
    $localFile = Get-Item -LiteralPath $localPath
    $localHash = (Get-FileHash -LiteralPath $localPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedDigest = "sha256:$localHash"

    if ([string]$remoteAsset.state -ne "uploaded") {
        throw "GitHub asset '$assetName' is not in the uploaded state."
    }
    if ([long]$remoteAsset.size -ne $localFile.Length) {
        throw "GitHub asset '$assetName' size does not match the prepared file."
    }
    if ([string]$remoteAsset.digest -ne $expectedDigest) {
        throw "GitHub asset '$assetName' digest does not match the prepared file."
    }
}

$ExeAssetName = [string]$Config.releaseAssetName
$LocalExePath = Join-Path $AssetDirectory $ExeAssetName
$LocalExe = Get-Item -LiteralPath $LocalExePath
$LocalExeHash = (Get-FileHash -LiteralPath $LocalExePath -Algorithm SHA256).Hash.ToLowerInvariant()
$CacheBust = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

if ($RequireLatest) {
    $LatestRelease = Invoke-GhJson @(
        "api", "repos/$Repository/releases/latest"
    )
    if ([string]$LatestRelease.tag_name -ne $TagName) {
        throw "GitHub latest release mismatch: expected $TagName, got $($LatestRelease.tag_name)."
    }

    $OnlineBaseUrl = "https://github.com/$Repository/releases/latest/download"
}
elseif (-not $Release.isDraft) {
    $OnlineBaseUrl = "https://github.com/$Repository/releases/download/$TagName"
}

if (-not [string]::IsNullOrWhiteSpace($OnlineBaseUrl)) {
    # Only the small manifest and checksum text are downloaded here. The large
    # executable is validated through GitHub's server-side digest and size.
    $ManifestResponse = Invoke-WebRequest `
        -UseBasicParsing `
        -Headers @{ "Cache-Control" = "no-cache" } `
        -Uri "$OnlineBaseUrl/update.json?verify=$CacheBust"
    $OnlineManifest = Get-WebResponseText $ManifestResponse | ConvertFrom-Json

    $Sha256Response = Invoke-WebRequest `
        -UseBasicParsing `
        -Headers @{ "Cache-Control" = "no-cache" } `
        -Uri "$OnlineBaseUrl/$ExeAssetName.sha256?verify=$CacheBust"
    $OnlineSha256 = (Get-WebResponseText $Sha256Response).Trim()

    if ([string]$OnlineManifest.version -ne $Version -or
        [string]$OnlineManifest.tag -ne $TagName -or
        [string]$OnlineManifest.asset -ne $ExeAssetName -or
        [string]$OnlineManifest.sha256Asset -ne "$ExeAssetName.sha256") {
        throw "Online update.json release identity does not match $TagName."
    }
    if ([long]$OnlineManifest.size -ne $LocalExe.Length -or
        [string]$OnlineManifest.sha256 -ne $LocalExeHash) {
        throw "Online update.json size or SHA-256 does not match the prepared executable."
    }
    if (-not $OnlineSha256.StartsWith("$LocalExeHash  $ExeAssetName", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Online checksum asset does not match the prepared executable."
    }
}

Write-Host "Online release verification passed for $Repository $TagName."
Write-Host "Verified GitHub asset metadata, sizes, server-side SHA-256 digests, and update metadata."
Write-Host "The executable was not downloaded again."
