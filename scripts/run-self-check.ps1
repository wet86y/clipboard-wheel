$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$ProjectDir = Join-Path $Root "src\ClipboardWheel"
$Project = Join-Path $ProjectDir "ClipboardWheel.csproj"
$sharedToolkitRoot = Join-Path $Root "shared\DesktopUpdateKit"
if (-not (Test-Path -LiteralPath (Join-Path $sharedToolkitRoot "src\DesktopUpdateKit\UpdateClient.cs"))) {
    throw "DesktopUpdateKit submodule is missing. Run: git submodule update --init --recursive"
}

Write-Host "Building diagnostic configuration..."
dotnet build $Project -c Release --nologo

$appSource = Get-Content -Raw (Join-Path $ProjectDir "App.xaml.cs")
$traceSource = Get-Content -Raw (Join-Path $ProjectDir "Services\PasteTrace.cs")
$sharedReleaseScriptPath = Join-Path $sharedToolkitRoot "tools\Build-Release.ps1"
$releaseScript = Get-Content -Raw $sharedReleaseScriptPath
$monitorSource = Get-Content -Raw (Join-Path $ProjectDir "Services\ClipboardMonitorService.cs")
$launchSource = Get-Content -Raw (Join-Path $ProjectDir "Services\MouseHookService.cs")
$settingsSource = Get-Content -Raw (Join-Path $ProjectDir "Services\SettingsService.cs")
$settingsModelSource = Get-Content -Raw (Join-Path $ProjectDir "Models\AppSettings.cs")
$historySource = Get-Content -Raw (Join-Path $ProjectDir "Services\ClipboardHistoryService.cs")
$overlaySource = Get-Content -Raw (Join-Path $ProjectDir "UI\WheelOverlay.xaml.cs")
$nativeMethodsSource = Get-Content -Raw (Join-Path $ProjectDir "Native\NativeMethods.cs")
$autoStartSource = Get-Content -Raw (Join-Path $ProjectDir "Services\AutoStartService.cs")
$settingsWindowSource = Get-Content -Raw (Join-Path $ProjectDir "UI\SettingsWindow.xaml.cs")
$manifestSource = Get-Content -Raw (Join-Path $ProjectDir "app.manifest")
$sharedUpdateRoot = Join-Path $sharedToolkitRoot "src\DesktopUpdateKit"
$sharedUpdateModels = Get-Content -Raw (Join-Path $sharedUpdateRoot "UpdateModels.cs")
$sharedUpdateClient = Get-Content -Raw (Join-Path $sharedUpdateRoot "UpdateClient.cs")
$sharedUpdateNodeCache = Get-Content -Raw (Join-Path $sharedUpdateRoot "UpdateNodeCache.cs")
$sharedUpdateSession = Get-Content -Raw (Join-Path $sharedUpdateRoot "UpdateDownloadSession.cs")
$sharedUpdateLauncher = Get-Content -Raw (Join-Path $sharedUpdateRoot "UpdateLauncher.cs")
$sharedAboutGuidelines = Get-Content -Raw (Join-Path $sharedToolkitRoot "docs\ABOUT_PAGE_GUIDELINES.md")
$sharedReleaseConfigSchema = Get-Content -Raw (Join-Path $sharedToolkitRoot "schemas\release-config.schema.json")
$updaterStubSource = Get-Content -Raw (Join-Path $sharedToolkitRoot "src\UpdaterStub\Program.cs")
$sharedReleaseRules = Get-Content -Raw (Join-Path $sharedToolkitRoot "tools\ReleaseRules.ps1")
$sharedPrepareReleaseAssets = Get-Content -Raw (Join-Path $sharedToolkitRoot "tools\Prepare-ReleaseAssets.ps1")
$sharedPublishRelease = Get-Content -Raw (Join-Path $sharedToolkitRoot "tools\Publish-Release.ps1")
$releaseConfigSource = Get-Content -Raw (Join-Path $Root "release.config.json")

$checks = @(
    @{ Name = "no Assembly.Location runtime gate"; Passed = $appSource -notmatch "Assembly\.Location" },
    @{ Name = "diagnostic trace is conditional"; Passed = $traceSource -match '\[Conditional\("PASTE_TRACE_ENABLED"\)\]' },
    @{ Name = "release publish disables trace"; Passed = $releaseScript -match "/p:PasteTraceEnabled=false" },
    @{ Name = "release publish excludes debug symbols"; Passed = $releaseScript -match "/p:DebugSymbols=false" -and $releaseScript -match "/p:DebugType=None" },
    @{ Name = "clipboard listener source is non-zero sized"; Passed = $monitorSource -match 'Width = 1' -and $monitorSource -match 'Height = 1' },
    @{ Name = "unmanaged launches have a generic duplicate guard"; Passed = $launchSource -match 'UnmanagedLaunchLeaseSeconds' -and $launchSource -match 'IsUnmanagedLeaseActive' },
    @{ Name = "shared WPS documents cannot bind transient windows"; Passed = $launchSource -match 'IsSharedWpsDocumentLaunch' -and $launchSource -match 'return IntPtr.Zero;' },
    @{ Name = "WPS host activation preserves its window state"; Passed = $launchSource -match 'preserveWindowState: true' -and $launchSource -match 'forceForeground: true' },
    @{ Name = "folder bindings bypass Explorer process sessions"; Passed = $launchSource -match 'RemoveLaunchSessions\(trackingKey\)' -and $launchSource -match 'ExtendedAction_folder_started' },
    @{ Name = "hidden tracked windows reactivate before close or minimize"; Passed = $launchSource -match 'IsWindowVisible\(windowHandle\)' -and $launchSource -match 'ExtendedAction_shortcut_hidden_window_reactivated' },
    @{ Name = "unverified resident processes have a bounded relaunch lease"; Passed = $launchSource -match 'ForceLaunchCooldownMs' -and $launchSource -match 'UnmanagedLaunchLeaseSeconds = 6' -and $launchSource -match 'WpsUnmanagedLaunchLeaseSeconds = 6' },
    @{ Name = "extended labels use measured overflow marquee instead of fixed truncation"; Passed = $overlaySource -match 'CountFullyMaskedTextElements' -and $overlaySource -match 'fullyMaskedCharacters > 2' -and $overlaySource -notmatch 'name\.Length <= 8' },
    @{ Name = "wheel overlay reasserts Win32 topmost without activation"; Passed = $overlaySource -match 'ReassertTopmost' -and $nativeMethodsSource -match 'HWND_TOPMOST' -and $nativeMethodsSource -match 'SWP_NOACTIVATE' },
    @{ Name = "startup normalizes downloaded executable names through shared stub"; Passed = $appSource -match 'EnsureCanonicalExecutableNameAsync' -and $sharedUpdateLauncher -match 'ExecutableRenameTransaction' -and $updaterStubSource -match '--rename-transaction' -and $updaterStubSource -match 'WaitForHealth' },
    @{ Name = "startup only rewrites changed settings"; Passed = $settingsSource -match 'if \(!string\.Equals\(json, normalizedJson' },
    @{ Name = "runtime clipboard history is released on exit"; Passed = $historySource -match 'Volatile\.Write\(ref _wheelSnapshot, Array\.Empty<ClipboardEntry>\(\)\)' },
    @{ Name = "clipboard capture accepts MIME encoded image formats"; Passed = $historySource -match 'image/png' -and $historySource -match 'image/jpeg' -and $historySource -match 'EncodedImageFormats' },
    @{ Name = "update UI uses in-page install controls"; Passed = $settingsWindowSource -match 'AboutInstallUpdateButton' -and $settingsWindowSource -match 'PauseResumeDownload_Click' },
    @{ Name = "shared updater supports pause and cancellation"; Passed = $sharedUpdateModels -match 'UpdateDownloadControl' -and $sharedUpdateClient -match 'WaitIfPausedAsync' },
    @{ Name = "parallel transport is implemented only in shared updater"; Passed = $sharedUpdateModels -match 'UpdateDownloadOptions' -and $sharedUpdateClient -match 'RangeHeaderValue' -and $sharedUpdateClient -match 'DownloadInParallelAsync' -and $settingsWindowSource -notmatch 'RangeHeaderValue' },
    @{ Name = "GitHub download nodes stay in the shared updater"; Passed = $sharedUpdateModels -match 'UpdateDownloadNode' -and $sharedUpdateClient -match 'BuiltInNodes' -and $sharedUpdateClient -match 'ProbeNodeAsync' -and $sharedUpdateClient -match 'DownloadSingleWithRetryAsync' -and $sharedUpdateNodeCache -match 'LastSuccessNodeId' -and $settingsWindowSource -notmatch 'gh-proxy' },
    @{ Name = "download sessions outlive settings windows"; Passed = $sharedUpdateSession -match 'PauseWhenUiCloses' -and $sharedUpdateSession -match 'ContinueInBackground' -and $settingsWindowSource -match 'BackgroundDownload_Click' -and $settingsWindowSource -match 'SharedUpdateSession\.PauseWhenUiCloses' },
    @{ Name = "users can select direct download or switch acceleration nodes"; Passed = $sharedUpdateModels -match 'RequestNextAcceleratedNode' -and $sharedUpdateModels -match 'SetUseAccelerationNodes' -and $sharedUpdateSession -match 'RequestNextAcceleratedNode' -and $settingsWindowSource -match 'AccelerationToggle_Changed' -and $settingsWindowSource -match 'SwitchUpdateNode_Click' },
    @{ Name = "administrator mode is optional and preserves asInvoker default"; Passed = $settingsWindowSource -match 'AdministratorModeToggle_Changed' -and $settingsWindowSource -match 'ElevationService.TryRestart' -and $appSource -match 'RunAsAdministratorEnabled' -and $manifestSource -match 'level="asInvoker"' },
    @{ Name = "about page contract stays host-owned and schema-backed"; Passed = $releaseConfigSource -match '"about"' -and $sharedAboutGuidelines -match '宿主项目负责' -and $sharedAboutGuidelines -match '立即安装' -and $sharedReleaseConfigSchema -match '"about"' },
    @{ Name = "release tooling enforces nodes and committed worktrees"; Passed = $releaseConfigSource -match '"downloadNodes"' -and $sharedReleaseRules -match 'Assert-DownloadNodes' -and $sharedReleaseRules -match 'Assert-CleanGitWorktree' -and $sharedPrepareReleaseAssets -match 'Assert-PreparedReleaseAssets' -and $sharedPublishRelease -match 'Assert-CleanGitWorktree' },
    @{ Name = "autostart defaults off and uses current-user Run key"; Passed = $settingsModelSource -match 'AutoStartEnabled \{ get; set; \} = false' -and $autoStartSource -match 'Registry\.CurrentUser' -and $autoStartSource -match 'CurrentVersion\\Run' }
)

$failed = @($checks | Where-Object { -not $_.Passed })
foreach ($check in $checks) {
    $status = if ($check.Passed) { "PASS" } else { "FAIL" }
    Write-Host "$status`: $($check.Name)"
}

if ($failed.Count -gt 0) {
    throw "Self-check failed: $($failed.Name -join ', ')"
}

Write-Host "Self-check passed."
