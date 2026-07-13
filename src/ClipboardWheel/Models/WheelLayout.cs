namespace ClipboardWheel.Models;

/// <summary>
/// Resolves the layout for the paste wheel.
///
/// Two shape families, each with a fixed tier ladder (no in-between values):
///   - circle:    { 4, 6, 8 }
///   - rectangle: { 4, 8 }       (2x2 or 2x4 grid; the 8-cell grid is the wider one, better for text)
///
/// Behaviour when clipboard has fewer entries than the configured tier:
///   - We never reduce the tier automatically; the configured tier is honoured.
///   - Empty slots are drawn (no text, slightly muted fill) so the user still
///     sees the layout shape they picked.
///   - User can manually pick a smaller tier in settings if they prefer larger
///     sectors and don't mind hiding entries.
/// </summary>
public static class WheelLayout
{
    public const string ShapeCircle = "circle";
    public const string ShapeRectangle = "rectangle";

    public static readonly int[] CircleTiers = { 4, 6, 8 };
    public static readonly int[] RectangleTiers = { 4, 8 };

    public static readonly string[] KnownShapes = { ShapeCircle, ShapeRectangle };

    /// <summary>
    /// Maps a shape name to its tier ladder. Unknown shapes fall back to
    /// Circle (legacy default).
    /// </summary>
    public static int[] GetTiersForShape(string? shape)
    {
        if (string.Equals(shape, ShapeRectangle, StringComparison.OrdinalIgnoreCase))
        {
            return RectangleTiers;
        }
        return CircleTiers;
    }

    /// <summary>
    /// Snaps an arbitrary sector count to the closest valid tier for the
    /// shape, preserving the user's preference when possible.
    /// </summary>
    public static int NormalizeSectorCount(string? shape, int requested)
    {
        var tiers = GetTiersForShape(shape);
        if (Array.IndexOf(tiers, requested) >= 0)
        {
            return requested;
        }
        return tiers.OrderBy(t => Math.Abs(t - requested)).First();
    }

    /// <summary>
    /// Validates a shape name. Returns ShapeCircle for null/empty/unknown.
    /// </summary>
    public static string NormalizeShape(string? shape)
    {
        if (string.IsNullOrWhiteSpace(shape)) return ShapeCircle;
        if (string.Equals(shape, ShapeRectangle, StringComparison.OrdinalIgnoreCase)) return ShapeRectangle;
        return ShapeCircle;
    }
}