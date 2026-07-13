using System.Windows.Interop;
using System.Windows.Threading;
using ClipboardWheel.Native;

namespace ClipboardWheel.Services;

public sealed class ClipboardMonitorService : IDisposable
{
    private const int ClipboardUpdateCoalesceMs = 40;

    private readonly ClipboardHistoryService _historyService;
    private readonly DispatcherTimer _captureTimer;
    private HwndSource? _source;

    public ClipboardMonitorService(ClipboardHistoryService historyService)
    {
        _historyService = historyService;
        _captureTimer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(ClipboardUpdateCoalesceMs)
        };
        _captureTimer.Tick += (_, _) =>
        {
            _captureTimer.Stop();
            _historyService.CaptureCurrentClipboard();
        };
    }

    public void Start()
    {
        if (_source is not null)
        {
            return;
        }

        var parameters = new HwndSourceParameters("SuperMiddleKeyClipboardListener")
        {
            // HwndSource treats zero dimensions as a special, slower path.
            // The source is never shown, so a minimal 1x1 surface is enough.
            Width = 1,
            Height = 1,
            WindowStyle = 0
        };

        _source = new HwndSource(parameters);
        _source.AddHook(WndProc);
        if (!NativeMethods.AddClipboardFormatListener(_source.Handle))
        {
            PasteTrace.Mark($"ClipboardListener_add_failed win32={System.Runtime.InteropServices.Marshal.GetLastWin32Error()}");
        }
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == NativeMethods.WM_CLIPBOARDUPDATE)
        {
            _captureTimer.Stop();
            _captureTimer.Start();
            handled = true;
            return IntPtr.Zero;
        }

        return IntPtr.Zero;
    }

    public void Dispose()
    {
        _captureTimer.Stop();
        if (_source is not null)
        {
            NativeMethods.RemoveClipboardFormatListener(_source.Handle);
            _source.Dispose();
            _source = null;
        }
    }
}
