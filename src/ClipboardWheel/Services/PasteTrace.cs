using System.Diagnostics;
using System.IO;
using System.Text;

namespace ClipboardWheel.Services;

/// <summary>
/// Lightweight file logger for the paste critical path.
///
/// Controlled by the PASTE_TRACE_ENABLED compilation symbol (set via
/// PasteTraceEnabled=true in the csproj).  When /p:PasteTraceEnabled=false
/// is passed during dotnet publish, the C# compiler strips all
/// Init / Mark / Dispose call sites — the published exe never touches
/// paste-trace.log.  Regular dotnet build / dotnet run keep the logger.
/// Designed so logging itself never crashes the host process.
/// </summary>
internal static class PasteTrace
{
    private static readonly object _lock = new();
    private static readonly Stopwatch _sw = Stopwatch.StartNew();
    private static StreamWriter? _writer;
    private static string? _logPath;
    private static bool _disposed;

    public static string? LogPath => _logPath;

    [Conditional("PASTE_TRACE_ENABLED")]
    public static void Init()
    {
        try
        {
            var dir = GetLogDirectory();
            Directory.CreateDirectory(dir);
            _logPath = Path.Combine(dir, "paste-trace.log");

            // Append across sessions; a "session start" line separates runs.
            var stream = new FileStream(
                _logPath,
                FileMode.Append,
                FileAccess.Write,
                FileShare.ReadWrite);
            _writer = new StreamWriter(stream, Encoding.UTF8)
            {
                AutoFlush = false
            };

            WriteHeader();
        }
        catch
        {
            // Logging must never crash the app. If init fails, Mark becomes a no-op.
            _writer = null;
            _logPath = null;
        }
    }

    [Conditional("PASTE_TRACE_ENABLED")]
    public static void Mark(string eventName)
    {
        if (_disposed) return;
        StreamWriter? writer;
        lock (_lock)
        {
            writer = _writer;
        }
        if (writer is null) return;

        try
        {
            var elapsedMs = _sw.Elapsed.TotalMilliseconds;
            var wallClock = DateTime.Now.ToString("HH:mm:ss.fff");
            lock (_lock)
            {
                writer.WriteLine($"[{elapsedMs,10:F2}ms | {wallClock}] {eventName}");
                writer.Flush();
            }
        }
        catch
        {
            // Swallow - logging must never crash the app.
        }
    }

    [Conditional("PASTE_TRACE_ENABLED")]
    public static void Dispose()
    {
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            try
            {
                _writer?.WriteLine($"=== session end at {DateTime.Now:O} ===");
                _writer?.Flush();
                _writer?.Dispose();
            }
            catch { }
            _writer = null;
        }
    }

    private static void WriteHeader()
    {
        var wallClock = DateTime.Now.ToString("HH:mm:ss.fff");
        lock (_lock)
        {
            _writer?.WriteLine($"=== session start pid={Environment.ProcessId} at {wallClock} (offsets relative to session start) ===");
            _writer?.Flush();
        }
    }

    private static string GetLogDirectory()
    {
#if PASTE_TRACE_NEXT_TO_EXECUTABLE
        return Path.GetDirectoryName(Environment.ProcessPath)
            ?? AppContext.BaseDirectory;
#else
        return Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "超级中键");
#endif
    }
}
