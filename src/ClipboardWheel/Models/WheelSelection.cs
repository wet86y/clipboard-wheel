namespace ClipboardWheel.Models;

public sealed class WheelSelection
{
    public ClipboardEntry? Entry { get; init; }
    public ExtendedWheelActionSlot? ExtendedAction { get; init; }

    public static WheelSelection FromEntry(ClipboardEntry entry) => new() { Entry = entry };

    public static WheelSelection FromExtendedAction(ExtendedWheelActionSlot action) => new() { ExtendedAction = action };
}
