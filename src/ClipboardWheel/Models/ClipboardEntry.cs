namespace ClipboardWheel.Models;

public sealed class ClipboardEntry
{
    public string Id { get; init; } = Guid.NewGuid().ToString("N");
    public DateTime CreatedAt { get; init; } = DateTime.Now;
    public string? SourceProcessName { get; init; }
    public string DisplayText { get; init; } = string.Empty;
    public string? PlainText { get; init; }
    public string? HtmlText { get; init; }
    public string? RtfText { get; init; }
    public string? CsvText { get; init; }
    public string? TsvText { get; init; }
    public byte[]? ImagePngBytes { get; init; }
    public byte[]? PreviewImagePngBytes { get; init; }
    public string? ImageHash { get; init; }
    public bool IsImageContent { get; init; }
    public bool LooksLikeSpreadsheet { get; init; }
    public bool LooksLikeSingleCell { get; init; }
    public string PreferredPasteMode { get; init; } = "smart";

    /// <summary>True when this is a Quick Copy placeholder, not a real clipboard entry.</summary>
    public bool IsQuickCopy { get; init; }

    /// <summary>Runtime-only pin. Locked entries keep their current wheel slot until unlocked or the app exits.</summary>
    public bool IsLocked { get; set; }

    public bool HasImage => ImagePngBytes is { Length: > 0 };

    public bool HasFormattedPayload => !string.IsNullOrWhiteSpace(HtmlText) || !string.IsNullOrWhiteSpace(RtfText);
}
