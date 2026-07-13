using System.IO;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Threading;
using DesktopUpdateKit;
using ClipboardWheel.Services;
using ClipboardWheel.UI;

namespace ClipboardWheel;

public partial class App : Application
{
    private const string SingleInstanceMutexName = @"Local\SuperMiddleKey.SingleInstance";
    private const string AdministratorRestartArgument = "--admin-restart";
    private const string ShortcutDropHelperArgument = "--shortcut-drop-helper";
    private const string VerifyReleaseArgument = "--verify-release";
    private const string UpdaterStubResourceName = "DesktopUpdateKit.Resources.UpdaterStub.exe";

    private Mutex? _singleInstanceMutex;
    private SettingsService? _settingsService;
    private ClipboardHistoryService? _historyService;
    private ClipboardMonitorService? _clipboardMonitorService;
    private WindowsClipboardHistoryService? _windowsHistoryService;
    private MouseHookService? _mouseHookService;
    private PasteService? _pasteService;
    private TrayService? _trayService;
    private WheelOverlay? _wheelOverlay;
    private readonly UpdateLauncher _executableNameLauncher = new();

    protected override void OnStartup(StartupEventArgs e)
    {
        if (HasArgument(e.Args, VerifyReleaseArgument))
        {
            base.OnStartup(e);
            Shutdown(VerifyReleaseBundle());
            return;
        }

        if (TryGetArgumentValue(e.Args, ShortcutDropHelperArgument, out var handoffId)
            && Guid.TryParseExact(handoffId, "N", out _))
        {
            base.OnStartup(e);
            PasteTrace.Init("shortcut-drop-helper.log");
            PasteTrace.Mark("ShortcutDrop_helper_process_started");
            var helperWindow = new ShortcutDropHelperWindow(handoffId);
            MainWindow = helperWindow;
            helperWindow.Show();
            return;
        }

        _singleInstanceMutex = new Mutex(initiallyOwned: true, SingleInstanceMutexName, out var isFirstInstance);
        if (!isFirstInstance)
        {
            if (!HasArgument(e.Args, AdministratorRestartArgument))
            {
                _singleInstanceMutex.Dispose();
                _singleInstanceMutex = null;
                Shutdown(0);
                return;
            }

            var takeoverMutex = new Mutex(initiallyOwned: false, SingleInstanceMutexName);
            _singleInstanceMutex.Dispose();
            _singleInstanceMutex = null;
            try
            {
                if (takeoverMutex.WaitOne(TimeSpan.FromSeconds(15)))
                {
                    _singleInstanceMutex = takeoverMutex;
                }
                else
                {
                    takeoverMutex.Dispose();
                    Shutdown(0);
                    return;
                }
            }
            catch (AbandonedMutexException)
            {
                _singleInstanceMutex = takeoverMutex;
            }
        }

        base.OnStartup(e);

        if (TryNormalizeExecutableName())
        {
            Shutdown(0);
            return;
        }

        PasteTrace.Init();

        _settingsService = new SettingsService();
        var settings = _settingsService.LoadOrCreate();

        if (settings.RunAsAdministratorEnabled && !ElevationService.IsAdministrator())
        {
            if (ElevationService.TryRestart(runAsAdministrator: true, out _))
            {
                Shutdown(0);
                return;
            }

            // Do not trap the user in an elevation prompt loop after a denied
            // UAC request or a machine without a usable elevation broker.
            settings.RunAsAdministratorEnabled = false;
            _settingsService.Save(settings);
        }

        _historyService = new ClipboardHistoryService(_settingsService);
        _pasteService = new PasteService(_settingsService, _historyService);
        _wheelOverlay = new WheelOverlay(_settingsService);
        _mouseHookService = new MouseHookService(_settingsService, _historyService, _pasteService, _wheelOverlay);
        _clipboardMonitorService = new ClipboardMonitorService(_historyService);
        _windowsHistoryService = new WindowsClipboardHistoryService(_historyService, _settingsService);
        _trayService = new TrayService(_settingsService, _historyService, _mouseHookService);

        _clipboardMonitorService.Start();
        _mouseHookService.Start();
        _trayService.Start();
        TryWriteUpdateHealthMarker(e.Args);

        if (settings.IsFirstRun)
        {
            var window = new SettingsWindow(_settingsService);
            window.Show();
            settings.IsFirstRun = false;
            _settingsService.Save(settings);
        }

        Dispatcher.BeginInvoke(async () =>
        {
            await _windowsHistoryService.TryImportAsync();
        }, DispatcherPriority.ApplicationIdle);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        SettingsWindow.StopUpdateDownloadForApplicationExit();
        _mouseHookService?.Dispose();
        _clipboardMonitorService?.Dispose();
        _trayService?.Dispose();
        _historyService?.Dispose();
        _wheelOverlay?.ClearForShutdown();
        PasteTrace.Dispose();
        _singleInstanceMutex?.ReleaseMutex();
        _singleInstanceMutex?.Dispose();
        _singleInstanceMutex = null;
        base.OnExit(e);
    }

    private bool TryNormalizeExecutableName()
    {
        try
        {
            return _executableNameLauncher
                .EnsureCanonicalExecutableNameAsync("超级中键.exe")
                .GetAwaiter()
                .GetResult();
        }
        catch
        {
            // A read-only or protected install location must not prevent the
            // application from starting under its current name.
            return false;
        }
    }

    private static bool HasArgument(IReadOnlyList<string> args, string expected)
    {
        return args.Any(argument => string.Equals(argument, expected, StringComparison.OrdinalIgnoreCase));
    }

    private static int VerifyReleaseBundle()
    {
        try
        {
            var assembly = Assembly.GetEntryAssembly();
            using var resource = assembly?.GetManifestResourceStream(UpdaterStubResourceName);
            if (resource is null)
            {
                return 10;
            }

            using var buffer = new MemoryStream();
            resource.CopyTo(buffer);
            if (buffer.Length < 64)
            {
                return 11;
            }

            buffer.Position = 0;
            using var reader = new BinaryReader(buffer, System.Text.Encoding.UTF8, leaveOpen: true);
            if (reader.ReadUInt16() != 0x5A4D)
            {
                return 12;
            }

            buffer.Position = 0x3C;
            var peOffset = reader.ReadInt32();
            if (peOffset < 64 || peOffset > buffer.Length - 24)
            {
                return 13;
            }

            buffer.Position = peOffset;
            if (reader.ReadUInt32() != 0x00004550)
            {
                return 14;
            }

            buffer.Position = peOffset + 6;
            var sectionCount = reader.ReadUInt16();
            buffer.Position = peOffset + 20;
            var optionalHeaderSize = reader.ReadUInt16();
            if (sectionCount is 0 or > 96 || optionalHeaderSize == 0)
            {
                return 15;
            }

            var sectionTableOffset = peOffset + 24L + optionalHeaderSize;
            if (sectionTableOffset + sectionCount * 40L > buffer.Length)
            {
                return 16;
            }

            for (var sectionIndex = 0; sectionIndex < sectionCount; sectionIndex++)
            {
                var sectionOffset = sectionTableOffset + sectionIndex * 40L;
                buffer.Position = sectionOffset + 16;
                var rawDataSize = reader.ReadUInt32();
                var rawDataOffset = reader.ReadUInt32();
                if (rawDataSize > 0
                    && (rawDataOffset == 0 || rawDataOffset + (ulong)rawDataSize > (ulong)buffer.Length))
                {
                    return 17;
                }
            }

            return 0;
        }
        catch
        {
            return 18;
        }
    }

    private static bool TryGetArgumentValue(
        IReadOnlyList<string> args,
        string expected,
        out string value)
    {
        for (var i = 0; i < args.Count - 1; i++)
        {
            if (string.Equals(args[i], expected, StringComparison.OrdinalIgnoreCase))
            {
                value = args[i + 1];
                return true;
            }
        }

        value = string.Empty;
        return false;
    }

    private static void TryWriteUpdateHealthMarker(IReadOnlyList<string> args)
    {
        for (var i = 0; i < args.Count - 1; i++)
        {
            if (!string.Equals(args[i], "--update-health", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var markerPath = args[i + 1];
            try
            {
                var directory = Path.GetDirectoryName(markerPath);
                if (string.IsNullOrWhiteSpace(directory))
                {
                    return;
                }

                Directory.CreateDirectory(directory);
                var tempPath = markerPath + $".tmp.{Environment.ProcessId}.{Guid.NewGuid():N}";
                File.WriteAllText(tempPath, ApplicationVersion.CurrentText);
                File.Move(tempPath, markerPath, overwrite: true);
            }
            catch
            {
                // A missing health marker causes UpdaterStub to roll back.
            }

            return;
        }
    }
}
