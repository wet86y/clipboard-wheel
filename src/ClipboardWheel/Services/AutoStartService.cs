using System.IO;
using Microsoft.Win32;

namespace ClipboardWheel.Services;

internal static class AutoStartService
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "SuperMiddleKey";

    public static bool TryApply(bool enabled, out string? error)
    {
        error = null;
        try
        {
            using var runKey = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true);
            if (runKey is null)
            {
                error = "无法打开当前用户的开机启动设置。";
                return false;
            }

            if (!enabled)
            {
                runKey.DeleteValue(ValueName, throwOnMissingValue: false);
                return true;
            }

            if (Environment.ProcessPath is not { Length: > 0 } executablePath
                || !string.Equals(Path.GetExtension(executablePath), ".exe", StringComparison.OrdinalIgnoreCase)
                || !File.Exists(executablePath))
            {
                error = "当前运行程序不是可用于开机启动的 exe。";
                return false;
            }

            runKey.SetValue(ValueName, Quote(executablePath), RegistryValueKind.String);
            return true;
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException or IOException)
        {
            error = ex.Message;
            return false;
        }
    }

    private static string Quote(string path) => '"' + path.Replace("\"", "\\\"") + '"';
}
