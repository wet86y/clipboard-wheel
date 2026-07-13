using System.Windows;
using ClipboardWheel.UI;
using Forms = System.Windows.Forms;
using Drawing = System.Drawing;
using Drawing2D = System.Drawing.Drawing2D;

namespace ClipboardWheel.Services;

public sealed class TrayService : IDisposable
{
    private readonly SettingsService _settingsService;
    private readonly ClipboardHistoryService _historyService;
    private readonly MouseHookService _mouseHookService;
    private Forms.NotifyIcon? _notifyIcon;
    private SettingsWindow? _settingsWindow;
    private int _trayClickGeneration;

    // Pre-built icons for the two capture states.
    private static readonly Drawing.Icon IconEnabled = BuildWheelIcon(Drawing.Color.FromArgb(0x3F, 0x6A, 0xFF), Drawing.Color.White);
    private static readonly Drawing.Icon IconDisabled = BuildWheelIcon(Drawing.Color.Gray, Drawing.Color.LightGray);

    public TrayService(SettingsService settingsService, ClipboardHistoryService historyService, MouseHookService mouseHookService)
    {
        _settingsService = settingsService;
        _historyService = historyService;
        _mouseHookService = mouseHookService;
    }

    public void Start()
    {
        _notifyIcon = new Forms.NotifyIcon
        {
            Icon = _mouseHookService.IsCaptureEnabled ? IconEnabled : IconDisabled,
            Visible = true,
            Text = "超级中键"
        };
        RebuildMenu();

        // Delay the left-click toggle briefly so accidental double-clicks only
        // toggle the global capture switch once. Settings are opened from the
        // right-click menu only.
        _notifyIcon.MouseClick += HandleTrayMouseClick;
    }

    private void ToggleCapture()
    {
        var enabled = !_mouseHookService.IsCaptureEnabled;
        _mouseHookService.IsCaptureEnabled = enabled;
        UpdateIcon();
        // Re-sync the checkmark in the context menu.
        RebuildMenu();
    }

    private void HandleTrayMouseClick(object? sender, Forms.MouseEventArgs args)
    {
        if (args.Button != Forms.MouseButtons.Left)
        {
            return;
        }

        ScheduleSingleClickToggle();
    }

    private void ScheduleSingleClickToggle()
    {
        var generation = Interlocked.Increment(ref _trayClickGeneration);
        Application.Current.Dispatcher.BeginInvoke(async () =>
        {
            await Task.Delay(260);
            if (generation != _trayClickGeneration)
            {
                return;
            }

            ToggleCapture();
        });
    }

    public void UpdateIcon()
    {
        if (_notifyIcon is null) return;
        _notifyIcon.Icon = _mouseHookService.IsCaptureEnabled ? IconEnabled : IconDisabled;
    }

    private void RebuildMenu()
    {
        if (_notifyIcon is null) return;

        var menu = new Forms.ContextMenuStrip();

        var enabled = new Forms.ToolStripMenuItem("启用中键轮盘")
        {
            Checked = _mouseHookService.IsCaptureEnabled,
            CheckOnClick = true
        };
        enabled.CheckedChanged += (_, _) =>
        {
            _mouseHookService.IsCaptureEnabled = enabled.Checked;
            UpdateIcon();
        };
        menu.Items.Add(enabled);

        var settings = new Forms.ToolStripMenuItem("设置");
        settings.Click += (_, _) => ShowSettings();
        menu.Items.Add(settings);

        var clear = new Forms.ToolStripMenuItem("清空历史");
        clear.Click += (_, _) => _historyService.Clear();
        menu.Items.Add(clear);

        menu.Items.Add(new Forms.ToolStripSeparator());

        var exit = new Forms.ToolStripMenuItem("退出");
        exit.Click += (_, _) => Application.Current.Shutdown();
        menu.Items.Add(exit);

        var previousMenu = _notifyIcon.ContextMenuStrip;
        _notifyIcon.ContextMenuStrip = menu;
        previousMenu?.Dispose();
    }

    private void ShowSettings()
    {
        Application.Current.Dispatcher.Invoke(() =>
        {
            if (_settingsWindow is { IsVisible: true })
            {
                if (_settingsWindow.WindowState == WindowState.Minimized)
                {
                    _settingsWindow.WindowState = WindowState.Normal;
                }

                _settingsWindow.Activate();
                _settingsWindow.Focus();
                return;
            }

            _settingsWindow = new SettingsWindow(_settingsService);
            _settingsWindow.Closed += (_, _) => _settingsWindow = null;
            _settingsWindow.Show();
            _settingsWindow.Activate();
        });
    }

    public void Dispose()
    {
        if (_notifyIcon is not null)
        {
            _notifyIcon.MouseClick -= HandleTrayMouseClick;
            _notifyIcon.Visible = false;
            var menu = _notifyIcon.ContextMenuStrip;
            _notifyIcon.ContextMenuStrip = null;
            menu?.Dispose();
            _notifyIcon.Dispose();
            _notifyIcon = null;
        }
    }

    /// <summary>
    /// Draw a simple wheel icon: outer circle + centre dot, at 32x32.
    /// Used for both the enabled (blue) and disabled (gray) tray states.
    /// </summary>
    private static Drawing.Icon BuildWheelIcon(Drawing.Color fill, Drawing.Color dot)
    {
        var bmp = new Drawing.Bitmap(32, 32);
        using var g = Drawing.Graphics.FromImage(bmp);
        g.SmoothingMode = Drawing2D.SmoothingMode.AntiAlias;
        g.Clear(Drawing.Color.Transparent);

        // Outer ring (filled circle)
        using var outerBrush = new Drawing.SolidBrush(fill);
        g.FillEllipse(outerBrush, 2, 2, 28, 28);

        // Centre dot
        using var dotBrush = new Drawing.SolidBrush(dot);
        g.FillEllipse(dotBrush, 12, 12, 8, 8);

        return Drawing.Icon.FromHandle(bmp.GetHicon());
    }
}
