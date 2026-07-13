using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Threading;
using ClipboardWheel.Models;
using ClipboardWheel.Native;
using ClipboardWheel.UI;

namespace ClipboardWheel.Services;

public sealed class MouseHookService : IDisposable
{
    private static readonly object LaunchSessionLock = new();
    // A binding can legitimately start several independent windows. Keep the
    // exact windows started by this app instead of discovering all processes
    // with the same executable name (which could affect the user's own windows).
    private static readonly Dictionary<string, List<TrackedLaunchSession>> LaunchSessions = new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, LaunchAttemptState> LaunchAttempts = new(StringComparer.OrdinalIgnoreCase);
    // Some single-instance and tabbed applications acknowledge a shell launch
    // through a short-lived helper process, then reuse an existing host window.
    // Keep that launch session briefly even when no new HWND was observable so a
    // second trigger cannot create a stream of duplicate launch requests.
    private const int UnmanagedLaunchLeaseSeconds = 6;
    private const int WpsUnmanagedLaunchLeaseSeconds = 6;
    private const int ForceLaunchCooldownMs = 1200;

    private sealed class LaunchAttemptState
    {
        public DateTime LastAttemptUtc { get; set; }
        public int AttemptCount { get; set; }
    }

    private sealed class TrackedLaunchSession
    {
        public TrackedLaunchSession(
            Process? process,
            int processId,
            IntPtr windowHandle,
            string? windowExecutablePath,
            IReadOnlySet<IntPtr> windowsBeforeLaunch,
            DateTime? unmanagedUntilUtc,
            string? documentPath,
            IntPtr wpsHostWindowHandle,
            bool isSharedWpsDocumentLaunch)
        {
            Process = process;
            ProcessId = processId;
            WindowHandle = windowHandle;
            WindowExecutablePath = windowExecutablePath;
            WindowsBeforeLaunch = new HashSet<IntPtr>(windowsBeforeLaunch);
            StartedUtc = DateTime.UtcNow;
            UnmanagedUntilUtc = unmanagedUntilUtc;
            DocumentPath = documentPath;
            WpsHostWindowHandle = wpsHostWindowHandle;
            IsSharedWpsDocumentLaunch = isSharedWpsDocumentLaunch;
        }

        public Process? Process { get; }
        public int ProcessId { get; }
        public IntPtr WindowHandle { get; private set; }
        public string? WindowExecutablePath { get; }
        public IReadOnlySet<IntPtr> WindowsBeforeLaunch { get; }
        public DateTime StartedUtc { get; }
        public DateTime? UnmanagedUntilUtc { get; private set; }
        public string? DocumentPath { get; }
        public IntPtr WpsHostWindowHandle { get; private set; }
        public bool IsSharedWpsDocumentLaunch { get; }
        public bool WpsTabAutomationUnavailable { get; private set; }

        public bool IsUnmanagedLeaseActive => UnmanagedUntilUtc is { } until && until > DateTime.UtcNow;

        public void RebindWindow(IntPtr windowHandle)
        {
            WindowHandle = windowHandle;
            // Once the launched session has a distinct window, it is fully
            // manageable and no longer needs the duplicate-launch guard.
            UnmanagedUntilUtc = null;
        }

        public void RebindWpsHostWindow(IntPtr windowHandle) => WpsHostWindowHandle = windowHandle;

        public void DisableWpsTabAutomation() => WpsTabAutomationUnavailable = true;
    }

    private sealed class ShortcutLaunchInfo
    {
        public string TargetPath { get; init; } = string.Empty;
        public string Arguments { get; init; } = string.Empty;
        public string WorkingDirectory { get; init; } = string.Empty;
    }

    // Application-specific behavior lives here rather than in the generic
    // window-session pipeline. Unknown applications simply use the safe default:
    // launch normally and only manage a newly observed top-level window.
    private sealed class ApplicationCompatibilityProfile
    {
        public ApplicationCompatibilityProfile(
            string? mainWindowClass,
            int windowWaitAttempts,
            string? programArguments = null,
            string? documentArgumentsPrefix = null)
        {
            MainWindowClass = mainWindowClass;
            WindowWaitAttempts = windowWaitAttempts;
            ProgramArguments = programArguments;
            DocumentArgumentsPrefix = documentArgumentsPrefix;
        }

        public string? MainWindowClass { get; }
        public int WindowWaitAttempts { get; }
        public string? ProgramArguments { get; }
        public string? DocumentArgumentsPrefix { get; }
    }

    private static readonly IReadOnlyDictionary<string, ApplicationCompatibilityProfile> CompatibilityProfiles =
        new Dictionary<string, ApplicationCompatibilityProfile>(StringComparer.OrdinalIgnoreCase)
        {
            // Excel's splash window is not stable. XLMAIN is the workbook window;
            // /x is Microsoft's documented separate-process switch.
            ["excel.exe"] = new("XLMAIN", 80, "/x", "/x"),
            // Word documents use their own top-level OpusApp windows. /w keeps a
            // program-only shortcut separate; document launches retain the path.
            ["winword.exe"] = new("OpusApp", 60, "/w"),
            // PowerPoint uses a distinct frame window but has no documented
            // separate-instance switch, so only the main-window filter applies.
            ["powerpnt.exe"] = new("PPTFrameClass", 60),
            // WPS executables use the safe generic policy. The extended startup
            // window allows their shell handoff to settle without assuming an
            // undocumented command-line switch or window class.
            ["wps.exe"] = new(null, 50),
            ["et.exe"] = new(null, 50),
            ["wpp.exe"] = new(null, 50),
            ["wpspdf.exe"] = new(null, 50)
        };

    private readonly SettingsService _settingsService;
    private readonly ClipboardHistoryService _historyService;
    private readonly PasteService _pasteService;
    private readonly WheelOverlay _wheelOverlay;
    private readonly NativeMethods.LowLevelMouseProc _callback;
    private readonly DispatcherTimer? _failSafeTimer;
    private readonly SemaphoreSlim _wheelActionGate = new(1, 1);
    private IntPtr _hookId = IntPtr.Zero;
    // _wheelActive is read by the hook thread (WM_MOUSEMOVE / WM_MBUTTONUP)
    // AND by the UI thread inside DispatchPendingPointerUpdate. Mark volatile so
    // the UI thread never observes a stale "true" after ConfirmSelection has
    // already hidden the wheel.
    private volatile bool _wheelActive;
    private DateTime _pauseUntilUtc = DateTime.MinValue;
    private DateTime _middleDownUtc = DateTime.MinValue;
    private DateTime _wheelActivatedUtc = DateTime.MinValue;
    private int _releaseVerifyGeneration;
    private int _confirmSelectionScheduled;
    private bool _lockToggledDuringWheel;
    private const int MiddleReleaseDebounceMs = 70;
    private const int WheelFailSafeMs = 8000;
    private const int WheelResetCooldownMs = 50;

    // UpdatePointer coalescing: WM_MOUSEMOVE arrives at ~125 Hz (every ~8 ms).
    // If we BeginInvoke each one, the UI thread accumulates a backlog of up to
    // hundreds of operations while the user is steering the wheel, and the
    // subsequent Dispatcher.Invoke(HideWheel) on WM_MBUTTONUP has to drain
    // that backlog synchronously before the paste path can start. That is the
    // dominant component of the perceived 1 s paste latency.
    //
    // Coalescing keeps a single pending UpdatePointer at a time. Every new
    // WM_MOUSEMOVE only updates _latestPointer and skips BeginInvoke when one
    // is already pending. The pending UpdatePointer always picks up the
    // freshest position when it runs, so visual smoothness is unchanged.
    private Point _latestPointer;
    private int _pointerUpdateScheduled; // 0 / 1, manipulated via Interlocked

    public MouseHookService(SettingsService settingsService, ClipboardHistoryService historyService, PasteService pasteService, WheelOverlay wheelOverlay)
    {
        _settingsService = settingsService;
        _historyService = historyService;
        _pasteService = pasteService;
        _wheelOverlay = wheelOverlay;
        _callback = HookCallback;

        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is not null)
        {
            _failSafeTimer = new DispatcherTimer(DispatcherPriority.Background, dispatcher)
            {
                Interval = TimeSpan.FromMilliseconds(500)
            };
            _failSafeTimer.Tick += (_, _) =>
            {
                if (_wheelActive && IsWheelFailSafeExpired())
                {
                    PasteTrace.Mark("Wheel_fail_safe_timer_timeout");
                    ScheduleWheelClose(executeSelection: false, "fail_safe_timer_timeout");
                }
            };
            _failSafeTimer.Start();
        }
    }

    public bool IsCaptureEnabled
    {
        get => _settingsService.Current.Mouse.MiddleButtonCaptureEnabled && DateTime.UtcNow >= _pauseUntilUtc;
        set
        {
            var settings = _settingsService.Current;
            settings.Mouse.MiddleButtonCaptureEnabled = value;
            _settingsService.Save(settings);
        }
    }

    public void Start()
    {
        if (_hookId != IntPtr.Zero)
        {
            return;
        }
        _hookId = NativeMethods.InstallLowLevelMouseHook(_callback);
        if (_hookId == IntPtr.Zero)
        {
            PasteTrace.Mark($"MouseHook_install_failed win32={Marshal.GetLastWin32Error()}");
        }
    }

    private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            return HookCallbackCore(nCode, wParam, lParam);
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"MouseHook_callback_exception {ex.GetType().Name}: {ex.Message}");
            return NativeMethods.CallNextHookEx(_hookId, nCode, wParam, lParam);
        }
    }

    private IntPtr HookCallbackCore(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0)
        {
            return NativeMethods.CallNextHookEx(_hookId, nCode, wParam, lParam);
        }

        var msg = wParam.ToInt32();
        var hook = Marshal.PtrToStructure<NativeMethods.MSLLHOOKSTRUCT>(lParam);
        var point = new Point(hook.pt.x, hook.pt.y);

        if (_wheelActive && IsWheelFailSafeExpired())
        {
            PasteTrace.Mark("Wheel_fail_safe_timeout");
            ScheduleWheelClose(executeSelection: false, "fail_safe_timeout");
            return NativeMethods.CallNextHookEx(_hookId, nCode, wParam, lParam);
        }

        switch (msg)
        {
            case NativeMethods.WM_MBUTTONDOWN:
                if (ShouldCapture())
                {
                    _middleDownUtc = DateTime.UtcNow;
                    Interlocked.Increment(ref _releaseVerifyGeneration);
                    ShowWheel(point);
                    return Suppress();
                }
                break;

            case NativeMethods.WM_MOUSEMOVE:
                if (_wheelActive)
                {
                    _latestPointer = point;
                    // Coalesce: only schedule if no UpdatePointer is already pending.
                    // CompareExchange returns the previous value; == 0 means we won
                    // the race to claim the "schedule me" slot.
                    if (Interlocked.CompareExchange(ref _pointerUpdateScheduled, 1, 0) == 0)
                    {
                        var app = Application.Current;
                        if (app is not null)
                        {
                            try
                            {
                                app.Dispatcher.BeginInvoke(
                                    new Action(DispatchPendingPointerUpdate),
                                    DispatcherPriority.Render);
                            }
                            catch (InvalidOperationException)
                            {
                                Interlocked.Exchange(ref _pointerUpdateScheduled, 0);
                            }
                        }
                        else
                        {
                            Interlocked.Exchange(ref _pointerUpdateScheduled, 0);
                        }
                    }
                    // Let WM_MOUSEMOVE pass through so the cursor stays responsive
                    // in the application behind the wheel (issue found 2026-07-07).
                    // The wheel overlay sectors still update via BeginInvoke above.
                }
                break;

            case NativeMethods.WM_MOUSEWHEEL:
            case NativeMethods.WM_MOUSEHWHEEL:
                if (_wheelActive)
                {
                    return Suppress();
                }
                break;

            case NativeMethods.WM_RBUTTONDOWN:
                if (_wheelActive)
                {
                    ToggleWheelSelectionLock();
                    return Suppress();
                }
                break;

            case NativeMethods.WM_RBUTTONUP:
                if (_wheelActive)
                {
                    return Suppress();
                }
                break;

            case NativeMethods.WM_MBUTTONUP:
                if (_wheelActive)
                {
                    var heldMs = (DateTime.UtcNow - _middleDownUtc).TotalMilliseconds;
                    if (heldMs < MiddleReleaseDebounceMs)
                    {
                        PasteTrace.Mark($"WM_MBUTTONUP_debounce held_ms={heldMs:0.0}");
                        ScheduleReleaseVerification();
                        return Suppress();
                    }

                    PasteTrace.Mark($"WM_MBUTTONUP pending_updates={_pointerUpdateScheduled}");
                    ScheduleWheelClose(executeSelection: true, "middle_button_up");
                    return Suppress();
                }
                break;
        }

        return NativeMethods.CallNextHookEx(_hookId, nCode, wParam, lParam);
    }

    private bool ShouldCapture()
    {
        return IsCaptureEnabled;

        // Legacy per-app capture modes are intentionally disabled. Capture is
        // now controlled only by the user's global tray/settings switch.
        //
        // var settings = _settingsService.Current;
        // var processName = ForegroundWindowService.GetForegroundProcessName();
        // var mode = ResolveProcessMode(processName, settings);
        //
        // if (mode.Equals("disabled", StringComparison.OrdinalIgnoreCase))
        // {
        //     return false;
        // }
        //
        // if (mode.Equals("modifierOnly", StringComparison.OrdinalIgnoreCase))
        // {
        //     return NativeMethods.IsKeyDown(NativeMethods.VK_CONTROL);
        // }
        //
        // return true;
    }

    private static string ResolveProcessMode(string? processName, AppSettings settings)
    {
        if (!string.IsNullOrWhiteSpace(processName) && settings.ProcessRules.TryGetValue(processName, out var exact))
        {
            return exact;
        }

        if (settings.ProcessRules.TryGetValue("default", out var fallback))
        {
            return fallback;
        }

        return settings.Mouse.DefaultCaptureMode;
    }

    private void ShowWheel(Point point)
    {
        var settings = _settingsService.Current;
        var shape = WheelLayout.NormalizeShape(settings.Wheel.Shape);
        var sectorCount = WheelLayout.NormalizeSectorCount(shape, settings.Wheel.SectorCount);
        var visibleHistorySlots = settings.Wheel.QuickCopy && sectorCount > 1
            ? sectorCount - 1
            : sectorCount;
        var items = _historyService.SnapshotForWheel(visibleHistorySlots);

        var effectiveItems = items;
        if (settings.Wheel.QuickCopy && sectorCount > 1)
        {
            // Quick Copy always owns the very last sector (clockwise =
            // top-left for both shapes).  Real clipboard entries fill
            // slots 0 .. sectorCount-2; empty slots before the copy
            // button render as muted (null).
            var list = new List<ClipboardEntry>(sectorCount);
            for (var i = 0; i < sectorCount - 1; i++)
            {
                list.Add(i < items.Count
                    ? items[i]
                    : null!);
            }
            list.Add(new ClipboardEntry { DisplayText = "复制", IsQuickCopy = true });
            effectiveItems = list;
        }

        _wheelActive = true;
        _wheelActivatedUtc = DateTime.UtcNow;
        Interlocked.Exchange(ref _confirmSelectionScheduled, 0);
        _lockToggledDuringWheel = false;
        var captured = effectiveItems;
        var app = Application.Current;
        if (app is null)
        {
            _wheelActive = false;
            _wheelActivatedUtc = DateTime.MinValue;
            return;
        }

        try
        {
            app.Dispatcher.BeginInvoke(() =>
            {
                if (!_wheelActive)
                {
                    return;
                }

                _wheelOverlay.ShowWheel(point, captured);
            }, DispatcherPriority.Send);
        }
        catch (InvalidOperationException)
        {
            _wheelActive = false;
            _wheelActivatedUtc = DateTime.MinValue;
        }
    }

    private bool IsWheelFailSafeExpired()
    {
        return _wheelActivatedUtc != DateTime.MinValue
            && (DateTime.UtcNow - _wheelActivatedUtc).TotalMilliseconds > WheelFailSafeMs;
    }

    private void ScheduleReleaseVerification()
    {
        var generation = Interlocked.Increment(ref _releaseVerifyGeneration);
        var app = Application.Current;
        if (app is null)
        {
            return;
        }

        try
        {
            app.Dispatcher.BeginInvoke(async () =>
            {
                await Task.Delay(MiddleReleaseDebounceMs);
                if (generation != _releaseVerifyGeneration || !_wheelActive)
                {
                    return;
                }

                if (NativeMethods.IsKeyDown(NativeMethods.VK_MBUTTON))
                {
                    PasteTrace.Mark("WM_MBUTTONUP_debounce_ignored button_still_down");
                    return;
                }

                PasteTrace.Mark($"WM_MBUTTONUP_debounce_confirm pending_updates={_pointerUpdateScheduled}");
                ScheduleWheelClose(executeSelection: true, "debounce_confirm");
            }, DispatcherPriority.Background);
        }
        catch (InvalidOperationException)
        {
            Interlocked.Increment(ref _releaseVerifyGeneration);
        }
    }

    private void ScheduleWheelClose(bool executeSelection, string reason)
    {
        if (Interlocked.CompareExchange(ref _confirmSelectionScheduled, 1, 0) != 0)
        {
            PasteTrace.Mark($"WheelClose_skip_already_scheduled reason={reason}");
            return;
        }

        var app = Application.Current;
        if (app is null)
        {
            ResetWheelInputState();
            return;
        }

        try
        {
            app.Dispatcher.BeginInvoke(async () =>
            {
                try
                {
                    WheelSelection? selection = null;

                    PasteTrace.Mark($"WheelClose_enter execute={executeSelection} reason={reason}");
                    if (executeSelection)
                    {
                        selection = _wheelOverlay.ConfirmSelection();
                    }

                    _wheelOverlay.HideWheel();
                    PasteTrace.Mark("HideWheel_complete");

                    var lockToggledDuringWheel = _lockToggledDuringWheel;
                    ResetWheelInputState();
                    PasteTrace.Mark("Wheel_input_state_reset");

                    if (!executeSelection)
                    {
                        return;
                    }

                    if (lockToggledDuringWheel)
                    {
                        PasteTrace.Mark("ConfirmWheelSelection_skip_after_lock_toggle");
                        return;
                    }

                    await _wheelActionGate.WaitAsync();
                    try
                    {
                        var selected = selection?.Entry;
                        var extendedAction = selection?.ExtendedAction;

                        if (extendedAction is not null)
                        {
                            await ExecuteExtendedActionAsync(extendedAction);
                        }
                        else if (selected is not null && selected.IsQuickCopy)
                        {
                            // Quick Copy: send Ctrl+C to copy whatever is selected
                            // in the foreground app, then consume the event.
                            await Task.Delay(20);
                            var sent = NativeMethods.SendCtrlC();
                            PasteTrace.Mark($"QuickCopy_sent_CtrlC ok={sent}");
                        }
                        else if (selected is not null)
                        {
                            var mode = _pasteService.ResolveMode(selected);
                            PasteTrace.Mark($"PasteAsync_start sector_mode={mode}");
                            await _pasteService.PasteAsync(selected, mode);
                            PasteTrace.Mark("PasteAsync_returned");
                        }
                        else
                        {
                            PasteTrace.Mark("ConfirmWheelSelection_no_selection (deadzone release)");
                        }
                    }
                    finally
                    {
                        _wheelActionGate.Release();
                    }
                }
                catch (Exception ex)
                {
                    ResetWheelInputState();
                    PasteTrace.Mark($"WheelClose_exception {ex.GetType().Name}: {ex.Message}");
                }
                finally
                {
                    Interlocked.Exchange(ref _confirmSelectionScheduled, 0);
                }
            }, DispatcherPriority.Send);
        }
        catch (InvalidOperationException)
        {
            ResetWheelInputState();
        }
    }

    private void ResetWheelInputState()
    {
        _wheelActive = false;
        _wheelActivatedUtc = DateTime.MinValue;
        _middleDownUtc = DateTime.MinValue;
        _lockToggledDuringWheel = false;
        _latestPointer = default;
        _pauseUntilUtc = DateTime.UtcNow.AddMilliseconds(WheelResetCooldownMs);
        Interlocked.Increment(ref _releaseVerifyGeneration);
        Interlocked.Exchange(ref _pointerUpdateScheduled, 0);
        Interlocked.Exchange(ref _confirmSelectionScheduled, 0);
    }

    private static async Task ExecuteExtendedActionAsync(ExtendedWheelActionSlot action)
    {
        await Task.Delay(20);
        if (!action.IsConfigured)
        {
            PasteTrace.Mark($"ExtendedAction_empty slot={action.SlotIndex}");
            return;
        }

        if (string.Equals(action.Mode, ExtendedWheelActionMode.Hotkey, StringComparison.OrdinalIgnoreCase))
        {
            var keys = ParseHotkey(action.Hotkey);
            if (keys.Count == 0)
            {
                PasteTrace.Mark($"ExtendedAction_hotkey_invalid slot={action.SlotIndex}");
                return;
            }

            var sent = NativeMethods.SendHotkey(keys);
            PasteTrace.Mark($"ExtendedAction_hotkey_sent slot={action.SlotIndex} hotkey={action.Hotkey} ok={sent}");
            return;
        }

        if (string.Equals(action.Mode, ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase))
        {
            await ExecuteShortcutActionAsync(action);
        }
    }

    private static async Task ExecuteShortcutActionAsync(ExtendedWheelActionSlot action)
    {
        if (string.IsNullOrWhiteSpace(action.ShortcutPath))
        {
            PasteTrace.Mark($"ExtendedAction_shortcut_empty slot={action.SlotIndex}");
            return;
        }

        try
        {
            var shortcutPath = action.ShortcutPath;
            var launchInfo = TryResolveShortcutLaunchInfo(shortcutPath);
            var trackingKey = BuildShortcutTrackingKey(shortcutPath, launchInfo, action.BrowserLaunchUrl);
            var folderTarget = launchInfo?.TargetPath;
            if (!string.IsNullOrWhiteSpace(folderTarget) && System.IO.Directory.Exists(folderTarget))
            {
                if (TryHandleExistingFolderWindow(folderTarget, action.SecondTriggerBehavior))
                {
                    ClearLaunchAttempt(trackingKey);
                    PasteTrace.Mark($"ExtendedAction_shortcut_folder_second_trigger slot={action.SlotIndex} behavior={action.SecondTriggerBehavior}");
                    return;
                }

                // Explorer is normally a long-lived shell process, not a
                // per-folder process. Folder bindings must never be retained in
                // the generic process/session tracker after their window closes.
                RemoveLaunchSessions(trackingKey);
                if (!TryReserveLaunchAttempt(trackingKey, out var folderAttempt))
                {
                    PasteTrace.Mark($"ExtendedAction_folder_launch_throttled slot={action.SlotIndex}");
                    return;
                }

                Process.Start(new ProcessStartInfo
                {
                    FileName = folderTarget,
                    UseShellExecute = true
                });
                PasteTrace.Mark($"ExtendedAction_folder_started slot={action.SlotIndex} attempt={folderAttempt}");
                return;
            }

            if (TryHandleExistingFolderWindow(launchInfo?.TargetPath, action.SecondTriggerBehavior))
            {
                PasteTrace.Mark($"ExtendedAction_shortcut_folder_second_trigger slot={action.SlotIndex} behavior={action.SecondTriggerBehavior}");
                return;
            }

            if (TryHandleExistingLaunchSession(trackingKey, action.SecondTriggerBehavior))
            {
                PasteTrace.Mark($"ExtendedAction_shortcut_second_trigger slot={action.SlotIndex} behavior={action.SecondTriggerBehavior}");
                return;
            }

            if (!TryReserveLaunchAttempt(trackingKey, out var launchAttempt))
            {
                PasteTrace.Mark($"ExtendedAction_shortcut_launch_throttled slot={action.SlotIndex}");
                return;
            }

            var windowExecutablePath = ResolveWindowExecutablePath(launchInfo);
            var windowsBeforeLaunch = string.IsNullOrWhiteSpace(windowExecutablePath)
                ? new HashSet<IntPtr>()
                : GetVisibleWindowsForExecutable(windowExecutablePath);
            var process = StartShortcutProcess(
                shortcutPath,
                launchInfo,
                windowExecutablePath,
                action.BrowserLaunchUrl,
                out var launchKind);
            var documentPath = launchInfo is not null && !IsExecutableTarget(launchInfo.TargetPath)
                ? launchInfo.TargetPath
                : null;
            var tracked = await TrackLaunchSessionAsync(
                trackingKey,
                process,
                windowExecutablePath,
                windowsBeforeLaunch,
                documentPath);
            PasteTrace.Mark(
                $"ExtendedAction_shortcut_started slot={action.SlotIndex} kind={launchKind} " +
                $"pid={tracked.ProcessId} hwnd=0x{tracked.WindowHandle.ToInt64():X} attempt={launchAttempt}");
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"ExtendedAction_shortcut_exception slot={action.SlotIndex} {ex.GetType().Name}: {ex.Message}");
        }
    }

    private static bool TryHandleExistingFolderWindow(string? shortcutTarget, string behavior)
    {
        if (string.IsNullOrWhiteSpace(shortcutTarget) || !System.IO.Directory.Exists(shortcutTarget))
        {
            return false;
        }

        if (!TryFindShellFolderWindow(shortcutTarget, out var handle))
        {
            return false;
        }

        ApplySecondTriggerToWindow(handle, behavior);
        return true;
    }

    private static bool TryHandleExistingLaunchSession(string trackingKey, string behavior)
    {
        List<TrackedLaunchSession> candidates;
        lock (LaunchSessionLock)
        {
            if (!LaunchSessions.TryGetValue(trackingKey, out var trackedProcesses))
            {
                return false;
            }

            PruneExpiredLaunchSessions(trackedProcesses);
            if (trackedProcesses.Count == 0)
            {
                LaunchSessions.Remove(trackingKey);
                return false;
            }

            // The most recently launched live instance is the one this shortcut
            // should toggle. Manual instances are never added to this list.
            candidates = trackedProcesses
                .OrderByDescending(candidate => candidate.StartedUtc)
                .ToList();
        }

        foreach (var candidate in candidates)
        {
            var windowHandle = GetTrackedWindowHandle(candidate);
            if (windowHandle != IntPtr.Zero)
            {
                ClearLaunchAttempt(trackingKey);
                if (!NativeMethods.IsWindowVisible(windowHandle) || NativeMethods.IsIconic(windowHandle))
                {
                    var activated = BringWindowToFront(windowHandle);
                    PasteTrace.Mark(
                        $"ExtendedAction_shortcut_hidden_window_reactivated pid={candidate.ProcessId} " +
                        $"hwnd=0x{windowHandle.ToInt64():X} foreground={activated}");
                    return true;
                }

                ApplySecondTriggerToWindow(windowHandle, behavior);
                PasteTrace.Mark(
                    $"ExtendedAction_shortcut_second_trigger_target pid={candidate.ProcessId} " +
                    $"hwnd=0x{windowHandle.ToInt64():X} source=observed_window");
                return true;
            }

            var wpsHostWindow = GetTrackedWpsHostWindow(candidate);
            if (wpsHostWindow != IntPtr.Zero)
            {
                ClearLaunchAttempt(trackingKey);
                // WPS can host several documents in one top-level window. A
                // wheel-launched file must at least bring that host back even
                // when the version does not expose its individual tabs to UIA.
                var foreground = BringWindowToFront(wpsHostWindow, preserveWindowState: true, forceForeground: true);
                PasteTrace.Mark(
                    $"ExtendedAction_wps_host_activated hwnd=0x{wpsHostWindow.ToInt64():X} " +
                    $"foreground={foreground}");
                TryActivateWpsDocumentTab(candidate, wpsHostWindow);
                return true;
            }

            if (candidate.IsUnmanagedLeaseActive)
            {
                // The launch helper may have exited after handing the request to
                // a tabbed/single-instance host. We deliberately do not control
                // that pre-existing host, but also must not launch it again.
                PasteTrace.Mark(
                    $"ExtendedAction_launch_session_unmanaged_host_active pid={candidate.ProcessId} " +
                    $"until={candidate.UnmanagedUntilUtc:O}");
                return true;
            }
        }

        PasteTrace.Mark("ExtendedAction_shortcut_tracked_process_has_no_window");
        return false;
    }

    private static IntPtr GetTrackedWindowHandle(TrackedLaunchSession candidate)
    {
        if (candidate.WindowHandle != IntPtr.Zero && NativeMethods.IsWindow(candidate.WindowHandle))
        {
            return candidate.WindowHandle;
        }

        // A WPS shell handoff can expose secondary splash/tool windows after
        // the document was already routed to a shared host. Never let the
        // generic "new HWND" heuristic turn one of those into a controllable
        // document window; this session is activation-only by design.
        if (candidate.IsSharedWpsDocumentLaunch)
        {
            return IntPtr.Zero;
        }

        if (string.IsNullOrWhiteSpace(candidate.WindowExecutablePath))
        {
            return IntPtr.Zero;
        }

        var replacementWindows = GetVisibleWindowsForExecutable(candidate.WindowExecutablePath)
            .Where(windowHandle => !candidate.WindowsBeforeLaunch.Contains(windowHandle))
            .ToList();
        if (replacementWindows.Count == 0)
        {
            return IntPtr.Zero;
        }

        var foreground = NativeMethods.GetForegroundWindow();
        var replacement = replacementWindows.Contains(foreground)
            ? foreground
            : replacementWindows[0];
        candidate.RebindWindow(replacement);
        PasteTrace.Mark(
            $"ExtendedAction_shortcut_window_rebound pid={candidate.ProcessId} " +
            $"hwnd=0x{replacement.ToInt64():X}");
        return replacement;
    }

    private static void ApplySecondTriggerToWindow(IntPtr handle, string behavior, Process? process = null)
    {
        if (string.Equals(behavior, ExtendedWheelSecondTriggerBehavior.Close, StringComparison.OrdinalIgnoreCase))
        {
            if (process is null || !process.CloseMainWindow())
            {
                NativeMethods.PostMessage(handle, NativeMethods.WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            }
        }
        else
        {
            if (NativeMethods.IsIconic(handle))
            {
                NativeMethods.ShowWindow(handle, NativeMethods.SW_RESTORE);
                NativeMethods.SetForegroundWindow(handle);
            }
            else
            {
                NativeMethods.ShowWindow(handle, NativeMethods.SW_MINIMIZE);
            }
        }
    }

    private static Process? StartShortcutProcess(
        string shortcutPath,
        ShortcutLaunchInfo? launchInfo,
        string? windowExecutablePath,
        string browserLaunchUrl,
        out string launchKind)
    {
        if (launchInfo is not null && System.IO.File.Exists(launchInfo.TargetPath))
        {
            if (!IsExecutableTarget(launchInfo.TargetPath))
            {
                var documentProfile = GetCompatibilityProfile(windowExecutablePath);
                if (documentProfile?.DocumentArgumentsPrefix is not null)
                {
                    launchKind = "document_isolated_" + System.IO.Path.GetFileNameWithoutExtension(windowExecutablePath!);
                    return Process.Start(new ProcessStartInfo
                    {
                        FileName = windowExecutablePath!,
                        Arguments = AppendArgument(
                            documentProfile.DocumentArgumentsPrefix,
                            QuoteCommandLineArgument(launchInfo.TargetPath)),
                        WorkingDirectory = System.IO.Path.GetDirectoryName(windowExecutablePath) ?? string.Empty,
                        UseShellExecute = false
                    });
                }

                launchKind = "document_shell_association";
                return Process.Start(new ProcessStartInfo
                {
                    FileName = launchInfo.TargetPath,
                    WorkingDirectory = string.IsNullOrWhiteSpace(launchInfo.WorkingDirectory)
                        ? System.IO.Path.GetDirectoryName(launchInfo.TargetPath) ?? string.Empty
                        : launchInfo.WorkingDirectory,
                    UseShellExecute = true
                });
            }

            var arguments = BuildLaunchArguments(launchInfo, browserLaunchUrl, out var browserUrlApplied);
            var profile = GetCompatibilityProfile(launchInfo.TargetPath);
            var isolatedProgram = profile?.ProgramArguments is not null
                && !arguments.Contains(profile.ProgramArguments, StringComparison.OrdinalIgnoreCase);
            if (isolatedProgram)
            {
                arguments = AppendArgument(profile!.ProgramArguments!, arguments);
            }

            launchKind = browserUrlApplied
                ? "browser_target_url"
                : isolatedProgram
                    ? "program_isolated_" + System.IO.Path.GetFileNameWithoutExtension(launchInfo.TargetPath)
                    : "target_executable";
            return Process.Start(new ProcessStartInfo
            {
                FileName = launchInfo.TargetPath,
                Arguments = arguments,
                WorkingDirectory = string.IsNullOrWhiteSpace(launchInfo.WorkingDirectory)
                    ? System.IO.Path.GetDirectoryName(launchInfo.TargetPath) ?? string.Empty
                    : launchInfo.WorkingDirectory,
                UseShellExecute = false
            });
        }

        launchKind = "shell_shortcut";
        return Process.Start(new ProcessStartInfo
        {
            FileName = shortcutPath,
            UseShellExecute = true
        });
    }

    private static IntPtr GetTrackedWpsHostWindow(TrackedLaunchSession candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate.DocumentPath)
            || !IsWpsExecutable(candidate.WindowExecutablePath))
        {
            return IntPtr.Zero;
        }

        if (candidate.WpsHostWindowHandle != IntPtr.Zero
            && NativeMethods.IsWindow(candidate.WpsHostWindowHandle))
        {
            return candidate.WpsHostWindowHandle;
        }

        // If a shell handoff made WPS foreground during the first launch, it is
        // the strongest available association between this wheel session and a
        // shared WPS host. Do not pick among several unrelated WPS windows.
        var foreground = NativeMethods.GetForegroundWindow();
        if (foreground != IntPtr.Zero
            && IsWindowOwnedByExecutable(foreground, candidate.WindowExecutablePath!))
        {
            candidate.RebindWpsHostWindow(foreground);
            return foreground;
        }

        var hosts = GetVisibleWindowsForExecutable(candidate.WindowExecutablePath!);
        if (hosts.Count == 1)
        {
            var onlyHost = hosts.First();
            candidate.RebindWpsHostWindow(onlyHost);
            return onlyHost;
        }

        return IntPtr.Zero;
    }

    private static bool TryActivateWpsDocumentTab(TrackedLaunchSession candidate, IntPtr preferredHostWindow)
    {
        if (string.IsNullOrWhiteSpace(candidate.DocumentPath)
            || !IsWpsExecutable(candidate.WindowExecutablePath)
            || candidate.WpsTabAutomationUnavailable
            || preferredHostWindow == IntPtr.Zero)
        {
            return false;
        }

        var fileName = System.IO.Path.GetFileName(candidate.DocumentPath);
        var fileStem = System.IO.Path.GetFileNameWithoutExtension(candidate.DocumentPath);
        if (string.IsNullOrWhiteSpace(fileName) || string.IsNullOrWhiteSpace(fileStem))
        {
            return false;
        }

        try
        {
            // Restrict UIA to the host captured for this wheel session. Looking
            // through every WPS window is slow and risks selecting a same-named
            // document belonging to an unrelated user window.
            var root = AutomationElement.FromHandle(preferredHostWindow);
            var tabItems = root.FindAll(
                TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TabItem));
            AutomationElement? matchingTab = null;
            var bestMatch = int.MaxValue;

            foreach (AutomationElement tabItem in tabItems)
            {
                var match = GetWpsTabMatchRank(tabItem.Current.Name, fileName, fileStem);
                if (match < bestMatch)
                {
                    matchingTab = tabItem;
                    bestMatch = match;
                }
            }

            if (matchingTab is null)
            {
                candidate.DisableWpsTabAutomation();
                PasteTrace.Mark($"ExtendedAction_wps_tab_unavailable items={tabItems.Count}");
                return false;
            }

            if (matchingTab.GetCurrentPattern(SelectionItemPattern.Pattern) is not SelectionItemPattern selection)
            {
                candidate.DisableWpsTabAutomation();
                PasteTrace.Mark($"ExtendedAction_wps_tab_no_selection_pattern hwnd=0x{preferredHostWindow.ToInt64():X}");
                return false;
            }

            selection.Select();
            var activated = BringWindowToFront(preferredHostWindow, preserveWindowState: true, forceForeground: true);
            PasteTrace.Mark(
                $"ExtendedAction_wps_tab_activated hwnd=0x{preferredHostWindow.ToInt64():X} " +
                $"match={bestMatch} foreground={activated}");
            return true;
        }
        catch (ElementNotAvailableException)
        {
            // WPS can rebuild the ribbon/tab tree during startup. Another
            // trigger can safely retry, without changing its launch state.
        }
        catch (Exception ex)
        {
            candidate.DisableWpsTabAutomation();
            PasteTrace.Mark($"ExtendedAction_wps_tab_activation_exception {ex.GetType().Name}: {ex.Message}");
        }

        return false;
    }

    private static int GetWpsTabMatchRank(string? tabName, string fileName, string fileStem)
    {
        var normalized = (tabName ?? string.Empty).Trim().TrimEnd('*').Trim();
        if (string.Equals(normalized, fileName, StringComparison.OrdinalIgnoreCase))
        {
            return 0;
        }

        if (string.Equals(normalized, fileStem, StringComparison.OrdinalIgnoreCase))
        {
            return 1;
        }

        return int.MaxValue;
    }

    private static async Task<TrackedLaunchSession> TrackLaunchSessionAsync(
        string trackingKey,
        Process? process,
        string? windowExecutablePath,
        IReadOnlySet<IntPtr> windowsBeforeLaunch,
        string? documentPath)
    {
        var processId = TryGetProcessId(process);
        var windowHandle = string.IsNullOrWhiteSpace(windowExecutablePath)
            ? IntPtr.Zero
            : await WaitForNewWindowAsync(windowExecutablePath, windowsBeforeLaunch);
        var wpsHostWindowHandle = windowHandle == IntPtr.Zero
            && !string.IsNullOrWhiteSpace(documentPath)
            && windowExecutablePath is { Length: > 0 } executablePath
            && IsWpsExecutable(executablePath)
            ? TryGetForegroundWpsHostWindow(executablePath)
            : IntPtr.Zero;

        if (windowHandle != IntPtr.Zero)
        {
            var activated = BringWindowToFront(windowHandle);
            if (!activated)
            {
                await Task.Delay(120);
                activated = BringWindowToFront(windowHandle);
            }

            PasteTrace.Mark(
                $"ExtendedAction_shortcut_initial_window_activation hwnd=0x{windowHandle.ToInt64():X} ok={activated}");
        }

        var tracked = new TrackedLaunchSession(
            process,
            processId,
            windowHandle,
            windowExecutablePath,
            windowsBeforeLaunch,
            windowHandle == IntPtr.Zero
                ? DateTime.UtcNow.AddSeconds(
                    IsWpsExecutable(windowExecutablePath)
                        ? WpsUnmanagedLaunchLeaseSeconds
                        : UnmanagedLaunchLeaseSeconds)
                : null,
            documentPath,
            wpsHostWindowHandle,
            windowHandle == IntPtr.Zero
                && !string.IsNullOrWhiteSpace(documentPath)
                && IsWpsExecutable(windowExecutablePath));
        if (tracked.IsUnmanagedLeaseActive)
        {
            PasteTrace.Mark(
                $"ExtendedAction_launch_session_unmanaged_host_lease pid={processId} " +
                $"seconds={(IsWpsExecutable(windowExecutablePath) ? WpsUnmanagedLaunchLeaseSeconds : UnmanagedLaunchLeaseSeconds)}");
        }
        lock (LaunchSessionLock)
        {
            if (!LaunchSessions.TryGetValue(trackingKey, out var trackedProcesses))
            {
                trackedProcesses = new List<TrackedLaunchSession>();
                LaunchSessions[trackingKey] = trackedProcesses;
            }

            PruneExpiredLaunchSessions(trackedProcesses);
            trackedProcesses.Add(tracked);
        }

        return tracked;
    }

    private static bool TryReserveLaunchAttempt(string trackingKey, out int attemptCount)
    {
        lock (LaunchSessionLock)
        {
            var now = DateTime.UtcNow;
            if (LaunchAttempts.TryGetValue(trackingKey, out var state)
                && (now - state.LastAttemptUtc).TotalMilliseconds < ForceLaunchCooldownMs)
            {
                attemptCount = state.AttemptCount;
                return false;
            }

            state ??= new LaunchAttemptState();
            state.LastAttemptUtc = now;
            state.AttemptCount++;
            LaunchAttempts[trackingKey] = state;
            attemptCount = state.AttemptCount;
            return true;
        }
    }

    private static void ClearLaunchAttempt(string trackingKey)
    {
        lock (LaunchSessionLock)
        {
            LaunchAttempts.Remove(trackingKey);
        }
    }

    private static void RemoveLaunchSessions(string trackingKey)
    {
        lock (LaunchSessionLock)
        {
            if (!LaunchSessions.Remove(trackingKey, out var sessions))
            {
                return;
            }

            foreach (var session in sessions)
            {
                session.Process?.Dispose();
            }
        }
    }

    private static IntPtr TryGetForegroundWpsHostWindow(string windowExecutablePath)
    {
        var foreground = NativeMethods.GetForegroundWindow();
        return foreground != IntPtr.Zero && IsWindowOwnedByExecutable(foreground, windowExecutablePath)
            ? foreground
            : IntPtr.Zero;
    }

    private static string BuildShortcutTrackingKey(
        string shortcutPath,
        ShortcutLaunchInfo? launchInfo,
        string browserLaunchUrl)
    {
        var normalizedPath = shortcutPath.Trim();
        if (launchInfo is null
            || !IsBrowserExecutable(launchInfo.TargetPath)
            || !TryNormalizeBrowserLaunchUrl(browserLaunchUrl, out var normalizedUrl))
        {
            return normalizedPath;
        }

        // Different browser URLs are separate bindings even when they share one
        // executable shortcut. The delimiter cannot appear in a Windows path or URL.
        return normalizedPath + "\u001F" + normalizedUrl;
    }

    private static string? ResolveWindowExecutablePath(ShortcutLaunchInfo? launchInfo)
    {
        if (launchInfo is null || !System.IO.File.Exists(launchInfo.TargetPath))
        {
            return null;
        }

        if (IsExecutableTarget(launchInfo.TargetPath))
        {
            return launchInfo.TargetPath;
        }

        return TryFindAssociatedExecutable(launchInfo.TargetPath);
    }

    private static string? TryFindAssociatedExecutable(string documentPath)
    {
        try
        {
            var result = new System.Text.StringBuilder(32768);
            var status = NativeMethods.FindExecutable(
                documentPath,
                System.IO.Path.GetDirectoryName(documentPath),
                result,
                result.Capacity);
            if (status <= 32)
            {
                return null;
            }

            var executablePath = result.ToString();
            return System.IO.File.Exists(executablePath) ? executablePath : null;
        }
        catch
        {
            return null;
        }
    }

    private static bool IsExecutableTarget(string targetPath)
    {
        var extension = System.IO.Path.GetExtension(targetPath);
        return extension.Equals(".exe", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".com", StringComparison.OrdinalIgnoreCase);
    }

    private static ApplicationCompatibilityProfile? GetCompatibilityProfile(string? executablePath)
    {
        if (string.IsNullOrWhiteSpace(executablePath))
        {
            return null;
        }

        return CompatibilityProfiles.TryGetValue(System.IO.Path.GetFileName(executablePath), out var profile)
            ? profile
            : null;
    }

    private static int TryGetProcessId(Process? process)
    {
        if (process is null)
        {
            return 0;
        }

        try
        {
            return process.Id;
        }
        catch
        {
            return 0;
        }
    }

    private static async Task<IntPtr> WaitForNewWindowAsync(string targetPath, IReadOnlySet<IntPtr> windowsBeforeLaunch)
    {
        var attempts = GetCompatibilityProfile(targetPath)?.WindowWaitAttempts ?? 20;
        for (var attempt = 0; attempt < attempts; attempt++)
        {
            await Task.Delay(100);
            var currentWindows = GetVisibleWindowsForExecutable(targetPath);
            var newWindows = currentWindows
                .Where(windowHandle => !windowsBeforeLaunch.Contains(windowHandle))
                .ToList();
            if (newWindows.Count == 0)
            {
                continue;
            }

            var foreground = NativeMethods.GetForegroundWindow();
            if (newWindows.Contains(foreground))
            {
                return foreground;
            }

            return newWindows[0];
        }

        return IntPtr.Zero;
    }

    private static bool IsWindowOwnedByExecutable(IntPtr windowHandle, string targetPath)
    {
        try
        {
            NativeMethods.GetWindowThreadProcessId(windowHandle, out var processId);
            if (processId == 0)
            {
                return false;
            }

            using var process = Process.GetProcessById((int)processId);
            var executablePath = process.MainModule?.FileName;
            return !string.IsNullOrWhiteSpace(executablePath)
                && string.Equals(executablePath, targetPath, StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    private static HashSet<IntPtr> GetVisibleWindowsForExecutable(string targetPath)
    {
        var windows = new HashSet<IntPtr>();
        NativeMethods.EnumWindows((windowHandle, _) =>
        {
            if (NativeMethods.IsWindowVisible(windowHandle)
                && IsWindowOwnedByExecutable(windowHandle, targetPath)
                && IsTrackableWindowForExecutable(windowHandle, targetPath))
            {
                windows.Add(windowHandle);
            }

            return true;
        }, IntPtr.Zero);
        return windows;
    }

    private static bool IsTrackableWindowForExecutable(IntPtr windowHandle, string targetPath)
    {
        var requiredClass = GetCompatibilityProfile(targetPath)?.MainWindowClass;
        if (string.IsNullOrWhiteSpace(requiredClass))
        {
            return true;
        }

        var className = GetWindowClassName(windowHandle);
        return string.Equals(className, requiredClass, StringComparison.OrdinalIgnoreCase);
    }

    private static string GetWindowClassName(IntPtr windowHandle)
    {
        try
        {
            var buffer = new System.Text.StringBuilder(256);
            return NativeMethods.GetClassName(windowHandle, buffer, buffer.Capacity) > 0
                ? buffer.ToString()
                : string.Empty;
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string BuildLaunchArguments(ShortcutLaunchInfo launchInfo, string browserLaunchUrl, out bool browserUrlApplied)
    {
        var arguments = launchInfo.Arguments;
        browserUrlApplied = false;
        if (!IsBrowserExecutable(launchInfo.TargetPath))
        {
            return arguments;
        }

        if (IsFirefoxExecutable(launchInfo.TargetPath))
        {
            if (!arguments.Contains("-new-window", StringComparison.OrdinalIgnoreCase))
            {
                arguments = AppendArgument(arguments, "-new-window");
            }
        }
        else if (!arguments.Contains("--new-window", StringComparison.OrdinalIgnoreCase))
        {
            arguments = AppendArgument(arguments, "--new-window");
        }

        if (!TryNormalizeBrowserLaunchUrl(browserLaunchUrl, out var normalizedUrl))
        {
            return arguments;
        }

        browserUrlApplied = true;
        return AppendArgument(arguments, QuoteCommandLineArgument(normalizedUrl));
    }

    private static bool TryNormalizeBrowserLaunchUrl(string value, out string normalizedUrl)
    {
        normalizedUrl = string.Empty;
        var candidate = value?.Trim() ?? string.Empty;
        if (candidate.Length == 0 || candidate.Any(char.IsWhiteSpace))
        {
            return false;
        }

        if (!candidate.Contains("://", StringComparison.Ordinal))
        {
            candidate = "https://" + candidate;
        }

        if (!Uri.TryCreate(candidate, UriKind.Absolute, out var uri)
            || (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
        {
            return false;
        }

        normalizedUrl = uri.AbsoluteUri;
        return true;
    }

    private static string AppendArgument(string arguments, string argument)
    {
        return string.IsNullOrWhiteSpace(arguments) ? argument : arguments + " " + argument;
    }

    private static string QuoteCommandLineArgument(string argument)
    {
        return '"' + argument.Replace("\"", "\\\"") + '"';
    }

    private static bool IsBrowserExecutable(string targetPath)
    {
        var executableName = System.IO.Path.GetFileName(targetPath);
        return executableName.Equals("msedge.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("chrome.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("brave.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("opera.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("vivaldi.exe", StringComparison.OrdinalIgnoreCase)
            || IsFirefoxExecutable(targetPath)
            || executableName.Equals("waterfox.exe", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsWpsExecutable(string? executablePath)
    {
        if (string.IsNullOrWhiteSpace(executablePath))
        {
            return false;
        }

        var executableName = System.IO.Path.GetFileName(executablePath);
        return executableName.Equals("wps.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("et.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("wpp.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("wpspdf.exe", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsFirefoxExecutable(string targetPath)
    {
        var executableName = System.IO.Path.GetFileName(targetPath);
        return executableName.Equals("firefox.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("waterfox.exe", StringComparison.OrdinalIgnoreCase);
    }

    private static bool BringWindowToFront(
        IntPtr windowHandle,
        bool preserveWindowState = false,
        bool forceForeground = false)
    {
        if (windowHandle == IntPtr.Zero || !NativeMethods.IsWindow(windowHandle))
        {
            return false;
        }

        // Shared WPS hosts can be maximized. Restoring an already visible host
        // has caused some WPS versions to recreate a smaller frame, so only
        // restore when it is actually minimized in that activation-only path.
        if (!preserveWindowState || NativeMethods.IsIconic(windowHandle))
        {
            NativeMethods.ShowWindow(windowHandle, NativeMethods.SW_RESTORE);
        }
        NativeMethods.SetWindowPos(
            windowHandle,
            NativeMethods.HWND_TOP,
            0,
            0,
            0,
            0,
            NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE | NativeMethods.SWP_SHOWWINDOW);
        NativeMethods.SetForegroundWindow(windowHandle);
        if (NativeMethods.GetForegroundWindow() == windowHandle)
        {
            return true;
        }

        if (forceForeground)
        {
            // Windows can deny SetForegroundWindow when this app is not the
            // foreground-input owner. Temporarily joining that input queue is
            // the narrowest fallback; it is used only for a recorded WPS host.
            var currentThread = NativeMethods.GetCurrentThreadId();
            var foregroundWindow = NativeMethods.GetForegroundWindow();
            var foregroundThread = foregroundWindow == IntPtr.Zero
                ? 0u
                : NativeMethods.GetWindowThreadProcessId(foregroundWindow, out _);
            var targetThread = NativeMethods.GetWindowThreadProcessId(windowHandle, out _);
            var attachedForeground = foregroundThread != 0 && foregroundThread != currentThread
                && NativeMethods.AttachThreadInput(currentThread, foregroundThread, true);
            var attachedTarget = targetThread != 0 && targetThread != currentThread
                && NativeMethods.AttachThreadInput(currentThread, targetThread, true);
            try
            {
                NativeMethods.BringWindowToTop(windowHandle);
                NativeMethods.SetForegroundWindow(windowHandle);
                NativeMethods.SetFocus(windowHandle);
                if (NativeMethods.GetForegroundWindow() == windowHandle)
                {
                    return true;
                }
            }
            finally
            {
                if (attachedTarget)
                {
                    NativeMethods.AttachThreadInput(currentThread, targetThread, false);
                }

                if (attachedForeground)
                {
                    NativeMethods.AttachThreadInput(currentThread, foregroundThread, false);
                }
            }
        }

        // Some applications complete their startup after creating the HWND. A
        // transient topmost pulse improves the first-launch case without changing
        // the application's persistent always-on-top setting.
        NativeMethods.SetWindowPos(
            windowHandle,
            NativeMethods.HWND_TOPMOST,
            0,
            0,
            0,
            0,
            NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE | NativeMethods.SWP_SHOWWINDOW);
        NativeMethods.SetWindowPos(
            windowHandle,
            NativeMethods.HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            NativeMethods.SWP_NOMOVE | NativeMethods.SWP_NOSIZE | NativeMethods.SWP_SHOWWINDOW);
        NativeMethods.SetForegroundWindow(windowHandle);
        return NativeMethods.GetForegroundWindow() == windowHandle;
    }

    private static void PruneExpiredLaunchSessions(List<TrackedLaunchSession> trackedProcesses)
    {
        for (var index = trackedProcesses.Count - 1; index >= 0; index--)
        {
            var candidate = trackedProcesses[index];
            if (candidate.WindowHandle != IntPtr.Zero && NativeMethods.IsWindow(candidate.WindowHandle))
            {
                continue;
            }

            if (candidate.IsSharedWpsDocumentLaunch
                && candidate.WpsHostWindowHandle != IntPtr.Zero
                && NativeMethods.IsWindow(candidate.WpsHostWindowHandle))
            {
                continue;
            }

            // Preserve an unresolved shell handoff for its short lease even if
            // its launcher process has already exited. A later trigger can still
            // rebind it if a distinct window finally appears.
            if (candidate.IsUnmanagedLeaseActive)
            {
                continue;
            }

            // A process by itself is not evidence that this binding still has a
            // usable target. Explorer, Everything and many single-instance apps
            // can stay resident after their window has closed or moved to tray.
            // Once the bounded handoff lease expires, permit a fresh launch.
            candidate.Process?.Dispose();
            trackedProcesses.RemoveAt(index);
        }
    }

    private static void ClearLaunchSessions()
    {
        lock (LaunchSessionLock)
        {
            foreach (var trackedProcesses in LaunchSessions.Values)
            {
                foreach (var candidate in trackedProcesses)
                {
                    candidate.Process?.Dispose();
                }
            }

            LaunchSessions.Clear();
            LaunchAttempts.Clear();
        }
    }

    private static ShortcutLaunchInfo? TryResolveShortcutLaunchInfo(string shortcutPath)
    {
        try
        {
            dynamic shortcut = CreateWScriptShortcut(shortcutPath);
            string? targetPath = shortcut.TargetPath;
            if (string.IsNullOrWhiteSpace(targetPath))
            {
                return null;
            }

            string? arguments = shortcut.Arguments;
            string? workingDirectory = shortcut.WorkingDirectory;
            return new ShortcutLaunchInfo
            {
                TargetPath = targetPath,
                Arguments = arguments ?? string.Empty,
                WorkingDirectory = workingDirectory ?? string.Empty
            };
        }
        catch
        {
            return null;
        }
    }

    private static dynamic CreateWScriptShortcut(string shortcutPath)
    {
        var shellType = Type.GetTypeFromProgID("WScript.Shell");
        if (shellType is null)
        {
            throw new InvalidOperationException("WScript.Shell is unavailable.");
        }

        dynamic shell = Activator.CreateInstance(shellType)!;
        return shell.CreateShortcut(shortcutPath);
    }

    private static bool TryFindShellFolderWindow(string folderPath, out IntPtr handle)
    {
        handle = IntPtr.Zero;
        try
        {
            var target = NormalizeFolderPath(folderPath);
            if (target is null)
            {
                return false;
            }

            var shellType = Type.GetTypeFromProgID("Shell.Application");
            if (shellType is null)
            {
                return false;
            }

            dynamic shell = Activator.CreateInstance(shellType)!;
            foreach (dynamic window in shell.Windows())
            {
                string? locationUrl;
                try
                {
                    locationUrl = window.LocationURL;
                }
                catch
                {
                    continue;
                }

                var candidate = FolderPathFromShellLocationUrl(locationUrl);
                if (candidate is null || !string.Equals(candidate, target, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                handle = new IntPtr(Convert.ToInt64(window.HWND, CultureInfo.InvariantCulture));
                return handle != IntPtr.Zero;
            }
        }
        catch
        {
            // Shell windows are a convenience path for folder shortcuts only.
        }

        return false;
    }

    private static string? FolderPathFromShellLocationUrl(string? locationUrl)
    {
        if (string.IsNullOrWhiteSpace(locationUrl))
        {
            return null;
        }

        try
        {
            return NormalizeFolderPath(new Uri(locationUrl).LocalPath);
        }
        catch
        {
            return null;
        }
    }

    private static string? NormalizeFolderPath(string folderPath)
    {
        try
        {
            return System.IO.Path.GetFullPath(folderPath).TrimEnd(
                System.IO.Path.DirectorySeparatorChar,
                System.IO.Path.AltDirectorySeparatorChar);
        }
        catch
        {
            return null;
        }
    }

    private static List<int> ParseHotkey(string hotkey)
    {
        var result = new List<int>();
        foreach (var rawPart in hotkey.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (TryParseVirtualKey(rawPart, out var key))
            {
                result.Add(key);
            }
        }

        return result;
    }

    private static bool TryParseVirtualKey(string keyText, out int key)
    {
        key = 0;
        var normalized = keyText.Trim().ToUpperInvariant();
        switch (normalized)
        {
            case "CTRL":
            case "CONTROL":
                key = NativeMethods.VK_CONTROL;
                return true;
            case "SHIFT":
                key = NativeMethods.VK_SHIFT;
                return true;
            case "ALT":
                key = NativeMethods.VK_MENU;
                return true;
            case "WIN":
            case "WINDOWS":
                key = NativeMethods.VK_LWIN;
                return true;
            case "ENTER":
            case "RETURN":
                key = 0x0D;
                return true;
            case "ESC":
            case "ESCAPE":
                key = 0x1B;
                return true;
            case "TAB":
                key = 0x09;
                return true;
            case "SPACE":
                key = 0x20;
                return true;
            case "BACK":
            case "BACKSPACE":
                key = 0x08;
                return true;
            case "DELETE":
            case "DEL":
                key = 0x2E;
                return true;
            case "INSERT":
            case "INS":
                key = 0x2D;
                return true;
            case "HOME":
                key = 0x24;
                return true;
            case "END":
                key = 0x23;
                return true;
            case "PAGEUP":
            case "PRIOR":
                key = 0x21;
                return true;
            case "PAGEDOWN":
            case "NEXT":
                key = 0x22;
                return true;
            case "UP":
                key = 0x26;
                return true;
            case "DOWN":
                key = 0x28;
                return true;
            case "LEFT":
                key = 0x25;
                return true;
            case "RIGHT":
                key = 0x27;
                return true;
            case "=":
                key = 0xBB;
                return true;
            case "-":
                key = 0xBD;
                return true;
            case ",":
                key = 0xBC;
                return true;
            case ".":
                key = 0xBE;
                return true;
            case "/":
                key = 0xBF;
                return true;
            case ";":
                key = 0xBA;
                return true;
            case "'":
                key = 0xDE;
                return true;
            case "[":
                key = 0xDB;
                return true;
            case "]":
                key = 0xDD;
                return true;
            case "\\":
                key = 0xDC;
                return true;
            case "`":
                key = 0xC0;
                return true;
        }

        if (normalized.Length == 1)
        {
            var ch = normalized[0];
            if (ch is >= 'A' and <= 'Z' or >= '0' and <= '9')
            {
                key = ch;
                return true;
            }
        }

        if (normalized.StartsWith('F') &&
            int.TryParse(normalized[1..], NumberStyles.None, CultureInfo.InvariantCulture, out var fKey) &&
            fKey is >= 1 and <= 24)
        {
            key = 0x70 + fKey - 1;
            return true;
        }

        return false;
    }

    private void ToggleWheelSelectionLock()
    {
        var app = Application.Current;
        if (app is null)
        {
            return;
        }

        app.Dispatcher.Invoke(() =>
        {
            var selected = _wheelOverlay.GetSelectedEntry();
            if (selected is null || selected.IsQuickCopy)
            {
                PasteTrace.Mark(selected is null ? "WheelLock_ignored_no_selection" : "WheelLock_ignored_quick_copy");
                return;
            }

            if (_historyService.ToggleLock(selected))
            {
                _lockToggledDuringWheel = true;
                _wheelOverlay.RefreshSelectionVisuals();
                PasteTrace.Mark($"WheelLock_toggled is_locked={selected.IsLocked}");
            }
        }, DispatcherPriority.Send);
    }

    // Runs on the UI thread (via BeginInvoke). Reads the freshest pointer the
    // hook thread has stored since the last call and updates the highlight.
    // Coalescing guarantees we get one fresh position per UI frame instead of
    // one per WM_MOUSEMOVE.
    private void DispatchPendingPointerUpdate()
    {
        Interlocked.Exchange(ref _pointerUpdateScheduled, 0);
        if (_wheelActive)
        {
            _wheelOverlay.UpdatePointer(_latestPointer);
        }
    }

    private static IntPtr Suppress() => new(1);

    public void Dispose()
    {
        _failSafeTimer?.Stop();
        ClearLaunchSessions();
        if (_hookId != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_hookId);
            _hookId = IntPtr.Zero;
        }
    }
}
