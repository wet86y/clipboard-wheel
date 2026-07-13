namespace ClipboardWheel.Models;

public sealed class AppSettings
{
    public const int CurrentSettingsVersion = 3;

    public int SettingsVersion { get; set; } = CurrentSettingsVersion;
    public bool IsFirstRun { get; set; } = true;
    public bool AutoStartEnabled { get; set; } = false;
    public bool RunAsAdministratorEnabled { get; set; } = false;
    public WheelSettings Wheel { get; set; } = new();
    public ThemeSettings Theme { get; set; } = new();
    public MouseSettings Mouse { get; set; } = new();
    public ClipboardSettings Clipboard { get; set; } = new();
    public PasteSettings Paste { get; set; } = new();
    public UpdateSettings Update { get; set; } = new();
    public Dictionary<string, string> ProcessRules { get; set; } = DefaultProcessRules();

    public static Dictionary<string, string> DefaultProcessRules() => new(StringComparer.OrdinalIgnoreCase)
    {
        ["default"] = "always",
        // Legacy per-app rules kept here for quick rollback if app-specific
        // capture behavior is needed again.
        // ["EXCEL.EXE"] = "always",
        // ["et.exe"] = "always",
        // ["wps.exe"] = "always",
        // ["WINWORD.EXE"] = "always",
        // ["chrome.exe"] = "always",
        // ["msedge.exe"] = "always",
        // ["AutoCAD.exe"] = "modifierOnly",
        // ["SolidWorks.exe"] = "modifierOnly",
        // ["FreeCAD.exe"] = "modifierOnly"
    };
}

public sealed class UpdateSettings
{
    public bool UseAccelerationNodes { get; set; } = true;
}

public sealed class WheelSettings
{
    // "circle" or "rectangle". Normalised by WheelLayout.NormalizeShape.
    public string Shape { get; set; } = WheelLayout.ShapeCircle;
    public int SectorCount { get; set; } = 6;
    public double Radius { get; set; } = 180;
    public double InnerDeadZoneRadius { get; set; } = 42;
    public double Opacity { get; set; } = 0.88;
    public bool ShowAtCursor { get; set; } = true;
    public bool AnimationEnabled { get; set; } = true;
    public int MaxPreviewChars { get; set; } = 32;
    // Angular gap between circle sectors (degrees on each side) so the
    // selected sector can expand into the gap without overlapping neighbours.
    public double SectorGapDegrees { get; set; } = 3.0;
    // Pixel gap between rectangle cells on each side; same purpose.
    public double SectorGapPixels { get; set; } = 4.0;
    // Scale factor applied to the selected sector (around its centroid) for
    // the "微微扩大" visual feedback. 1.0 disables, 1.05 is barely visible,
    // 1.10 is clearly visible.
    public double SelectedSectorScale { get; set; } = 1.08;
    // When true the last sector is reserved as a native Ctrl+C copy button.
    public bool QuickCopy { get; set; } = true;
    public ExtendedWheelSettings ExtendedWheel { get; set; } = new();
}

public sealed class ExtendedWheelSettings
{
    public const int SlotCount = 12;

    public bool Enabled { get; set; } = false;
    public double BreakoutBufferPixels { get; set; } = 18;
    public List<ExtendedWheelActionSlot> Slots { get; set; } = CreateDefaultSlots();

    public static List<ExtendedWheelActionSlot> CreateDefaultSlots()
    {
        var slots = new List<ExtendedWheelActionSlot>(SlotCount);
        for (var i = 0; i < SlotCount; i++)
        {
            slots.Add(new ExtendedWheelActionSlot { SlotIndex = i });
        }
        return slots;
    }
}

public sealed class ExtendedWheelActionSlot
{
    public int SlotIndex { get; set; }
    public bool Enabled { get; set; }
    public string Name { get; set; } = string.Empty;
    public string Mode { get; set; } = ExtendedWheelActionMode.None;
    public string Hotkey { get; set; } = string.Empty;
    public string ShortcutPath { get; set; } = string.Empty;
    // Optional http(s) address appended when the shortcut targets a browser.
    // No user-data-dir is supplied, so the browser keeps using its existing
    // profile, including the user's signed-in state and cookies.
    public string BrowserLaunchUrl { get; set; } = string.Empty;
    public string SecondTriggerBehavior { get; set; } = ExtendedWheelSecondTriggerBehavior.Minimize;

    public bool IsConfigured => Mode switch
    {
        ExtendedWheelActionMode.Hotkey => Enabled
            && !string.IsNullOrWhiteSpace(Hotkey),
        ExtendedWheelActionMode.Shortcut => Enabled
            && !string.IsNullOrWhiteSpace(ShortcutPath),
        _ => false
    };

    public string DisplayName
    {
        get
        {
            if (!string.IsNullOrWhiteSpace(Name))
            {
                return Name.Trim();
            }

            if (string.Equals(Mode, ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase)
                && !string.IsNullOrWhiteSpace(ShortcutPath))
            {
                return System.IO.Path.GetFileNameWithoutExtension(ShortcutPath);
            }

            if (string.Equals(Mode, ExtendedWheelActionMode.Hotkey, StringComparison.OrdinalIgnoreCase)
                && !string.IsNullOrWhiteSpace(Hotkey))
            {
                return Hotkey.Trim();
            }

            return "未设置";
        }
    }
}

public static class ExtendedWheelActionMode
{
    public const string None = "none";
    public const string Hotkey = "hotkey";
    public const string Shortcut = "shortcut";
}

public static class ExtendedWheelSecondTriggerBehavior
{
    public const string Close = "close";
    public const string Minimize = "minimize";
}

public sealed class ThemeSettings
{
    public string BackgroundColor { get; set; } = "#202020";
    public string SectorColor { get; set; } = "#2D2D2D";
    public string SectorHoverColor { get; set; } = "#3F6AFF";
    public string SectorBorderColor { get; set; } = "#666666";
    public string TextColor { get; set; } = "#FFFFFF";
    public string MutedTextColor { get; set; } = "#BBBBBB";
    public string CenterColor { get; set; } = "#111111";
}

public sealed class MouseSettings
{
    public string DefaultCaptureMode { get; set; } = "always";
    public string TriggerButton { get; set; } = "middle";
    public int LongPressThresholdMs { get; set; } = 120;
    public bool SuppressOriginalMiddleClick { get; set; } = true;
    public bool CancelWhenReleaseInDeadZone { get; set; } = true;
    public bool MiddleButtonCaptureEnabled { get; set; } = true;
}

public sealed class ClipboardSettings
{
    public int MaxHistoryItems { get; set; } = 8;
    public bool LoadWindowsClipboardHistoryOnStartup { get; set; } = true;
    public bool CapturePlainText { get; set; } = true;
    public bool CaptureHtml { get; set; } = true;
    public bool CaptureRtf { get; set; } = true;
    public bool CaptureCsv { get; set; } = true;
    public bool CaptureImages { get; set; } = false;
    public bool IgnorePasswordLikeText { get; set; } = false;
}

public sealed class PasteSettings
{
    public string DefaultMode { get; set; } = "smart";
    public bool RestoreClipboardAfterPaste { get; set; } = false;
    // 150ms is enough for the foreground app to read the clipboard during
    // Ctrl+V processing on a normally responsive system; bump it up via
    // settings.json if you see your original clipboard come back before
    // the paste lands.
    public int RestoreDelayMs { get; set; } = 150;
    public string CtrlModifierMode { get; set; } = "plainText";
    public string ShiftModifierMode { get; set; } = "formatted";
    // Wheel pastes are excluded from Windows clipboard history (Win+V) by
    // default so repetitive pastes don't push the user's real copies out.
    // Flip back to true to restore the original Win+V behaviour.
    public bool AddPasteToClipboardHistory { get; set; } = false;
}
