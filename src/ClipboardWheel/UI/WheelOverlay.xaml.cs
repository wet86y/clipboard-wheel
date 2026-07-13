using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using ClipboardWheel.Models;
using ClipboardWheel.Native;
using ClipboardWheel.Services;

namespace ClipboardWheel.UI;

public partial class WheelOverlay : Window
{
    private readonly SettingsService _settingsService;

    private readonly List<Path> _sectorPaths = new();
    private readonly List<ScaleTransform> _sectorScales = new();  // paired with _sectorPaths
    private readonly List<Point> _sectorCentroids = new();
    private readonly List<Geometry> _sectorHitGeometries = new();   // for rectangle hit-testing
    private readonly List<Path> _extendedActionPaths = new();
    private readonly List<FrameworkElement> _extendedActionContents = new();
    private readonly List<TextBlock> _extendedActionTexts = new();
    private readonly List<ScaleTransform> _extendedActionScales = new();
    private readonly List<int> _extendedActionSlotIndexes = new();
    private readonly List<int> _extendedActionDirections = new();
    private List<ClipboardEntry?> _entries = new();
    private ClipboardEntry? _selected;
    private ExtendedWheelActionSlot? _selectedExtendedAction;
    private int _selectedIndex = -1;
    private int _selectedExtendedSlotIndex = -1;
    private int _activeExtendedDirection = -1;
    private int _visualGeneration;

    private Point _centerScreen;        // wheel centre in screen coordinates
    private double _wheelRadius;        // configured wheel radius (sectors drawn at this radius)
    private double _canvasPadding;      // pixels of padding around wheel content, so selected sectors have room to expand

    // ── Hardcoded layout constants (no longer user-configurable) ──
    private const double SectorGapPixels = 8;   // rectangle cell gap px
    private const double SelectedSectorScale = 1.04;
    private const double SelectionAnimMs = 75;
    private const double ShowAnimMs = 58;
    private const double HideAnimMs = 60;
    // Keep a selected sector until the pointer has crossed a small angular
    // buffer. This prevents ordinary hand tremor at a sector boundary from
    // changing the action just as the middle button is released.
    private const double MainSelectionHysteresisDegrees = 2.5;
    private const double ExtendedSelectionHysteresisDegrees = 3.0;
    private const double CircleSectorCornerRadius = 10;
    private const double RectSectorCornerRadius = 8;
    private const double RectCenterCutoutGap = 5;
    private const double RectEightCenterDeadZoneScale = 1.22;
    private const double RectImageFillFactor = 1.02;
    private const double SectorImageHeightFillFactor = 0.96;
    private const double SectorImageWidthFillFactor = 0.92;
    private const double DenseSectorImageFillBoost = 1.2;
    private const double ImageClipInsetPixels = 10;
    private const double ExtendedRingGap = 4;
    private const double ExtendedRingMinThickness = 52;
    private const double ExtendedActionAnimMs = 50;
    private const double ExtendedActionMarqueePixelsPerSecond = 28;
    private const double ExtendedActionMarqueeMinDurationMs = 1800;
    private const double ExtendedActionMarqueeLeadInMs = 350;
    private const double ExtendedActionIconMinWidth = 62;
    private const double ExtendedActionIconMinHeight = 50;
    private const int AnimationFrameRate = 60;
    private const string LockedSectorColor = "#4E6E9E";
    private const string LockedSectorHoverColor = "#5F83BA";
    // ────────────────────────────────────────────────────────────────

    public WheelOverlay(SettingsService settingsService)
    {
        _settingsService = settingsService;
        InitializeComponent();
        SourceInitialized += (_, _) =>
        {
            ApplyNoActivateStyle();
            ReassertTopmost();
        };
        Hide();
    }

    public void ShowWheel(Point screenPoint, IReadOnlyList<ClipboardEntry> entries)
    {
        var settings = _settingsService.Current;
        _visualGeneration++;
        var shape = WheelLayout.NormalizeShape(settings.Wheel.Shape);
        var resolvedCount = WheelLayout.NormalizeSectorCount(shape, settings.Wheel.SectorCount);

        var padded = new List<ClipboardEntry?>(resolvedCount);
        for (var i = 0; i < resolvedCount; i++)
        {
            padded.Add(i < entries.Count ? entries[i] : null);
        }
        _entries = padded;
        _selected = null;
        _selectedExtendedAction = null;
        _selectedIndex = -1;
        _selectedExtendedSlotIndex = -1;
        _activeExtendedDirection = -1;

        // Reserve padding so the selected sector's animated expansion isn't
        // clipped by the window edge.
        _wheelRadius = settings.Wheel.Radius;
        _canvasPadding = _wheelRadius * 0.06;
        if (IsExtendedWheelAvailable(settings, shape))
        {
            _canvasPadding += ExtendedRingGap + GetExtendedRingThickness(_wheelRadius) + _wheelRadius * 0.03;
        }
        var diameter = (_wheelRadius + _canvasPadding) * 2;

        Width = diameter;
        Height = diameter;

        var (scaleX, scaleY) = GetDeviceToDipScale();
        _centerScreen = settings.Wheel.ShowAtCursor
            ? new Point(screenPoint.X * scaleX, screenPoint.Y * scaleY)
            : new Point(SystemParameters.PrimaryScreenWidth / 2, SystemParameters.PrimaryScreenHeight / 2);
        Left = _centerScreen.X - _wheelRadius - _canvasPadding;
        Top = _centerScreen.Y - _wheelRadius - _canvasPadding;
        Opacity = settings.Wheel.AnimationEnabled ? 0.0 : settings.Wheel.Opacity;

        DrawWheel();
        Show();
        Topmost = true;
        ReassertTopmost();
        Dispatcher.BeginInvoke(
            DispatcherPriority.Render,
            new Action(() =>
            {
                if (IsVisible)
                {
                    ReassertTopmost();
                }
            }));
        PlayShowAnimation(settings.Wheel.Opacity, settings.Wheel.AnimationEnabled);
    }

    public void UpdatePointer(Point screenPoint)
    {
        if (!IsVisible) return;

        var (scaleX, scaleY) = GetDeviceToDipScale();
        var pt = new Point(screenPoint.X * scaleX, screenPoint.Y * scaleY);

        var dx = pt.X - _centerScreen.X;
        var dy = pt.Y - _centerScreen.Y;
        var distance = Math.Sqrt(dx * dx + dy * dy);

        if (_entries.Count == 0)
        {
            UpdateSelectionVisuals(-1, -1);
            return;
        }

        var settings = _settingsService.Current;
        var shape = WheelLayout.NormalizeShape(settings.Wheel.Shape);

        // Rectangle: cell-based hit test. Also respects the central dead
        // zone (shared with circle) so the middle area cancels selection.
        int index;
        if (shape == WheelLayout.ShapeRectangle)
        {
            SetActiveExtendedDirection(-1);
            if (distance < GetEffectiveDeadZoneRadius(settings, shape, _entries.Count))
            {
                UpdateSelectionVisuals(-1, -1);
                return;
            }
            index = HitTestRectangle(dx, dy);
        }
        else
        {
            if (distance < GetEffectiveDeadZoneRadius(settings, shape, _entries.Count))
            {
                SetActiveExtendedDirection(-1);
                UpdateSelectionVisuals(-1, -1);
                return;
            }

            var extendedDirection = ResolveExtendedDirection(settings, distance, dx, dy);
            SetActiveExtendedDirection(extendedDirection);
            if (extendedDirection >= 0)
            {
                var extendedSlot = HitTestExtendedAction(dx, dy, extendedDirection);
                if (extendedSlot >= 0)
                {
                    UpdateSelectionVisuals(-1, extendedSlot);
                    return;
                }
            }

            index = HitTestCircle(dx, dy);
        }

        UpdateSelectionVisuals(index, -1);
    }

    private int HitTestCircle(double dx, double dy)
    {
        var angle = Math.Atan2(dy, dx) * 180.0 / Math.PI;
        angle = (angle + 450.0) % 360.0;
        var sectorAngle = 360.0 / _entries.Count;
        var index = (int)Math.Floor(angle / sectorAngle);
        index = Math.Clamp(index, 0, _entries.Count - 1);

        if (_selectedIndex >= 0
            && _selectedIndex < _entries.Count
            && _selectedIndex != index
            && AngleInRange(
                angle,
                _selectedIndex * sectorAngle - MainSelectionHysteresisDegrees,
                (_selectedIndex + 1) * sectorAngle + MainSelectionHysteresisDegrees))
        {
            return _selectedIndex;
        }

        return index;
    }

    private int HitTestRectangle(double dx, double dy)
    {
        // dx, dy are relative to the wheel centre. Convert to canvas-local.
        var lx = dx + _wheelRadius + _canvasPadding;
        var ly = dy + _wheelRadius + _canvasPadding;
        var localPoint = new Point(lx, ly);
        for (var i = 0; i < _sectorHitGeometries.Count; i++)
        {
            if (_sectorHitGeometries[i].FillContains(localPoint))
            {
                return i;
            }
        }
        return -1;
    }

    private int ResolveExtendedDirection(AppSettings settings, double distance, double dx, double dy)
    {
        if (!IsExtendedWheelAvailable(settings, WheelLayout.ShapeCircle))
        {
            return -1;
        }

        if (distance <= _wheelRadius)
        {
            return -1;
        }

        var buffer = settings.Wheel.ExtendedWheel.BreakoutBufferPixels;
        if (distance <= _wheelRadius + buffer)
        {
            return _activeExtendedDirection;
        }

        var angle = Math.Atan2(dy, dx) * 180.0 / Math.PI;
        if (angle >= -45 && angle < 45)
        {
            return 1; // right
        }

        if (angle >= 45 && angle < 135)
        {
            return 2; // down
        }

        if (angle >= -135 && angle < -45)
        {
            return 0; // up
        }

        return 3; // left
    }

    private int HitTestExtendedAction(double dx, double dy, int direction)
    {
        var angle = Math.Atan2(dy, dx) * 180.0 / Math.PI;

        if (_selectedExtendedSlotIndex >= 0
            && IsExtendedActionSlotConfigured(_selectedExtendedSlotIndex)
            && GetExtendedSlotGeometry(_selectedExtendedSlotIndex, gapDegrees: 0).Direction == direction)
        {
            var (_, selectedStart, selectedEnd) = GetExtendedSlotGeometry(_selectedExtendedSlotIndex, gapDegrees: 0);
            if (AngleInRange(
                angle,
                selectedStart - ExtendedSelectionHysteresisDegrees,
                selectedEnd + ExtendedSelectionHysteresisDegrees))
            {
                return _selectedExtendedSlotIndex;
            }
        }

        for (var i = 0; i < _extendedActionPaths.Count; i++)
        {
            if (_extendedActionDirections[i] != direction)
            {
                continue;
            }

            var slotIndex = _extendedActionSlotIndexes[i];
            if (!IsExtendedActionSlotConfigured(slotIndex))
            {
                continue;
            }

            // Hit testing deliberately uses the full logical 30-degree slot.
            // Rendering keeps its visual gap, but that gap must not fall through
            // to the inner wheel when it lies between two configured actions.
            var (_, start, end) = GetExtendedSlotGeometry(slotIndex, gapDegrees: 0);
            if (AngleInRange(angle, start, end))
            {
                return slotIndex;
            }
        }

        return -1;
    }

    private bool IsExtendedActionSlotConfigured(int slotIndex)
    {
        var slots = _settingsService.Current.Wheel.ExtendedWheel.Slots;
        return slotIndex >= 0
            && slotIndex < slots.Count
            && slots[slotIndex].IsConfigured;
    }

    public WheelSelection? ConfirmSelection()
    {
        var settings = _settingsService.Current;
        if (_selectedExtendedAction is not null)
        {
            return WheelSelection.FromExtendedAction(_selectedExtendedAction);
        }

        if (settings.Mouse.CancelWhenReleaseInDeadZone && _selected is null)
        {
            return null;
        }
        return _selected is null ? null : WheelSelection.FromEntry(_selected);
    }

    public ClipboardEntry? GetSelectedEntry()
    {
        return _selected;
    }

    public void RefreshSelectionVisuals()
    {
        var selectedIndex = _selectedIndex;
        _selectedIndex = int.MinValue;
        UpdateSelectionVisuals(selectedIndex, _selectedExtendedSlotIndex);
    }

    public void HideWheel()
    {
        var settings = _settingsService.Current;
        _visualGeneration++;
        _selected = null;
        _selectedExtendedAction = null;
        _selectedExtendedSlotIndex = -1;
        _activeExtendedDirection = -1;
        if (!IsVisible || !settings.Wheel.AnimationEnabled)
        {
            ClearAndHide();
            return;
        }

        PlayHideAnimation(_visualGeneration);
    }

    public void ClearForShutdown()
    {
        _visualGeneration++;
        BeginAnimation(OpacityProperty, null);
        RootCanvas.RenderTransform = null;
        RootCanvas.Children.Clear();
        _sectorPaths.Clear();
        _sectorScales.Clear();
        _sectorCentroids.Clear();
        _sectorHitGeometries.Clear();
        _extendedActionPaths.Clear();
        _extendedActionContents.Clear();
        _extendedActionTexts.Clear();
        _extendedActionScales.Clear();
        _extendedActionSlotIndexes.Clear();
        _extendedActionDirections.Clear();
        _entries = new List<ClipboardEntry?>();
        _selected = null;
        _selectedExtendedAction = null;
        Hide();
    }

    private void DrawWheel()
    {
        RootCanvas.Children.Clear();
        _sectorPaths.Clear();
        _sectorScales.Clear();
        _sectorCentroids.Clear();
        _sectorHitGeometries.Clear();
        _extendedActionPaths.Clear();
        _extendedActionContents.Clear();
        _extendedActionTexts.Clear();
        _extendedActionScales.Clear();
        _extendedActionSlotIndexes.Clear();
        _extendedActionDirections.Clear();

        var settings = _settingsService.Current;
        RootCanvas.Width = (_wheelRadius + _canvasPadding) * 2;
        RootCanvas.Height = (_wheelRadius + _canvasPadding) * 2;

        var shape = WheelLayout.NormalizeShape(settings.Wheel.Shape);
        if (shape == WheelLayout.ShapeRectangle)
        {
            DrawRectangleWheel();
        }
        else
        {
            DrawCircleWheel();
        }

        // Inner dead-zone disc for both shapes.
        DrawCenterDisk(shape);

        UpdateSelectionVisuals(-1, -1);
    }

    // ──────────────────────────────────────────────────────────────────
    // Circle layout
    // ──────────────────────────────────────────────────────────────────

    private void DrawCircleWheel()
    {
        var settings = _settingsService.Current;
        var center = new Point(_wheelRadius + _canvasPadding, _wheelRadius + _canvasPadding);
        var dead = settings.Wheel.InnerDeadZoneRadius;
        var count = _entries.Count;
        if (count == 0) return;
        var sectorAngle = 360.0 / count;

        var normalBrush = BrushFromHex(settings.Theme.SectorColor);
        var textBrush = BrushFromHex(settings.Theme.TextColor);
        var emptyBrush = BrushFromHex(settings.Theme.MutedTextColor);
        var lockedBrush = BrushFromHex(LockedSectorColor);
        var lockedHoverBrush = BrushFromHex(LockedSectorHoverColor);
        var copyBrush = BrushFromHex("#2E7D32");  // dark green — Quick Copy

        // Each sector is uniformly scaled to 0.9× around its centroid.
        // This naturally creates even gaps between adjacent sectors
        // without needing a stroke or separator lines.
        const double baseScaleFactor = 0.9;
        const double normalFontSize = 12;
        const double quickCopyFontSize = 14;

        for (var i = 0; i < count; i++)
        {
            var start = -90.0 + i * sectorAngle;
            var end = -90.0 + (i + 1) * sectorAngle;

            var path = new Path
            {
                Data = CreateSectorGeometry(center, dead, _wheelRadius, start, end, CircleSectorCornerRadius),
                Stroke = null,
            };

            var entry = _entries[i];
            path.Fill = GetSectorFill(entry, false, normalBrush, normalBrush, lockedBrush, lockedHoverBrush, copyBrush, copyBrush, emptyBrush);
            path.Opacity = entry is null ? 0.35 : 1.0;
            path.Tag = i;

            var centroid = CircleSectorCentroid(center, dead, _wheelRadius, start, end);
            // TransformGroup: base 0.9× + animatable 1.0× → 1.04× on hover.
            var baseScale = new ScaleTransform(baseScaleFactor, baseScaleFactor, centroid.X, centroid.Y);
            var animScale = new ScaleTransform(1.0, 1.0, center.X, center.Y);
            var group = new TransformGroup();
            group.Children.Add(baseScale);
            group.Children.Add(animScale);
            path.RenderTransform = group;

            RootCanvas.Children.Add(path);
            _sectorPaths.Add(path);
            _sectorScales.Add(animScale); // animate this one
            _sectorCentroids.Add(centroid);

            if (entry is not null)
            {
                var fontSize = entry.IsQuickCopy ? quickCopyFontSize : normalFontSize;
                AddSectorPreviewText(entry, textBrush, center, dead, _wheelRadius, start, end, centroid, baseScaleFactor, fontSize);
            }
        }

        DrawExtendedActionSectors(center);
    }

    private void DrawExtendedActionSectors(Point center)
    {
        var settings = _settingsService.Current;
        if (!IsExtendedWheelAvailable(settings, WheelLayout.ShapeCircle))
        {
            return;
        }

        var slots = settings.Wheel.ExtendedWheel.Slots;
        var inner = _wheelRadius + ExtendedRingGap;
        var outer = inner + GetExtendedRingThickness(_wheelRadius);
        var normalBrush = BrushFromHex("#303642");
        var configuredBrush = BrushFromHex("#314A68");
        var emptyBrush = BrushFromHex("#666666");
        var textBrush = BrushFromHex(settings.Theme.TextColor);

        for (var slotIndex = 0; slotIndex < ExtendedWheelSettings.SlotCount; slotIndex++)
        {
            var (direction, start, end) = GetExtendedSlotGeometry(slotIndex, settings.Wheel.SectorGapDegrees);
            var slot = slots[slotIndex];
            var configured = slot.IsConfigured;
            var geometry = CreateSectorGeometry(center, inner, outer, start, end, CircleSectorCornerRadius);
            var path = new Path
            {
                Data = geometry,
                Fill = configured ? configuredBrush : normalBrush,
                Stroke = null,
                Opacity = configured ? 0.92 : 0.32,
                Visibility = Visibility.Hidden,
                Tag = slotIndex
            };

            var centroid = CircleSectorCentroid(center, inner, outer, start, end);
            var scale = new ScaleTransform(0.88, 0.88, center.X, center.Y);
            path.RenderTransform = scale;

            var budget = GetExtendedActionContentBudget(inner, outer, start, end);
            var (content, text) = CreateExtendedActionContent(slot, configured ? textBrush : emptyBrush, budget);
            content.Opacity = configured ? 0.95 : 0.55;
            content.Visibility = Visibility.Hidden;
            content.IsHitTestVisible = false;
            content.Tag = slotIndex;
            var contentCenter = GetExtendedActionContentCenter(center, inner, outer, start, end);
            Canvas.SetLeft(content, contentCenter.X - content.Width / 2);
            Canvas.SetTop(content, contentCenter.Y - content.Height / 2);

            RootCanvas.Children.Add(path);
            RootCanvas.Children.Add(content);
            _extendedActionPaths.Add(path);
            _extendedActionContents.Add(content);
            _extendedActionTexts.Add(text);
            _extendedActionScales.Add(scale);
            _extendedActionSlotIndexes.Add(slotIndex);
            _extendedActionDirections.Add(direction);
        }
    }

    private readonly struct ExtendedActionContentBudget
    {
        public double Width { get; }
        public double Height { get; }
        public bool CanShowIcon { get; }

        public ExtendedActionContentBudget(double width, double height, bool canShowIcon)
        {
            Width = width;
            Height = height;
            CanShowIcon = canShowIcon;
        }
    }

    private static ExtendedActionContentBudget GetExtendedActionContentBudget(double inner, double outer, double start, double end)
    {
        var radialThickness = Math.Max(1, outer - inner);
        var midRadius = (inner + outer) / 2;
        var sweepRad = Math.Abs(end - start) * Math.PI / 180.0;
        var tangentialWidth = Math.Max(1, 2 * midRadius * Math.Sin(sweepRad / 2));
        var width = Math.Clamp(tangentialWidth * 0.82, 42, 96);
        var height = Math.Clamp(radialThickness * 0.84, 34, 78);
        var canShowIcon = width >= ExtendedActionIconMinWidth && height >= ExtendedActionIconMinHeight;
        return new ExtendedActionContentBudget(width, height, canShowIcon);
    }

    private static Point GetExtendedActionContentCenter(
        Point wheelCenter,
        double inner,
        double outer,
        double start,
        double end)
    {
        var thickness = Math.Max(1, outer - inner);
        var midRadius = inner + thickness / 2.0;

        // A sector's bounding-box centre is not its visual centre. Position the
        // caption on the radial midpoint instead, so every direction follows the
        // same geometry without a screen-direction-specific offset.
        return PointOnCircle(wheelCenter, midRadius, (start + end) / 2.0);
    }

    private static (FrameworkElement Content, TextBlock Text) CreateExtendedActionContent(
        ExtendedWheelActionSlot slot,
        Brush textBrush,
        ExtendedActionContentBudget budget)
    {
        var shortcutMode = string.Equals(slot.Mode, ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase);
        var configured = slot.IsConfigured;
        var textViewportWidth = Math.Min(72, Math.Max(42, budget.Width));
        var text = new TextBlock
        {
            Text = configured ? GetExtendedActionName(slot) : string.Empty,
            Foreground = textBrush,
            FontSize = 10.5,
            FontWeight = configured ? FontWeights.SemiBold : FontWeights.Normal,
            Width = textViewportWidth,
            Height = 18,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.NoWrap,
            TextTrimming = TextTrimming.CharacterEllipsis,
            VerticalAlignment = VerticalAlignment.Center,
            RenderTransform = new TranslateTransform(),
            Tag = textViewportWidth
        };

        if (!configured)
        {
            return (new Grid { Width = 0, Height = 0, IsHitTestVisible = false }, text);
        }

        if (shortcutMode && budget.CanShowIcon)
        {
            var iconSize = Math.Clamp(Math.Min(budget.Height - 23, budget.Width * 0.52), 26, 40);
            var panel = new StackPanel
            {
                Width = budget.Width,
                Height = iconSize + 21,
                Orientation = System.Windows.Controls.Orientation.Vertical,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };

            var icon = new System.Windows.Controls.Image
            {
                Width = iconSize,
                Height = iconSize,
                Stretch = Stretch.Uniform,
                Source = LoadShortcutIcon(slot.ShortcutPath),
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                Margin = new Thickness(0, 0, 0, 2)
            };
            panel.Children.Add(icon);

            panel.Children.Add(new Border
            {
                Width = textViewportWidth,
                Height = 18,
                ClipToBounds = true,
                Child = text
            });
            return (panel, text);
        }

        var grid = new Grid
        {
            Width = budget.Width,
            Height = budget.Height,
            ClipToBounds = true
        };
        text.Height = 18;
        text.TextWrapping = TextWrapping.NoWrap;
        text.VerticalAlignment = VerticalAlignment.Center;
        text.HorizontalAlignment = System.Windows.HorizontalAlignment.Center;
        grid.Children.Add(text);
        return (grid, text);
    }

    private void UpdateExtendedActionText(TextBlock text, ExtendedWheelActionSlot slot, bool isSelected)
    {
        if (text.RenderTransform is not TranslateTransform transform)
        {
            transform = new TranslateTransform();
            text.RenderTransform = transform;
        }
        transform.BeginAnimation(TranslateTransform.XProperty, null);
        transform.X = 0;
        var viewportWidth = GetExtendedActionTextViewportWidth(text);
        text.Width = viewportWidth;
        text.TextAlignment = TextAlignment.Center;
        text.HorizontalAlignment = System.Windows.HorizontalAlignment.Center;
        text.TextTrimming = TextTrimming.CharacterEllipsis;

        if (!slot.IsConfigured)
        {
            text.Text = string.Empty;
            return;
        }

        var fullName = GetExtendedActionName(slot);
        var fullyMaskedCharacters = CountFullyMaskedTextElements(text, fullName, viewportWidth);
        if (_settingsService.Current.Wheel.AnimationEnabled && fullyMaskedCharacters > 2)
        {
            StartExtendedActionMarquee(text, fullName);
            return;
        }

        // Keep the complete value for normal WPF ellipsis handling. The old
        // fixed eight-character truncation was wrong for narrow glyphs, CJK
        // names and differently sized wheel layouts.
        text.Text = fullName;
        text.TextAlignment = TextAlignment.Center;
        text.TextTrimming = TextTrimming.CharacterEllipsis;
    }

    private static double GetExtendedActionTextViewportWidth(TextBlock text)
    {
        return text.Tag is double width && width > 0 ? width : 72;
    }

    private int CountFullyMaskedTextElements(TextBlock text, string value, double viewportWidth)
    {
        if (string.IsNullOrEmpty(value))
        {
            return 0;
        }

        var maskedCount = 0;
        var consumedWidth = 0.0;
        var elements = StringInfo.GetTextElementEnumerator(value);
        while (elements.MoveNext())
        {
            var element = elements.GetTextElement();
            if (consumedWidth >= viewportWidth)
            {
                maskedCount++;
            }

            consumedWidth += MeasureExtendedActionTextWidth(text, element);
        }

        return maskedCount;
    }

    private static string GetExtendedActionName(ExtendedWheelActionSlot slot)
    {
        if (!slot.IsConfigured)
        {
            return string.Empty;
        }

        return slot.DisplayName.Trim();
    }

    private double MeasureExtendedActionTextWidth(TextBlock text, string value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return 0;
        }

        var typeface = new Typeface(text.FontFamily, text.FontStyle, text.FontWeight, text.FontStretch);
        var pixelsPerDip = VisualTreeHelper.GetDpi(text).PixelsPerDip;
        var formatted = new FormattedText(
            value,
            CultureInfo.CurrentUICulture,
            System.Windows.FlowDirection.LeftToRight,
            typeface,
            text.FontSize,
            text.Foreground,
            pixelsPerDip);
        return formatted.WidthIncludingTrailingWhitespace;
    }

    private void StartExtendedActionMarquee(TextBlock text, string value)
    {
        if (text.RenderTransform is not TranslateTransform transform)
        {
            transform = new TranslateTransform();
            text.RenderTransform = transform;
        }

        var spacer = "   ";
        var cycleWidth = MeasureExtendedActionTextWidth(text, value + spacer);
        text.Text = value + spacer + value;
        text.Width = MeasureExtendedActionTextWidth(text, text.Text);
        text.TextAlignment = TextAlignment.Left;
        text.HorizontalAlignment = System.Windows.HorizontalAlignment.Left;
        text.TextTrimming = TextTrimming.None;
        transform.BeginAnimation(TranslateTransform.XProperty, null);
        transform.X = 0;

        var duration = TimeSpan.FromMilliseconds(Math.Max(
            ExtendedActionMarqueeMinDurationMs,
            cycleWidth / ExtendedActionMarqueePixelsPerSecond * 1000));
        var animation = new DoubleAnimation
        {
            From = 0,
            To = -cycleWidth,
            BeginTime = TimeSpan.FromMilliseconds(ExtendedActionMarqueeLeadInMs),
            Duration = duration,
            RepeatBehavior = RepeatBehavior.Forever
        };
        Timeline.SetDesiredFrameRate(animation, AnimationFrameRate);
        transform.BeginAnimation(TranslateTransform.XProperty, animation);
    }

    // ──────────────────────────────────────────────────────────────────
    // Rectangle layout (2x2 for 4; 3x3 with hollow centre for 8)
    // ──────────────────────────────────────────────────────────────────

    private void DrawRectangleWheel()
    {
        var settings = _settingsService.Current;
        var diameter = (_wheelRadius + _canvasPadding) * 2;
        var inset = _canvasPadding;
        var count = _entries.Count;
        if (count == 0) return;

        // Tier-driven grid layout. 8-tier uses a 3x3 grid with the centre cell
        // hollowed out (the "九宫格" pattern); 4-tier uses a plain 2x2.
        var (rows, cols) = count <= 4 ? (2, 2) : (3, 3);
        var gap = SectorGapPixels;

        var innerWidth = diameter - inset * 2 - gap * (cols - 1);
        var innerHeight = diameter - inset * 2 - gap * (rows - 1);
        var cellW = innerWidth / cols;
        var cellH = innerHeight / rows;

        var normalBrush = BrushFromHex(settings.Theme.SectorColor);
        var textBrush = BrushFromHex(settings.Theme.TextColor);
        var emptyBrush = BrushFromHex(settings.Theme.MutedTextColor);
        var lockedBrush = BrushFromHex(LockedSectorColor);
        var lockedHoverBrush = BrushFromHex(LockedSectorHoverColor);
        var copyBrush = BrushFromHex("#2E7D32");

        var idx = 0;

        // Clockwise sector order so the rectangle matches the circle's
        // radial layout.  2×2 starts at top-right; 3×3 starts at top-
        // centre and winds clockwise, skipping the middle cell.
        var order = (rows, cols) switch
        {
            (2, 2) => new[] { (0, 1), (1, 1), (1, 0), (0, 0) },
            (3, 3) => new[] { (0, 1), (0, 2), (1, 2), (2, 2),
                              (2, 1), (2, 0), (1, 0), (0, 0) },
            _ => Array.Empty<(int r, int c)>()
        };

        var center = new Point(_wheelRadius + _canvasPadding, _wheelRadius + _canvasPadding);
        var centerDeadZoneRadius = GetEffectiveDeadZoneRadius(settings, WheelLayout.ShapeRectangle, count);
        var centerCutoutRadius = centerDeadZoneRadius + RectCenterCutoutGap;

        foreach (var (r, c) in order)
        {
            if (idx >= count) break;

            var x = inset + c * (cellW + gap);
            var y = inset + r * (cellH + gap);

            var rectGeom = CreateRectangleSectorGeometry(
                new Rect(x, y, cellW, cellH),
                center,
                centerCutoutRadius,
                RectSectorCornerRadius);
            var path = new Path
            {
                Data = rectGeom,
                Stroke = null,
            };
            var entry = _entries[idx];
            path.Fill = GetSectorFill(entry, false, normalBrush, normalBrush, lockedBrush, lockedHoverBrush, copyBrush, copyBrush, emptyBrush);
            path.Opacity = entry is null ? 0.35 : 1.0;
            path.Tag = idx;

            var cellCentre = new Point(x + cellW / 2, y + cellH / 2);
            var scaleOrigin = count <= 4 ? center : cellCentre;
            var scale = new ScaleTransform(1.0, 1.0, scaleOrigin.X, scaleOrigin.Y);
            path.RenderTransform = scale;

            RootCanvas.Children.Add(path);
            _sectorPaths.Add(path);
            _sectorScales.Add(scale);
            _sectorCentroids.Add(cellCentre);
            _sectorHitGeometries.Add(rectGeom);

            if (entry is not null)
            {
                var rl = ComputeRectTextLayout(cellW, cellH);
                if (entry.IsImageContent)
                {
                    AddRectanglePreviewImage(
                        entry,
                        new Rect(x, y, cellW, cellH),
                        center,
                        centerCutoutRadius,
                        cellCentre,
                        rl);
                }
                else
                {
                    AddRectanglePreviewText(entry, textBrush, rectGeom, cellCentre, rl);
                }
            }
            idx++;
        }
    }

    // ──────────────────────────────────────────────────────────────────
    // Shared
    // ──────────────────────────────────────────────────────────────────

    private void DrawCenterDisk(string shape)
    {
        var settings = _settingsService.Current;
        var center = new Point(_wheelRadius + _canvasPadding, _wheelRadius + _canvasPadding);
        var dead = GetEffectiveDeadZoneRadius(settings, shape, _entries.Count);
        var centerBrush = BrushFromHex(settings.Theme.CenterColor);

        var deadZone = new Ellipse
        {
            Width = dead * 2,
            Height = dead * 2,
            Fill = centerBrush,
            Stroke = null,
            Opacity = 0.9
        };
        Canvas.SetLeft(deadZone, center.X - dead);
        Canvas.SetTop(deadZone, center.Y - dead);
        RootCanvas.Children.Add(deadZone);
    }

    // ──────────────────────────────────────────────────────────────────
    // Auto-computed text layout (replaces hardcoded preview-chars setting)
    // ──────────────────────────────────────────────────────────────────

    private readonly struct TextLayout
    {
        public readonly double TextWidth;
        public readonly double TextHeight;
        public TextLayout(double tw, double th) { TextWidth = tw; TextHeight = th; }
    }

    private readonly struct TextLineSlot
    {
        public readonly Point Center;
        public readonly double Width;
        public TextLineSlot(Point center, double width) { Center = center; Width = width; }
    }

    /// <summary>Compute text area for a rectangle cell.</summary>
    private static TextLayout ComputeRectTextLayout(double cellW, double cellH)
    {
        var textW = Math.Max(44, cellW - 44);
        var textH = Math.Max(32, cellH - 48);
        return new TextLayout(textW, textH);
    }

    private void AddRectanglePreviewText(
        ClipboardEntry entry,
        Brush textBrush,
        Geometry clipGeometry,
        Point cellCenter,
        TextLayout layout)
    {
        var text = GetPreviewText(entry);
        if (string.IsNullOrWhiteSpace(text))
        {
            return;
        }

        var fontSize = entry.IsQuickCopy ? 14 : 12;
        var fontWeight = entry.IsQuickCopy ? FontWeights.Bold : FontWeights.Normal;
        var foreground = entry.IsQuickCopy ? BrushFromHex("#3F6AFF") : textBrush;
        var lineHeight = fontSize * 1.25;
        var maxLines = Math.Max(1, (int)(layout.TextHeight / lineHeight));
        var textWidth = entry.IsQuickCopy ? layout.TextWidth * 1.18 : layout.TextWidth;
        var widths = new double[maxLines];
        for (var i = 0; i < widths.Length; i++)
        {
            widths[i] = textWidth;
        }

        var (firstSlot, lines) = WrapPreviewTextCentered(text, widths, fontSize);
        if (lines.Count == 0)
        {
            return;
        }

        var textTop = cellCenter.Y - layout.TextHeight / 2;
        for (var i = 0; i < lines.Count; i++)
        {
            var slot = firstSlot + i;
            var center = new Point(cellCenter.X, textTop + slot * lineHeight + lineHeight / 2);
            AddPreviewLine(
                lines[i],
                foreground,
                fontSize,
                fontWeight,
                center,
                textWidth,
                lineHeight,
                clipGeometry,
                null);
        }
    }

    private void AddRectanglePreviewImage(
        ClipboardEntry entry,
        Rect cellRect,
        Point wheelCenter,
        double centerCutoutRadius,
        Point cellCenter,
        TextLayout layout)
    {
        var image = DecodePreviewImage(entry);
        if (image is null)
        {
            PasteTrace.Mark($"PreviewImage_rectangle_decode_failed bytes={entry.ImagePngBytes?.Length ?? 0}");
            return;
        }

        var (previewWidth, previewHeight) = FitImageInside(
            image,
            layout.TextWidth * RectImageFillFactor,
            layout.TextHeight * RectImageFillFactor);
        AddPreviewImage(
            image,
            cellCenter,
            previewWidth,
            previewHeight,
            CreateInsetRectangleSectorGeometry(cellRect, wheelCenter, centerCutoutRadius, RectSectorCornerRadius),
            null);
    }

    private void AddSectorPreviewText(
        ClipboardEntry entry,
        Brush textBrush,
        Point center,
        double innerRadius,
        double outerRadius,
        double startAngleDeg,
        double endAngleDeg,
        Point sectorCentroid,
        double baseScaleFactor,
        double fontSize)
    {
        var text = GetPreviewText(entry);
        if (string.IsNullOrWhiteSpace(text))
        {
            return;
        }

        var fontWeight = entry.IsQuickCopy ? FontWeights.Bold : FontWeights.Normal;
        var foreground = entry.IsQuickCopy ? BrushFromHex("#3F6AFF") : textBrush;
        var lineHeight = fontSize * 1.25;
        var innerMargin = Math.Max(22, fontSize * 1.8);
        var outerMargin = Math.Max(30, fontSize * 2.4);
        var edgeAngleMargin = Math.Max(4.0, Math.Abs(endAngleDeg - startAngleDeg) * 0.10);
        var sectorClip = CreateSectorGeometry(center, innerRadius, outerRadius, startAngleDeg, endAngleDeg, CircleSectorCornerRadius);
        var imageClip = CreateInsetSectorGeometry(center, innerRadius, outerRadius, startAngleDeg, endAngleDeg, CircleSectorCornerRadius);
        var slots = BuildSectorTextLineSlots(
            sectorClip,
            center,
            innerRadius + innerMargin,
            outerRadius - outerMargin,
            startAngleDeg,
            endAngleDeg,
            edgeAngleMargin,
            lineHeight,
            entry.IsQuickCopy ? 1.10 : 1.0);

        if (slots.Count == 0)
        {
            return;
        }

        if (entry.IsImageContent)
        {
            var image = DecodePreviewImage(entry);
            if (image is null)
            {
                PasteTrace.Mark($"PreviewImage_sector_decode_failed bytes={entry.ImagePngBytes?.Length ?? 0}");
                return;
            }

            try
            {
                var imageLayout = ComputeSectorImageLayout(
                    image,
                    slots,
                    lineHeight,
                    innerRadius,
                    outerRadius,
                    startAngleDeg,
                    endAngleDeg);

                AddPreviewImage(
                    image,
                    imageLayout.Center,
                    imageLayout.Width,
                    imageLayout.Height,
                    imageClip,
                    new ScaleTransform(baseScaleFactor, baseScaleFactor, sectorCentroid.X, sectorCentroid.Y));
            }
            catch (Exception ex)
            {
                PasteTrace.Mark($"PreviewImage_sector_layout_exception {ex.GetType().Name}: {ex.Message}");
            }

            return;
        }

        var widths = new double[slots.Count];
        for (var i = 0; i < slots.Count; i++)
        {
            widths[i] = slots[i].Width;
        }

        var (firstSlot, lines) = WrapPreviewTextCentered(text, widths, fontSize);
        if (lines.Count == 0)
        {
            return;
        }

        for (var lineIndex = 0; lineIndex < lines.Count; lineIndex++)
        {
            var slot = slots[Math.Min(firstSlot + lineIndex, slots.Count - 1)];
            AddPreviewLine(
                lines[lineIndex],
                foreground,
                fontSize,
                fontWeight,
                slot.Center,
                slot.Width,
                lineHeight,
                sectorClip,
                new ScaleTransform(baseScaleFactor, baseScaleFactor, sectorCentroid.X - (slot.Center.X - slot.Width / 2), sectorCentroid.Y - (slot.Center.Y - lineHeight / 2)));
        }
    }

    private void AddPreviewLine(
        string text,
        Brush foreground,
        double fontSize,
        FontWeight fontWeight,
        Point center,
        double width,
        double lineHeight,
        Geometry clipGeometry,
        Transform? renderTransform)
    {
        var label = new TextBlock
        {
            Text = text,
            Foreground = foreground,
            FontSize = fontSize,
            FontWeight = fontWeight,
            TextWrapping = TextWrapping.NoWrap,
            TextTrimming = TextTrimming.None,
            Width = width,
            Height = lineHeight,
            TextAlignment = TextAlignment.Center,
            LineStackingStrategy = LineStackingStrategy.BlockLineHeight,
            LineHeight = lineHeight,
        };

        var left = center.X - width / 2;
        var top = center.Y - lineHeight / 2;
        var clip = clipGeometry.Clone();
        clip.Transform = new TranslateTransform(-left, -top);
        label.Clip = clip;
        label.RenderTransform = renderTransform;

        Canvas.SetLeft(label, left);
        Canvas.SetTop(label, top);
        RootCanvas.Children.Add(label);
    }

    private void AddPreviewImage(
        ImageSource imageSource,
        Point center,
        double width,
        double height,
        Geometry clipGeometry,
        Transform? renderTransform)
    {
        var left = center.X - width / 2;
        var top = center.Y - height / 2;
        var previewRect = new Rect(left, top, width, height);
        var clippedPreview = new CombinedGeometry(
            GeometryCombineMode.Intersect,
            clipGeometry.Clone(),
            new RectangleGeometry(previewRect));
        clippedPreview.Freeze();

        var brush = new ImageBrush(imageSource)
        {
            Stretch = Stretch.Uniform,
            AlignmentX = AlignmentX.Center,
            AlignmentY = AlignmentY.Center,
            Viewport = previewRect,
            ViewportUnits = BrushMappingMode.Absolute,
        };
        if (brush.CanFreeze)
        {
            brush.Freeze();
        }

        var path = new Path
        {
            Data = clippedPreview,
            Fill = brush,
            Opacity = 0.96,
            SnapsToDevicePixels = true,
        };

        path.RenderTransform = renderTransform;
        PasteTrace.Mark($"PreviewImage_add width={width:0.0} height={height:0.0} left={left:0.0} top={top:0.0}");

        RootCanvas.Children.Add(path);
    }

    private static (double width, double height) FitImageInside(BitmapSource image, double maxWidth, double maxHeight)
    {
        if (image.PixelWidth <= 0 || image.PixelHeight <= 0 || maxWidth <= 0 || maxHeight <= 0)
        {
            return (Math.Max(1, maxWidth), Math.Max(1, maxHeight));
        }

        var imageAspect = image.PixelWidth / (double)image.PixelHeight;
        var boxAspect = maxWidth / maxHeight;
        return imageAspect >= boxAspect
            ? (maxWidth, Math.Max(1, maxWidth / imageAspect))
            : (Math.Max(1, maxHeight * imageAspect), maxHeight);
    }

    private readonly struct ImagePreviewLayout
    {
        public readonly Point Center;
        public readonly double Width;
        public readonly double Height;

        public ImagePreviewLayout(Point center, double width, double height)
        {
            Center = center;
            Width = width;
            Height = height;
        }
    }

    private static ImagePreviewLayout ComputeSectorImageLayout(
        BitmapSource image,
        IReadOnlyList<TextLineSlot> slots,
        double lineHeight,
        double innerRadius,
        double outerRadius,
        double startAngleDeg,
        double endAngleDeg)
    {
        var center = WeightedSlotCenter(slots);
        var availableTop = slots.First().Center.Y - lineHeight / 2;
        var availableBottom = slots.Last().Center.Y + lineHeight / 2;
        var safeInnerRadius = innerRadius + ImageClipInsetPixels;
        var safeOuterRadius = Math.Max(safeInnerRadius + 1, outerRadius - ImageClipInsetPixels);
        var safeRadialThickness = Math.Max(lineHeight, safeOuterRadius - safeInnerRadius);
        var midRadius = (safeInnerRadius + safeOuterRadius) / 2;
        var sweepDeg = Math.Abs(endAngleDeg - startAngleDeg);
        var sweepRad = sweepDeg * Math.PI / 180.0;
        var tangentialWidth = Math.Max(lineHeight, 2 * midRadius * Math.Sin(sweepRad / 2) - 2 * ImageClipInsetPixels);
        var denseSectorBoost = sweepDeg <= 60.0 + 0.1 ? DenseSectorImageFillBoost : 1.0;
        var maxHeight = safeRadialThickness * SectorImageHeightFillFactor * denseSectorBoost;
        var maxWidth = tangentialWidth * SectorImageWidthFillFactor * denseSectorBoost;
        var (width, height) = FitImageInside(image, maxWidth, maxHeight);

        for (var i = 0; i < 3; i++)
        {
            center = ClampImageCenterToSlots(center, width, height, slots, lineHeight, availableTop, availableBottom);
            (width, height) = FitImageInside(image, maxWidth, maxHeight);
        }

        center = ClampImageCenterToSlots(center, width, height, slots, lineHeight, availableTop, availableBottom);
        return new ImagePreviewLayout(center, width, height);
    }

    private static Point WeightedSlotCenter(IReadOnlyList<TextLineSlot> slots)
    {
        var weightedX = 0.0;
        var weightedY = 0.0;
        var total = 0.0;

        foreach (var slot in slots)
        {
            var weight = Math.Max(1, slot.Width);
            weightedX += slot.Center.X * weight;
            weightedY += slot.Center.Y * weight;
            total += weight;
        }

        return total <= 0
            ? slots[slots.Count / 2].Center
            : new Point(weightedX / total, weightedY / total);
    }

    private static Point ClampImageCenterToSlots(
        Point desired,
        double width,
        double height,
        IReadOnlyList<TextLineSlot> slots,
        double lineHeight,
        double availableTop,
        double availableBottom)
    {
        var minY = availableTop + height / 2;
        var maxY = availableBottom - height / 2;
        var y = minY <= maxY
            ? Math.Clamp(desired.Y, minY, maxY)
            : (availableTop + availableBottom) / 2;
        var half = Math.Max(lineHeight / 2, height / 2 + lineHeight / 2);
        var minCenterX = double.NegativeInfinity;
        var maxCenterX = double.PositiveInfinity;
        var weightedX = 0.0;
        var total = 0.0;

        foreach (var slot in slots)
        {
            if (Math.Abs(slot.Center.Y - y) > half)
            {
                continue;
            }

            var left = slot.Center.X - slot.Width / 2;
            var right = slot.Center.X + slot.Width / 2;
            minCenterX = Math.Max(minCenterX, left + width / 2);
            maxCenterX = Math.Min(maxCenterX, right - width / 2);

            var weight = Math.Max(1, slot.Width);
            weightedX += slot.Center.X * weight;
            total += weight;
        }

        var x = total > 0 ? weightedX / total : desired.X;
        if (minCenterX <= maxCenterX)
        {
            x = Math.Clamp(x, minCenterX, maxCenterX);
        }

        return new Point(x, y);
    }

    private static BitmapSource? DecodePreviewImage(ClipboardEntry entry)
    {
        var bytes = entry.PreviewImagePngBytes is { Length: > 0 } previewBytes
            ? previewBytes
            : entry.ImagePngBytes;
        if (bytes is not { Length: > 0 })
        {
            return null;
        }

        try
        {
            using var stream = new System.IO.MemoryStream(bytes);
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            frame?.Freeze();
            if (frame is not null)
            {
                PasteTrace.Mark($"PreviewImage_decode_ok bytes={bytes.Length} size={frame.PixelWidth}x{frame.PixelHeight}");
            }
            return frame;
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"PreviewImage_decode_exception {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    private static List<TextLineSlot> BuildSectorTextLineSlots(
        Geometry sectorClip,
        Point center,
        double innerRadius,
        double outerRadius,
        double startAngleDeg,
        double endAngleDeg,
        double edgeAngleMarginDeg,
        double lineHeight,
        double widthScale)
    {
        var slots = new List<TextLineSlot>();
        if (outerRadius <= innerRadius)
        {
            return slots;
        }

        var bounds = sectorClip.Bounds;
        var y = bounds.Top + lineHeight / 2;
        while (y <= bounds.Bottom - lineHeight / 2)
        {
            double? runStart = null;
            var bestStart = 0.0;
            var bestEnd = 0.0;
            var xStep = 2.0;

            for (var x = bounds.Left; x <= bounds.Right; x += xStep)
            {
                var pt = new Point(x, y);
                var inside = IsPointInSectorTextBand(
                    pt,
                    center,
                    innerRadius,
                    outerRadius,
                    startAngleDeg,
                    endAngleDeg,
                    edgeAngleMarginDeg)
                    && sectorClip.FillContains(pt);

                if (inside)
                {
                    runStart ??= x;
                }
                else if (runStart is not null)
                {
                    if (x - runStart.Value > bestEnd - bestStart)
                    {
                        bestStart = runStart.Value;
                        bestEnd = x - xStep;
                    }
                    runStart = null;
                }
            }

            if (runStart is not null && bounds.Right - runStart.Value > bestEnd - bestStart)
            {
                bestStart = runStart.Value;
                bestEnd = bounds.Right;
            }

            var width = (bestEnd - bestStart) * 0.88 * widthScale;
            if (width >= 24)
            {
                slots.Add(new TextLineSlot(new Point((bestStart + bestEnd) / 2, y), width));
            }

            y += lineHeight;
        }

        return slots;
    }

    private static (int firstSlot, List<string> lines) WrapPreviewTextCentered(string text, IReadOnlyList<double> widths, double fontSize)
    {
        var firstSlot = 0;
        var lines = WrapPreviewText(text, widths, fontSize);
        if (lines.Count == 0)
        {
            return (0, lines);
        }

        for (var attempt = 0; attempt < 6; attempt++)
        {
            var nextFirst = Math.Max(0, (widths.Count - lines.Count) / 2);
            if (nextFirst == firstSlot)
            {
                break;
            }

            firstSlot = nextFirst;
            lines = WrapPreviewText(text, SliceWidths(widths, firstSlot), fontSize);
            if (lines.Count == 0)
            {
                return (0, lines);
            }
        }

        return (firstSlot, lines);
    }

    private static double[] SliceWidths(IReadOnlyList<double> widths, int start)
    {
        var result = new double[Math.Max(0, widths.Count - start)];
        for (var i = 0; i < result.Length; i++)
        {
            result[i] = widths[start + i];
        }
        return result;
    }

    private static List<string> WrapPreviewText(string text, IReadOnlyList<double> widths, double fontSize)
    {
        var lines = new List<string>();
        var index = 0;

        for (var lineIndex = 0; lineIndex < widths.Count && index < text.Length; lineIndex++)
        {
            while (index < text.Length && (text[index] == ' ' || text[index] == '\n'))
            {
                index++;
            }

            if (index >= text.Length)
            {
                break;
            }

            var line = TakePreviewLine(text, ref index, widths[lineIndex], fontSize);
            if (lineIndex == widths.Count - 1 && index < text.Length)
            {
                line = AddEllipsisToFit(line, widths[lineIndex], fontSize);
            }

            if (!string.IsNullOrEmpty(line))
            {
                lines.Add(line);
            }
        }

        return lines;
    }

    private static string TakePreviewLine(string text, ref int index, double maxWidth, double fontSize)
    {
        var start = index;
        var width = 0.0;

        while (index < text.Length)
        {
            var ch = text[index];
            if (ch == '\n')
            {
                index++;
                break;
            }

            var nextWidth = width + EstimatePreviewCharWidth(ch, fontSize);
            if (nextWidth > maxWidth && index > start)
            {
                break;
            }

            width = nextWidth;
            index++;

            if (nextWidth > maxWidth)
            {
                break;
            }
        }

        return text[start..index].Trim();
    }

    private static string AddEllipsisToFit(string text, double maxWidth, double fontSize)
    {
        const string ellipsis = "…";
        var maxTextWidth = Math.Max(0, maxWidth - EstimatePreviewCharWidth(ellipsis[0], fontSize));
        while (text.Length > 0 && EstimatePreviewTextWidth(text, fontSize) > maxTextWidth)
        {
            text = text[..^1];
        }
        return text.TrimEnd() + ellipsis;
    }

    private static double EstimatePreviewTextWidth(string text, double fontSize)
    {
        var width = 0.0;
        foreach (var ch in text)
        {
            width += EstimatePreviewCharWidth(ch, fontSize);
        }
        return width;
    }

    private static double EstimatePreviewCharWidth(char ch, double fontSize)
    {
        if (ch == ' ')
        {
            return fontSize * 0.35;
        }

        if (ch < 128)
        {
            return fontSize * 0.58;
        }

        return fontSize * 0.96;
    }

    private static bool IsPointInSectorTextBand(
        Point point,
        Point center,
        double innerRadius,
        double outerRadius,
        double startAngleDeg,
        double endAngleDeg,
        double edgeAngleMarginDeg)
    {
        var dx = point.X - center.X;
        var dy = point.Y - center.Y;
        var distance = Math.Sqrt(dx * dx + dy * dy);
        if (distance < innerRadius || distance > outerRadius)
        {
            return false;
        }

        var angle = Math.Atan2(dy, dx) * 180.0 / Math.PI;
        return IsAngleWithinSweep(angle, startAngleDeg, endAngleDeg, edgeAngleMarginDeg);
    }

    private static bool IsAngleWithinSweep(double angleDeg, double startAngleDeg, double endAngleDeg, double marginDeg)
    {
        var sweep = NormalizeAnglePositive(endAngleDeg - startAngleDeg);
        if (sweep <= marginDeg * 2)
        {
            return true;
        }

        var relative = NormalizeAnglePositive(angleDeg - startAngleDeg);
        return relative >= marginDeg && relative <= sweep - marginDeg;
    }

    private static double NormalizeAnglePositive(double angleDeg)
    {
        var angle = angleDeg % 360.0;
        return angle < 0 ? angle + 360.0 : angle;
    }

    private static string GetPreviewText(ClipboardEntry entry)
    {
        var text = !string.IsNullOrWhiteSpace(entry.PlainText)
            ? entry.PlainText
            : entry.DisplayText;

        if (string.IsNullOrWhiteSpace(text))
        {
            return "";
        }

        return text.Replace("\r\n", "\n").Replace('\r', '\n').Replace("\t", "    ").Trim();
    }

    // ──────────────────────────────────────────────────────────────────

    private void UpdateSelectionVisuals(int mainIndex, int extendedSlotIndex)
    {
        UpdateSectorVisuals(mainIndex);
        UpdateExtendedActionVisuals(extendedSlotIndex);
    }

    private void UpdateSectorVisuals(int index)
    {
        if (index == _selectedIndex)
        {
            return;
        }

        var settings = _settingsService.Current;
        var normal = BrushFromHex(settings.Theme.SectorColor);
        var hover = BrushFromHex(settings.Theme.SectorHoverColor);
        var locked = BrushFromHex(LockedSectorColor);
        var lockedHover = BrushFromHex(LockedSectorHoverColor);
        var copy = BrushFromHex("#2E7D32");
        var copyHover = BrushFromHex("#2E7D32");  // dark green — Quick Copy selected
        var empty = BrushFromHex(settings.Theme.MutedTextColor);
        var targetScale = SelectedSectorScale;
        var animDuration = TimeSpan.FromMilliseconds(SelectionAnimMs);

        ClipboardEntry? selectedEntry = null;
        for (var i = 0; i < _sectorPaths.Count; i++)
        {
            var path = _sectorPaths[i];
            var scale = _sectorScales[i];
            var entry = _entries[i];

            if (entry is null)
            {
                // Empty slot: keep muted fill, no selection state ever.
                path.Fill = empty;
                AnimateScale(scale, 1.0, animDuration);
                continue;
            }

            var isSelected = i == index;
            path.Fill = GetSectorFill(entry, isSelected, normal, hover, locked, lockedHover, copy, copyHover, empty);

            // Animate the scale smoothly. WPF's BeginAnimation handles the
            // "from current value" interpolation when From is unset, so a
            // rapid sector change still feels fluid (no jump back to 1.0).
            AnimateScale(scale, isSelected ? targetScale : 1.0, animDuration);

            if (isSelected)
            {
                selectedEntry = entry;
            }
        }

        _selected = selectedEntry;
        _selectedIndex = index;
    }

    private void UpdateExtendedActionVisuals(int slotIndex)
    {
        if (slotIndex == _selectedExtendedSlotIndex)
        {
            return;
        }

        var settings = _settingsService.Current;
        var normal = BrushFromHex("#303642");
        var configured = BrushFromHex("#314A68");
        var hover = BrushFromHex(settings.Theme.SectorHoverColor);
        var empty = BrushFromHex("#666666");
        var duration = TimeSpan.FromMilliseconds(SelectionAnimMs);

        ExtendedWheelActionSlot? selectedAction = null;
        for (var i = 0; i < _extendedActionPaths.Count; i++)
        {
            var path = _extendedActionPaths[i];
            var content = _extendedActionContents[i];
            var text = _extendedActionTexts[i];
            var scale = _extendedActionScales[i];
            var currentSlotIndex = _extendedActionSlotIndexes[i];
            var slot = settings.Wheel.ExtendedWheel.Slots[currentSlotIndex];
            var isConfigured = slot.IsConfigured;
            var isSelected = isConfigured && currentSlotIndex == slotIndex;

            path.Fill = isSelected ? hover : isConfigured ? configured : normal;
            path.Opacity = isSelected ? 0.96 : isConfigured ? 0.92 : 0.32;
            text.Foreground = isConfigured ? BrushFromHex(settings.Theme.TextColor) : empty;
            content.Opacity = isSelected ? 1.0 : isConfigured ? 0.95 : 0.55;
            UpdateExtendedActionText(text, slot, isSelected);
            AnimateScale(scale, isSelected ? SelectedSectorScale : 1.0, duration);

            if (isSelected && isConfigured)
            {
                selectedAction = slot;
            }
        }

        _selectedExtendedAction = selectedAction;
        _selectedExtendedSlotIndex = selectedAction is null ? -1 : slotIndex;
    }

    private void SetActiveExtendedDirection(int direction)
    {
        if (direction == _activeExtendedDirection)
        {
            return;
        }

        var previousDirection = _activeExtendedDirection;
        _activeExtendedDirection = direction;
        if (direction < 0)
        {
            _selectedExtendedAction = null;
            _selectedExtendedSlotIndex = -1;
        }

        var generation = ++_visualGeneration;
        var duration = TimeSpan.FromMilliseconds(ExtendedActionAnimMs);
        PasteTrace.Mark($"ExtendedWheel_direction_changed from={previousDirection} to={direction}");
        for (var i = 0; i < _extendedActionPaths.Count; i++)
        {
            var visible = _extendedActionDirections[i] == direction;
            if (visible)
            {
                var slot = _settingsService.Current.Wheel.ExtendedWheel.Slots[_extendedActionSlotIndexes[i]];
                UpdateExtendedActionText(_extendedActionTexts[i], slot, isSelected: false);
            }
            else
            {
                StopExtendedActionMarquee(_extendedActionTexts[i]);
            }
            SetExtendedVisualVisibility(_extendedActionPaths[i], _extendedActionScales[i], visible, duration, generation);
            SetExtendedContentVisibility(_extendedActionContents[i], visible, duration, generation);
        }
    }

    private static void StopExtendedActionMarquee(TextBlock text)
    {
        if (text.RenderTransform is not TranslateTransform transform)
        {
            return;
        }

        transform.BeginAnimation(TranslateTransform.XProperty, null);
        transform.X = 0;
    }

    private void SetExtendedVisualVisibility(Path path, ScaleTransform scale, bool visible, TimeSpan duration, int generation)
    {
        if (visible)
        {
            var targetOpacity = path.Opacity;
            path.BeginAnimation(OpacityProperty, null);
            path.Opacity = 0.0;
            path.Visibility = Visibility.Visible;
            scale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            scale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
            scale.ScaleX = 0.88;
            scale.ScaleY = 0.88;
            path.BeginAnimation(
                OpacityProperty,
                CreateDoubleAnimation(0.0, targetOpacity, duration, new QuadraticEase { EasingMode = EasingMode.EaseOut }));
            AnimateScale(scale, 1.0, duration);
        }
        else
        {
            if (path.Visibility != Visibility.Visible)
            {
                scale.ScaleX = 0.88;
                scale.ScaleY = 0.88;
                return;
            }

            var restoreOpacity = path.Opacity;
            AnimateScale(scale, 0.88, duration);
            var fade = CreateDoubleAnimation(0.0, duration, new QuadraticEase { EasingMode = EasingMode.EaseIn });
            fade.Completed += (_, _) =>
            {
                if (generation != _visualGeneration)
                {
                    return;
                }

                path.BeginAnimation(OpacityProperty, null);
                path.Opacity = restoreOpacity;
                path.Visibility = Visibility.Hidden;
            };
            path.BeginAnimation(OpacityProperty, fade);
        }
    }

    private void SetExtendedContentVisibility(FrameworkElement content, bool visible, TimeSpan duration, int generation)
    {
        if (visible)
        {
            var targetOpacity = content.Opacity;
            content.BeginAnimation(OpacityProperty, null);
            content.Opacity = 0.0;
            content.Visibility = Visibility.Visible;
            content.BeginAnimation(
                OpacityProperty,
                CreateDoubleAnimation(0.0, targetOpacity, duration, new QuadraticEase { EasingMode = EasingMode.EaseOut }));
        }
        else
        {
            if (content.Visibility != Visibility.Visible)
            {
                return;
            }

            var restoreOpacity = content.Opacity;
            var fade = CreateDoubleAnimation(0.0, duration, new QuadraticEase { EasingMode = EasingMode.EaseIn });
            fade.Completed += (_, _) =>
            {
                if (generation != _visualGeneration)
                {
                    return;
                }

                content.BeginAnimation(OpacityProperty, null);
                content.Opacity = restoreOpacity;
                content.Visibility = Visibility.Hidden;
            };
            content.BeginAnimation(OpacityProperty, fade);
        }
    }

    private static Brush GetSectorFill(
        ClipboardEntry? entry,
        bool isSelected,
        Brush normal,
        Brush hover,
        Brush locked,
        Brush lockedHover,
        Brush copy,
        Brush copyHover,
        Brush empty)
    {
        if (entry is null)
        {
            return empty;
        }

        if (entry.IsQuickCopy)
        {
            return isSelected ? copyHover : copy;
        }

        if (entry.IsLocked)
        {
            return isSelected ? lockedHover : locked;
        }

        return isSelected ? hover : normal;
    }

    private static void AnimateScale(ScaleTransform transform, double target, TimeSpan duration)
    {
        var anim = CreateDoubleAnimation(target, duration, new QuadraticEase { EasingMode = EasingMode.EaseOut });
        transform.BeginAnimation(ScaleTransform.ScaleXProperty, anim);
        transform.BeginAnimation(ScaleTransform.ScaleYProperty, anim);
    }

    private static bool IsExtendedWheelAvailable(AppSettings settings, string shape)
    {
        return settings.Wheel.ExtendedWheel.Enabled
            && string.Equals(shape, WheelLayout.ShapeCircle, StringComparison.OrdinalIgnoreCase);
    }

    private static ImageSource? LoadShortcutIcon(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !System.IO.File.Exists(path))
        {
            return null;
        }

        var iconLocation = TryResolveShortcutIconLocation(path);
        return LoadIconFromLocation(iconLocation)
            ?? LoadShellIcon(ResolveShortcutIconPath(path))
            ?? LoadAssociatedIcon(ResolveShortcutIconPath(path))
            ?? LoadShellIcon(path)
            ?? LoadAssociatedIcon(path);
    }

    private static string ResolveShortcutIconPath(string shortcutPath)
    {
        var targetPath = TryResolveShortcutTarget(shortcutPath);
        return !string.IsNullOrWhiteSpace(targetPath)
            && (System.IO.File.Exists(targetPath) || System.IO.Directory.Exists(targetPath))
            ? targetPath
            : shortcutPath;
    }

    private static string? TryResolveShortcutTarget(string shortcutPath)
    {
        try
        {
            dynamic shortcut = CreateWScriptShortcut(shortcutPath);
            string? targetPath = shortcut.TargetPath;
            return targetPath;
        }
        catch
        {
            return null;
        }
    }

    private static string? TryResolveShortcutIconLocation(string shortcutPath)
    {
        try
        {
            dynamic shortcut = CreateWScriptShortcut(shortcutPath);
            string? iconLocation = shortcut.IconLocation;
            return iconLocation;
        }
        catch
        {
            return null;
        }
    }

    private static dynamic CreateWScriptShortcut(string shortcutPath)
    {
        var shellType = Type.GetTypeFromProgID("WScript.Shell");
        if (shellType is null)
        {
            throw new InvalidOperationException("WScript.Shell is unavailable.");
        }

        dynamic shell = Activator.CreateInstance(shellType)!;
        return shell.CreateShortcut(shortcutPath);
    }

    private static ImageSource? LoadIconFromLocation(string? iconLocation)
    {
        if (string.IsNullOrWhiteSpace(iconLocation))
        {
            return null;
        }

        var (iconPath, iconIndex) = ParseIconLocation(iconLocation);
        if (string.IsNullOrWhiteSpace(iconPath) || !System.IO.File.Exists(iconPath))
        {
            return null;
        }

        IntPtr[] largeIcons = new IntPtr[1];
        try
        {
            var count = ExtractIconEx(iconPath, iconIndex, largeIcons, null, 1);
            if (count == 0 || largeIcons[0] == IntPtr.Zero)
            {
                return null;
            }

            var source = Imaging.CreateBitmapSourceFromHIcon(
                largeIcons[0],
                Int32Rect.Empty,
                BitmapSizeOptions.FromWidthAndHeight(32, 32));
            source.Freeze();
            return source;
        }
        finally
        {
            if (largeIcons[0] != IntPtr.Zero)
            {
                DestroyIcon(largeIcons[0]);
            }
        }
    }

    private static (string Path, int Index) ParseIconLocation(string iconLocation)
    {
        var location = Environment.ExpandEnvironmentVariables(iconLocation.Trim().Trim('"'));
        var index = 0;
        var comma = location.LastIndexOf(',');
        if (comma > 0 && int.TryParse(location[(comma + 1)..], out var parsedIndex))
        {
            index = parsedIndex;
            location = location[..comma].Trim().Trim('"');
        }

        return (location, index);
    }

    private static ImageSource? LoadShellIcon(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        var info = new SHFILEINFO();
        var result = SHGetFileInfo(
            path,
            0,
            ref info,
            (uint)Marshal.SizeOf<SHFILEINFO>(),
            SHGFI_ICON | SHGFI_LARGEICON);
        if (result == IntPtr.Zero || info.hIcon == IntPtr.Zero)
        {
            return null;
        }

        try
        {
            var source = Imaging.CreateBitmapSourceFromHIcon(
                info.hIcon,
                Int32Rect.Empty,
                BitmapSizeOptions.FromWidthAndHeight(32, 32));
            source.Freeze();
            return source;
        }
        finally
        {
            DestroyIcon(info.hIcon);
        }
    }

    private static ImageSource? LoadAssociatedIcon(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !System.IO.File.Exists(path))
        {
            return null;
        }

        try
        {
            using var icon = System.Drawing.Icon.ExtractAssociatedIcon(path);
            if (icon is null)
            {
                return null;
            }

            var source = Imaging.CreateBitmapSourceFromHIcon(
                icon.Handle,
                Int32Rect.Empty,
                BitmapSizeOptions.FromWidthAndHeight(32, 32));
            source.Freeze();
            return source;
        }
        catch
        {
            return null;
        }
    }

    private static double GetExtendedRingThickness(double wheelRadius)
    {
        return Math.Max(ExtendedRingMinThickness, wheelRadius * 0.44);
    }

    private static (int Direction, double StartAngle, double EndAngle) GetExtendedSlotGeometry(int slotIndex, double gapDegrees)
    {
        var (direction, start, end) = slotIndex switch
        {
            0 => (0, -135, -105),
            1 => (0, -105, -75),
            2 => (0, -75, -45),
            3 => (1, -45, -15),
            4 => (1, -15, 15),
            5 => (1, 15, 45),
            6 => (2, 45, 75),
            7 => (2, 75, 105),
            8 => (2, 105, 135),
            9 => (3, 135, 165),
            10 => (3, 165, 195),
            _ => (3, 195, 225)
        };
        var gap = Math.Clamp(gapDegrees * 0.5, 0, 4);
        return (direction, start + gap, end - gap);
    }

    private static bool AngleInRange(double angle, double start, double end)
    {
        var normalizedAngle = NormalizeAngle360(angle);
        var normalizedStart = NormalizeAngle360(start);
        var normalizedEnd = NormalizeAngle360(end);
        return normalizedStart <= normalizedEnd
            ? normalizedAngle >= normalizedStart && normalizedAngle <= normalizedEnd
            : normalizedAngle >= normalizedStart || normalizedAngle <= normalizedEnd;
    }

    private static double NormalizeAngle360(double angle)
    {
        angle %= 360.0;
        return angle < 0 ? angle + 360.0 : angle;
    }

    private const uint SHGFI_ICON = 0x000000100;
    private const uint SHGFI_LARGEICON = 0x000000000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct SHFILEINFO
    {
        public IntPtr hIcon;
        public int iIcon;
        public uint dwAttributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szDisplayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)]
        public string szTypeName;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SHGetFileInfo(
        string pszPath,
        uint dwFileAttributes,
        ref SHFILEINFO psfi,
        uint cbFileInfo,
        uint uFlags);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern uint ExtractIconEx(
        string szFileName,
        int nIconIndex,
        IntPtr[]? phiconLarge,
        IntPtr[]? phiconSmall,
        uint nIcons);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyIcon(IntPtr hIcon);

    private void PlayShowAnimation(double targetOpacity, bool animationEnabled)
    {
        var center = new Point((_wheelRadius + _canvasPadding), (_wheelRadius + _canvasPadding));
        var scale = new ScaleTransform(animationEnabled ? 0.88 : 1.0, animationEnabled ? 0.88 : 1.0, center.X, center.Y);
        RootCanvas.RenderTransform = scale;

        if (!animationEnabled)
        {
            Opacity = targetOpacity;
            return;
        }

        var duration = TimeSpan.FromMilliseconds(ShowAnimMs);
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        scale.BeginAnimation(ScaleTransform.ScaleXProperty, CreateDoubleAnimation(1.0, duration, ease));
        scale.BeginAnimation(ScaleTransform.ScaleYProperty, CreateDoubleAnimation(1.0, duration, ease));
        BeginAnimation(OpacityProperty, CreateDoubleAnimation(targetOpacity, duration, ease));
    }

    private void PlayHideAnimation(int generation)
    {
        var center = new Point((_wheelRadius + _canvasPadding), (_wheelRadius + _canvasPadding));
        var scale = RootCanvas.RenderTransform as ScaleTransform
            ?? new ScaleTransform(1.0, 1.0, center.X, center.Y);
        RootCanvas.RenderTransform = scale;

        var duration = TimeSpan.FromMilliseconds(HideAnimMs);
        var ease = new CubicEase { EasingMode = EasingMode.EaseIn };
        scale.BeginAnimation(ScaleTransform.ScaleXProperty, CreateDoubleAnimation(0.94, duration, ease));
        scale.BeginAnimation(ScaleTransform.ScaleYProperty, CreateDoubleAnimation(0.94, duration, ease));

        var fade = CreateDoubleAnimation(0.0, duration, ease);
        fade.Completed += (_, _) =>
        {
            if (generation == _visualGeneration)
            {
                ClearAndHide();
            }
        };
        BeginAnimation(OpacityProperty, fade);
    }

    private void ClearAndHide()
    {
        BeginAnimation(OpacityProperty, null);
        RootCanvas.RenderTransform = null;
        Hide();
        RootCanvas.Children.Clear();
        _sectorPaths.Clear();
        _sectorScales.Clear();
        _sectorCentroids.Clear();
        _sectorHitGeometries.Clear();
    }

    private static DoubleAnimation CreateDoubleAnimation(double target, TimeSpan duration, IEasingFunction easing)
    {
        var anim = new DoubleAnimation
        {
            To = target,
            Duration = duration,
            EasingFunction = easing
        };
        Timeline.SetDesiredFrameRate(anim, AnimationFrameRate);
        return anim;
    }

    private static DoubleAnimation CreateDoubleAnimation(double from, double target, TimeSpan duration, IEasingFunction easing)
    {
        var anim = CreateDoubleAnimation(target, duration, easing);
        anim.From = from;
        return anim;
    }

    // ──────────────────────────────────────────────────────────────────
    // Geometry helpers
    // ──────────────────────────────────────────────────────────────────

    private double GetEffectiveDeadZoneRadius(AppSettings settings, string shape, int sectorCount)
    {
        var dead = settings.Wheel.InnerDeadZoneRadius;
        if (!string.Equals(shape, WheelLayout.ShapeRectangle, StringComparison.OrdinalIgnoreCase) || sectorCount <= 4)
        {
            return dead;
        }

        var diameter = (_wheelRadius + _canvasPadding) * 2;
        var innerWidth = diameter - _canvasPadding * 2 - SectorGapPixels * 2;
        var cellSize = innerWidth / 3.0;
        var maxDead = Math.Max(dead, cellSize / 2.0 - RectCenterCutoutGap);
        return Math.Min(dead * RectEightCenterDeadZoneScale, maxDead);
    }

    private static Geometry CreateRectangleSectorGeometry(Rect rect, Point center, double cutoutRadius, double cornerRadius)
    {
        var rectGeom = new RectangleGeometry(rect)
        {
            RadiusX = cornerRadius,
            RadiusY = cornerRadius,
        };

        var cutout = new EllipseGeometry(center, cutoutRadius, cutoutRadius);
        return new CombinedGeometry(GeometryCombineMode.Exclude, rectGeom, cutout);
    }

    private static Geometry CreateInsetRectangleSectorGeometry(Rect rect, Point center, double cutoutRadius, double cornerRadius)
    {
        var insetRect = rect;
        insetRect.Inflate(-ImageClipInsetPixels, -ImageClipInsetPixels);
        if (insetRect.Width <= 1 || insetRect.Height <= 1)
        {
            insetRect = rect;
        }

        var insetCorner = Math.Max(0, cornerRadius - ImageClipInsetPixels / 2);
        return CreateRectangleSectorGeometry(
            insetRect,
            center,
            cutoutRadius + ImageClipInsetPixels,
            insetCorner);
    }

    private static PathGeometry CreateInsetSectorGeometry(Point center, double innerRadius, double outerRadius, double startAngleDeg, double endAngleDeg, double cornerRadius)
    {
        var safeInnerRadius = innerRadius + ImageClipInsetPixels;
        var safeOuterRadius = outerRadius - ImageClipInsetPixels;
        if (safeOuterRadius <= safeInnerRadius + 1)
        {
            return CreateSectorGeometry(center, innerRadius, outerRadius, startAngleDeg, endAngleDeg, cornerRadius);
        }

        return CreateParallelInsetSectorGeometry(
            center,
            safeInnerRadius,
            safeOuterRadius,
            startAngleDeg,
            endAngleDeg,
            ImageClipInsetPixels);
    }

    private static PathGeometry CreateParallelInsetSectorGeometry(
        Point center,
        double innerRadius,
        double outerRadius,
        double startAngleDeg,
        double endAngleDeg,
        double sideInset)
    {
        if (outerRadius <= innerRadius + 1)
        {
            return CreateSharpSectorGeometry(center, innerRadius, outerRadius, startAngleDeg, endAngleDeg);
        }

        var inset = Math.Min(sideInset, Math.Max(0, innerRadius - 1));
        var startUnit = UnitOnCircle(startAngleDeg);
        var endUnit = UnitOnCircle(endAngleDeg);
        var startNormal = new Vector(-startUnit.Y, startUnit.X);
        var endNormal = new Vector(endUnit.Y, -endUnit.X);

        var outerStart = PointOnOffsetRadial(center, startUnit, startNormal, inset, outerRadius);
        var outerEnd = PointOnOffsetRadial(center, endUnit, endNormal, inset, outerRadius);
        var innerEnd = PointOnOffsetRadial(center, endUnit, endNormal, inset, innerRadius);
        var innerStart = PointOnOffsetRadial(center, startUnit, startNormal, inset, innerRadius);
        var sweep = Math.Abs(endAngleDeg - startAngleDeg);
        var largeArc = sweep > 180;

        var figure = new PathFigure { StartPoint = outerStart, IsClosed = true };
        figure.Segments.Add(new ArcSegment(outerEnd, new Size(outerRadius, outerRadius), 0, largeArc, SweepDirection.Clockwise, true));
        figure.Segments.Add(new LineSegment(innerEnd, true));
        figure.Segments.Add(new ArcSegment(innerStart, new Size(innerRadius, innerRadius), 0, largeArc, SweepDirection.Counterclockwise, true));
        figure.Segments.Add(new LineSegment(outerStart, true));
        return new PathGeometry(new[] { figure });
    }

    private static PathGeometry CreateSectorGeometry(Point center, double innerRadius, double outerRadius, double startAngleDeg, double endAngleDeg, double cornerRadius)
    {
        var sweepDeg = Math.Abs(endAngleDeg - startAngleDeg);
        var radialThickness = outerRadius - innerRadius;
        var corner = Math.Min(cornerRadius, Math.Max(0, radialThickness / 3));

        if (corner <= 0.1 || sweepDeg <= 2.0)
        {
            return CreateSharpSectorGeometry(center, innerRadius, outerRadius, startAngleDeg, endAngleDeg);
        }

        var maxAngleOffset = sweepDeg * 0.22;
        var outerAngleOffset = Math.Min(maxAngleOffset, corner / outerRadius * 180.0 / Math.PI);
        var innerAngleOffset = Math.Min(maxAngleOffset, corner / Math.Max(1.0, innerRadius) * 180.0 / Math.PI);

        if (outerAngleOffset <= 0.1 || innerAngleOffset <= 0.1)
        {
            return CreateSharpSectorGeometry(center, innerRadius, outerRadius, startAngleDeg, endAngleDeg);
        }

        var outerStart = PointOnCircle(center, outerRadius, startAngleDeg + outerAngleOffset);
        var outerEnd = PointOnCircle(center, outerRadius, endAngleDeg - outerAngleOffset);
        var innerEnd = PointOnCircle(center, innerRadius, endAngleDeg - innerAngleOffset);
        var innerStart = PointOnCircle(center, innerRadius, startAngleDeg + innerAngleOffset);

        var outerEndControl = PointOnCircle(center, outerRadius, endAngleDeg);
        var outerStartControl = PointOnCircle(center, outerRadius, startAngleDeg);
        var innerEndControl = PointOnCircle(center, innerRadius, endAngleDeg);
        var innerStartControl = PointOnCircle(center, innerRadius, startAngleDeg);

        var endOuterRadial = PointOnCircle(center, outerRadius - corner, endAngleDeg);
        var endInnerRadial = PointOnCircle(center, innerRadius + corner, endAngleDeg);
        var startInnerRadial = PointOnCircle(center, innerRadius + corner, startAngleDeg);
        var startOuterRadial = PointOnCircle(center, outerRadius - corner, startAngleDeg);

        var largeOuterArc = Math.Abs((endAngleDeg - outerAngleOffset) - (startAngleDeg + outerAngleOffset)) > 180;
        var largeInnerArc = Math.Abs((endAngleDeg - innerAngleOffset) - (startAngleDeg + innerAngleOffset)) > 180;

        var figure = new PathFigure { StartPoint = outerStart, IsClosed = true };
        figure.Segments.Add(new ArcSegment(outerEnd, new Size(outerRadius, outerRadius), 0, largeOuterArc, SweepDirection.Clockwise, true));
        figure.Segments.Add(new QuadraticBezierSegment(outerEndControl, endOuterRadial, true));
        figure.Segments.Add(new LineSegment(endInnerRadial, true));
        figure.Segments.Add(new QuadraticBezierSegment(innerEndControl, innerEnd, true));
        figure.Segments.Add(new ArcSegment(innerStart, new Size(innerRadius, innerRadius), 0, largeInnerArc, SweepDirection.Counterclockwise, true));
        figure.Segments.Add(new QuadraticBezierSegment(innerStartControl, startInnerRadial, true));
        figure.Segments.Add(new LineSegment(startOuterRadial, true));
        figure.Segments.Add(new QuadraticBezierSegment(outerStartControl, outerStart, true));

        return new PathGeometry(new[] { figure });
    }

    private static PathGeometry CreateSharpSectorGeometry(Point center, double innerRadius, double outerRadius, double startAngleDeg, double endAngleDeg)
    {
        var outerStart = PointOnCircle(center, outerRadius, startAngleDeg);
        var outerEnd = PointOnCircle(center, outerRadius, endAngleDeg);
        var innerEnd = PointOnCircle(center, innerRadius, endAngleDeg);
        var innerStart = PointOnCircle(center, innerRadius, startAngleDeg);
        var largeArc = Math.Abs(endAngleDeg - startAngleDeg) > 180;

        var figure = new PathFigure { StartPoint = outerStart, IsClosed = true };
        figure.Segments.Add(new ArcSegment(outerEnd, new Size(outerRadius, outerRadius), 0, largeArc, SweepDirection.Clockwise, true));
        figure.Segments.Add(new LineSegment(innerEnd, true));
        figure.Segments.Add(new ArcSegment(innerStart, new Size(innerRadius, innerRadius), 0, largeArc, SweepDirection.Counterclockwise, true));
        figure.Segments.Add(new LineSegment(outerStart, true));

        return new PathGeometry(new[] { figure });
    }

    private static Point PointOnCircle(Point center, double radius, double angleDeg)
    {
        var rad = angleDeg * Math.PI / 180.0;
        return new Point(center.X + radius * Math.Cos(rad), center.Y + radius * Math.Sin(rad));
    }

    private static Vector UnitOnCircle(double angleDeg)
    {
        var rad = angleDeg * Math.PI / 180.0;
        return new Vector(Math.Cos(rad), Math.Sin(rad));
    }

    private static Point PointOnOffsetRadial(Point center, Vector radialUnit, Vector inwardNormal, double sideInset, double radius)
    {
        var normalOffset = Math.Min(sideInset, Math.Max(0, radius - 1));
        var along = Math.Sqrt(Math.Max(1, radius * radius - normalOffset * normalOffset));
        return center + inwardNormal * normalOffset + radialUnit * along;
    }

    private static Point CircleSectorCentroid(Point center, double innerRadius, double outerRadius, double startAngleDeg, double endAngleDeg)
    {
        // Approximate centroid: angular midpoint, radius midpoint. Used as the
        // ScaleTransform origin so expansion looks like the sector "puffs out"
        // around its visible arc centre.
        var midAngle = (startAngleDeg + endAngleDeg) / 2.0;
        var midRadius = (innerRadius + outerRadius) / 2.0;
        return PointOnCircle(center, midRadius, midAngle);
    }

    private static Brush BrushFromHex(string hex)
    {
        try
        {
            return new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
        }
        catch
        {
            return Brushes.Gray;
        }
    }

    private (double scaleX, double scaleY) GetDeviceToDipScale()
    {
        var source = PresentationSource.FromVisual(this);
        if (source?.CompositionTarget is null)
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            return (1.0 / dpi.DpiScaleX, 1.0 / dpi.DpiScaleY);
        }
        var m = source.CompositionTarget.TransformFromDevice;
        return (m.M11, m.M22);
    }

    private void ApplyNoActivateStyle()
    {
        var helper = new WindowInteropHelper(this);
        var hwnd = helper.Handle;
        var exStyle = NativeMethods.GetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE);
        NativeMethods.SetWindowLong(hwnd, NativeMethods.GWL_EXSTYLE, exStyle | NativeMethods.WS_EX_NOACTIVATE | NativeMethods.WS_EX_TOOLWINDOW | NativeMethods.WS_EX_TRANSPARENT);
    }

    private void ReassertTopmost()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero)
        {
            return;
        }

        try
        {
            Topmost = true;
            _ = NativeMethods.SetWindowPos(
                hwnd,
                NativeMethods.HWND_TOPMOST,
                0,
                0,
                0,
                0,
                NativeMethods.SWP_NOMOVE
                    | NativeMethods.SWP_NOSIZE
                    | NativeMethods.SWP_NOACTIVATE
                    | NativeMethods.SWP_NOOWNERZORDER);
        }
        catch (Exception)
        {
            // WPF Topmost remains the compatibility fallback on older shells
            // or unusual host windows where the Win32 enhancement fails.
            Topmost = true;
        }
    }
}
