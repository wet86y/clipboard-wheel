using System.IO;
using System.Text.Json;
using ClipboardWheel.Models;

namespace ClipboardWheel.Services;

public sealed class SettingsService
{
    private const string AppDataDirectoryName = "超级中键";
    private const string LegacyAppDataDirectoryName = "ClipboardWheel";

    private readonly string _directory;
    private readonly string _path;
    private readonly JsonSerializerOptions _jsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true
    };

    private AppSettings? _cached;

    public event EventHandler<AppSettings>? SettingsChanged;

    public SettingsService()
    {
        _directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), AppDataDirectoryName);
        _path = Path.Combine(_directory, "settings.json");
    }

    public AppSettings Current => _cached ?? LoadOrCreate();

    public string SettingsPath => _path;

    public AppSettings LoadOrCreate()
    {
        Directory.CreateDirectory(_directory);
        TryMigrateLegacySettings();

        if (!File.Exists(_path))
        {
            _cached = new AppSettings();
            Save(_cached, raiseEvent: false);
            return _cached;
        }

        try
        {
            var json = File.ReadAllText(_path);
            _cached = JsonSerializer.Deserialize<AppSettings>(json, _jsonOptions) ?? new AppSettings();
            Normalize(_cached);
            // Most starts should be read-only. Previously every launch rewrote
            // settings.json even when normalization made no change, causing
            // avoidable disk activity on the startup path.
            var normalizedJson = JsonSerializer.Serialize(_cached, _jsonOptions);
            if (!string.Equals(json, normalizedJson, StringComparison.Ordinal))
            {
                WriteSettingsJson(normalizedJson);
            }
            return _cached;
        }
        catch
        {
            var backup = Path.Combine(_directory, $"settings.broken.{DateTime.Now:yyyyMMddHHmmss}.json");
            try
            {
                File.Copy(_path, backup, overwrite: true);
            }
            catch
            {
                // A broken settings file should not prevent startup when the
                // file is locked, deleted concurrently, or inaccessible.
            }

            _cached = new AppSettings();
            Save(_cached, raiseEvent: false);
            return _cached;
        }
    }

    private void TryMigrateLegacySettings()
    {
        if (File.Exists(_path))
        {
            return;
        }

        var legacyPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            LegacyAppDataDirectoryName,
            "settings.json");
        if (!File.Exists(legacyPath))
        {
            return;
        }

        try
        {
            File.Copy(legacyPath, _path, overwrite: false);
        }
        catch
        {
            // Migration is best-effort; failing to copy should fall back to defaults.
        }
    }

    public void Save(AppSettings settings, bool raiseEvent = true)
    {
        Directory.CreateDirectory(_directory);
        Normalize(settings);
        var json = JsonSerializer.Serialize(settings, _jsonOptions);

        WriteSettingsJson(json);

        _cached = settings;
        if (raiseEvent)
        {
            SettingsChanged?.Invoke(this, settings);
        }
    }

    private void WriteSettingsJson(string json)
    {
        var tempPath = _path + $".tmp.{Environment.ProcessId}.{Guid.NewGuid():N}";
        try
        {
            File.WriteAllText(tempPath, json);
            File.Move(tempPath, _path, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(tempPath))
                {
                    File.Delete(tempPath);
                }
            }
            catch
            {
                // The primary save already completed; a leftover temp file is
                // harmless and can be cleaned up on the next save.
            }
        }
    }

    private static void Normalize(AppSettings settings)
    {
        var loadedVersion = settings.SettingsVersion;
        settings.Update ??= new UpdateSettings();

        if (settings.ProcessRules is null)
        {
            settings.ProcessRules = AppSettings.DefaultProcessRules();
        }
        else if (settings.ProcessRules.Comparer != StringComparer.OrdinalIgnoreCase)
        {
            settings.ProcessRules = new Dictionary<string, string>(
                settings.ProcessRules,
                StringComparer.OrdinalIgnoreCase);
        }

        settings.Paste.DefaultMode = "smart";
        settings.Paste.RestoreClipboardAfterPaste = false;
        settings.Paste.RestoreDelayMs = 150;
        settings.Paste.AddPasteToClipboardHistory = false;
        settings.Clipboard.LoadWindowsClipboardHistoryOnStartup = true;
        settings.Clipboard.CapturePlainText = true;
        settings.Clipboard.CaptureHtml = true;
        settings.Clipboard.CaptureRtf = true;
        settings.Clipboard.CaptureCsv = true;
        settings.Clipboard.IgnorePasswordLikeText = false;
        settings.Clipboard.MaxHistoryItems = 8;
        settings.Mouse.DefaultCaptureMode = "always";
        settings.ProcessRules.Clear();
        settings.ProcessRules["default"] = settings.Mouse.DefaultCaptureMode;

        settings.Wheel.SectorCount = Math.Clamp(settings.Wheel.SectorCount, 2, 16);
        settings.Wheel.Radius = Math.Clamp(settings.Wheel.Radius, 80, 360);
        settings.Wheel.InnerDeadZoneRadius = Math.Clamp(settings.Wheel.InnerDeadZoneRadius, 0, settings.Wheel.Radius - 20);
        settings.Wheel.Opacity = Math.Clamp(settings.Wheel.Opacity, 0.2, 1.0);
        settings.Wheel.MaxPreviewChars = Math.Clamp(settings.Wheel.MaxPreviewChars, 8, 120);
        settings.Wheel.ExtendedWheel ??= new ExtendedWheelSettings();
        settings.Wheel.ExtendedWheel.BreakoutBufferPixels = Math.Clamp(
            settings.Wheel.ExtendedWheel.BreakoutBufferPixels,
            0,
            80);
        NormalizeExtendedWheelSlots(settings.Wheel.ExtendedWheel);

        if (loadedVersion < 3)
        {
            settings.Wheel.QuickCopy = true;
        }

        settings.SettingsVersion = AppSettings.CurrentSettingsVersion;
    }

    private static void NormalizeExtendedWheelSlots(ExtendedWheelSettings extended)
    {
        var slotsByIndex = (extended.Slots ?? ExtendedWheelSettings.CreateDefaultSlots())
            .Where(slot => slot is not null)
            .GroupBy(slot => Math.Clamp(slot.SlotIndex, 0, ExtendedWheelSettings.SlotCount - 1))
            .ToDictionary(group => group.Key, group => group.First());

        var normalized = new List<ExtendedWheelActionSlot>(ExtendedWheelSettings.SlotCount);
        for (var i = 0; i < ExtendedWheelSettings.SlotCount; i++)
        {
            if (!slotsByIndex.TryGetValue(i, out var slot))
            {
                slot = new ExtendedWheelActionSlot();
            }

            slot.SlotIndex = i;
            slot.Mode = NormalizeExtendedActionMode(slot.Mode);
            slot.SecondTriggerBehavior = NormalizeSecondTriggerBehavior(slot.SecondTriggerBehavior);
            slot.Name = slot.Name?.Trim() ?? string.Empty;
            slot.Hotkey = slot.Hotkey?.Trim() ?? string.Empty;
            slot.ShortcutPath = slot.ShortcutPath?.Trim() ?? string.Empty;
            slot.BrowserLaunchUrl = slot.BrowserLaunchUrl?.Trim() ?? string.Empty;
            normalized.Add(slot);
        }

        extended.Slots = normalized;
    }

    private static string NormalizeExtendedActionMode(string? mode)
    {
        return string.Equals(mode, ExtendedWheelActionMode.Hotkey, StringComparison.OrdinalIgnoreCase)
            ? ExtendedWheelActionMode.Hotkey
            : string.Equals(mode, ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase)
                ? ExtendedWheelActionMode.Shortcut
                : ExtendedWheelActionMode.None;
    }

    private static string NormalizeSecondTriggerBehavior(string? behavior)
    {
        return string.Equals(behavior, ExtendedWheelSecondTriggerBehavior.Close, StringComparison.OrdinalIgnoreCase)
            ? ExtendedWheelSecondTriggerBehavior.Close
            : ExtendedWheelSecondTriggerBehavior.Minimize;
    }
}
