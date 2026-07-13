using System.Diagnostics;
using ClipboardWheel.Native;

namespace ClipboardWheel.Services;

public static class ForegroundWindowService
{
    public static string? GetForegroundProcessName()
    {
        try
        {
            var hWnd = NativeMethods.GetForegroundWindow();
            if (hWnd == IntPtr.Zero)
            {
                return null;
            }

            NativeMethods.GetWindowThreadProcessId(hWnd, out var pid);
            if (pid == 0)
            {
                return null;
            }

            var process = Process.GetProcessById((int)pid);
            return process.ProcessName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
                ? process.ProcessName
                : process.ProcessName + ".exe";
        }
        catch
        {
            return null;
        }
    }
}
