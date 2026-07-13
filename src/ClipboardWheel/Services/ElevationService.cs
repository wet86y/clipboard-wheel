using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

namespace ClipboardWheel.Services;

internal static class ElevationService
{
    private const uint TokenAssignPrimary = 0x0001;
    private const uint TokenDuplicate = 0x0002;
    private const uint TokenQuery = 0x0008;
    private const uint TokenAdjustDefault = 0x0080;
    private const uint TokenAdjustSessionId = 0x0100;
    private const uint CreateUnicodeEnvironment = 0x00000400;
    private const int SecurityImpersonation = 2;
    private const int TokenPrimary = 1;

    public static bool IsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    public static bool TryRestart(bool runAsAdministrator, out string? error)
    {
        error = null;
        if (Environment.ProcessPath is not { Length: > 0 } executablePath
            || !Path.IsPathFullyQualified(executablePath)
            || !File.Exists(executablePath))
        {
            error = "当前运行方式不支持权限模式切换。";
            return false;
        }

        try
        {
            return runAsAdministrator
                ? TryStartElevated(executablePath, out error)
                : TryStartUnelevated(executablePath, new[] { "--admin-restart" }, out error);
        }
        catch (Win32Exception exception)
        {
            error = exception.NativeErrorCode == 1223
                ? "已取消管理员权限请求。"
                : exception.Message;
            return false;
        }
        catch (Exception exception)
        {
            error = exception.Message;
            return false;
        }
    }

    public static bool TryStartUnelevatedProcess(
        IReadOnlyList<string> arguments,
        out string? error)
    {
        error = null;
        if (Environment.ProcessPath is not { Length: > 0 } executablePath
            || !Path.IsPathFullyQualified(executablePath)
            || !File.Exists(executablePath))
        {
            error = "当前运行方式不支持启动普通权限辅助进程。";
            return false;
        }

        try
        {
            return TryStartUnelevated(executablePath, arguments, out error);
        }
        catch (Exception exception)
        {
            error = exception.Message;
            return false;
        }
    }

    private static bool TryStartElevated(string executablePath, out string? error)
    {
        error = null;
        var startInfo = new ProcessStartInfo
        {
            FileName = executablePath,
            WorkingDirectory = Path.GetDirectoryName(executablePath) ?? string.Empty,
            UseShellExecute = true,
            Verb = "runas"
        };
        startInfo.ArgumentList.Add("--admin-restart");
        if (Process.Start(startInfo) is null)
        {
            error = "管理员模式进程启动失败。";
            return false;
        }

        return true;
    }

    private static bool TryStartUnelevated(
        string executablePath,
        IReadOnlyList<string> arguments,
        out string? error)
    {
        error = null;
        var sessionId = Process.GetCurrentProcess().SessionId;
        using var shell = Process.GetProcessesByName("explorer")
            .FirstOrDefault(process => process.SessionId == sessionId);
        if (shell is null)
        {
            error = "找不到当前用户的 Windows Shell，无法切回普通权限。";
            return false;
        }

        var desiredAccess = TokenQuery
            | TokenDuplicate
            | TokenAssignPrimary
            | TokenAdjustDefault
            | TokenAdjustSessionId;
        if (!OpenProcessToken(shell.Handle, desiredAccess, out var shellToken))
        {
            error = $"无法读取 Windows Shell 权限令牌：{Marshal.GetLastWin32Error()}。";
            return false;
        }

        try
        {
            if (!DuplicateTokenEx(
                    shellToken,
                    desiredAccess,
                    IntPtr.Zero,
                    SecurityImpersonation,
                    TokenPrimary,
                    out var primaryToken))
            {
                error = $"无法复制普通权限令牌：{Marshal.GetLastWin32Error()}。";
                return false;
            }

            try
            {
                var environment = IntPtr.Zero;
                var environmentCreated = CreateEnvironmentBlock(out environment, primaryToken, false);
                try
                {
                    var startupInfo = new StartupInfo
                    {
                        cb = Marshal.SizeOf<StartupInfo>()
                    };
                    var commandLine = new StringBuilder($"\"{executablePath.Replace("\"", "\\\"")}\"");
                    foreach (var argument in arguments)
                    {
                        commandLine.Append(" \"");
                        commandLine.Append(argument.Replace("\"", "\\\""));
                        commandLine.Append('"');
                    }
                    var workingDirectory = Path.GetDirectoryName(executablePath) ?? string.Empty;
                    var started = CreateProcessAsUser(
                        primaryToken,
                        executablePath,
                        commandLine,
                        IntPtr.Zero,
                        IntPtr.Zero,
                        false,
                        environmentCreated ? CreateUnicodeEnvironment : 0,
                        environmentCreated ? environment : IntPtr.Zero,
                        workingDirectory,
                        ref startupInfo,
                        out var processInformation);
                    if (!started)
                    {
                        error = $"无法启动普通权限进程：{Marshal.GetLastWin32Error()}。";
                        return false;
                    }

                    CloseHandle(processInformation.ProcessHandle);
                    CloseHandle(processInformation.ThreadHandle);
                    return true;
                }
                finally
                {
                    if (environmentCreated)
                    {
                        DestroyEnvironmentBlock(environment);
                    }
                }
            }
            finally
            {
                CloseHandle(primaryToken);
            }
        }
        finally
        {
            CloseHandle(shellToken);
        }
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenProcessToken(
        IntPtr processHandle,
        uint desiredAccess,
        out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DuplicateTokenEx(
        IntPtr existingToken,
        uint desiredAccess,
        IntPtr tokenAttributes,
        int impersonationLevel,
        int tokenType,
        out IntPtr primaryToken);

    [DllImport("userenv.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateEnvironmentBlock(
        out IntPtr environment,
        IntPtr token,
        [MarshalAs(UnmanagedType.Bool)] bool inherit);

    [DllImport("userenv.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyEnvironmentBlock(IntPtr environment);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessAsUser(
        IntPtr token,
        string applicationName,
        StringBuilder commandLine,
        IntPtr processAttributes,
        IntPtr threadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
        uint creationFlags,
        IntPtr environment,
        string currentDirectory,
        ref StartupInfo startupInfo,
        out ProcessInformation processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public int cb;
        public string? reserved;
        public string? desktop;
        public string? title;
        public int x;
        public int y;
        public int xSize;
        public int ySize;
        public int xCountChars;
        public int yCountChars;
        public int fillAttribute;
        public int flags;
        public short showWindow;
        public short reserved2;
        public IntPtr reserved2Pointer;
        public IntPtr standardInput;
        public IntPtr standardOutput;
        public IntPtr standardError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr ProcessHandle;
        public IntPtr ThreadHandle;
        public int ProcessId;
        public int ThreadId;
    }
}
