using System.IO;

namespace ClipboardWheel.UI;

internal static class ShortcutDropHandoff
{
    private const string DirectoryName = "shortcut-drop-handoff";

    public static string GetResultPath(string handoffId)
    {
        if (!Guid.TryParseExact(handoffId, "N", out _))
        {
            throw new ArgumentException("Invalid shortcut drop handoff id.", nameof(handoffId));
        }

        return Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "超级中键",
            DirectoryName,
            handoffId + ".result");
    }

    public static void WriteResult(string handoffId, string shortcutPath)
    {
        var resultPath = GetResultPath(handoffId);
        var directory = Path.GetDirectoryName(resultPath)
            ?? throw new InvalidOperationException("Shortcut drop handoff directory is unavailable.");
        Directory.CreateDirectory(directory);

        var tempPath = resultPath + $".tmp.{Environment.ProcessId}.{Guid.NewGuid():N}";
        try
        {
            File.WriteAllText(tempPath, shortcutPath);
            File.Move(tempPath, resultPath, overwrite: true);
        }
        finally
        {
            TryDelete(tempPath);
        }
    }

    public static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // A stale random handoff file is harmless and can be overwritten
            // only by a process that already knows its unguessable id.
        }
    }
}
