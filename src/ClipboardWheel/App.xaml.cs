using System.IO;
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
