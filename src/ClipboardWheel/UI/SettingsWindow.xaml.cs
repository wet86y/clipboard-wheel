using System.Diagnostics;
using System.Net.Http;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Navigation;
using ClipboardWheel.Models;
using ClipboardWheel.Services;
using DesktopUpdateKit;
using Microsoft.Win32;
using WpfDragEventArgs = System.Windows.DragEventArgs;
using WpfKeyEventArgs = System.Windows.Input.KeyEventArgs;
using WpfMessageBox = System.Windows.MessageBox;

namespace ClipboardWheel.UI;

public partial class SettingsWindow : Window
{
    private const int DefaultCircleSectorCount = 6;
    private const int DefaultRectangleSectorCount = 8;
    private const double PreviewCanvasSize = 360;
    private const double PreviewOuterMargin = 16;
    private const double PreviewBaseSectorScale = 0.9;
    private const double PreviewExtendedRingGap = 4;
    private const double PreviewExtendedRingMinThickness = 52;
    private const double PreviewSectorCornerRadius = 10;

    private readonly SettingsService _settingsService;
    private readonly List<Path> _previewSlotPaths = new();
    private AppSettings _settings;
    private int _selectedExtendedSlotIndex;
    private bool _loadingUi;
    private bool _recordingHotkey;
    private bool _checkingForUpdate;
    private UpdateRelease? _availableUpdate;
    private readonly List<Key> _recordedHotkeyKeys = new();
    private static readonly UpdateClientOptions UpdateOptions = new(
        ApplicationId: "clipboard-wheel",
        Repository: "wet86y/clipboard-wheel",
        ExeAssetName: "super-middle-key.exe",
        Sha256AssetName: "super-middle-key.exe.sha256",
        TempDirectoryName: "SuperMiddleKey-update");
    private static readonly UpdateClient SharedUpdateClient = new(UpdateOptions);
    private static readonly UpdateDownloadSession SharedUpdateSession = new(SharedUpdateClient);
    private readonly UpdateLauncher _updateLauncher = new();
    private CancellationTokenSource? _shortcutDropHelperCancellation;

    public SettingsWindow(SettingsService settingsService)
    {
        _settingsService = settingsService;
        _settings = CloneSettings(_settingsService.Current);
        InitializeComponent();
        PreviewKeyDown += SettingsWindow_PreviewKeyDown;
        PreviewKeyUp += SettingsWindow_PreviewKeyUp;
        PreviewTextInput += SettingsWindow_PreviewTextInput;
        LoadSettingsToUi();
        HookValueTexts();
        AboutUpdateStatusText.Text = "点击“检查更新”获取 GitHub Releases 中的最新版本。";
        AboutVersionText.Text = ApplicationVersion.CurrentText;
        SharedUpdateSession.Changed += SharedUpdateSession_Changed;
        ApplySharedUpdateSession(SharedUpdateSession.Snapshot);
    }

    private static AppSettings CloneSettings(AppSettings source)
    {
        var json = JsonSerializer.Serialize(source);
        return JsonSerializer.Deserialize<AppSettings>(json) ?? new AppSettings();
    }

    private void LoadSettingsToUi()
    {
        _loadingUi = true;
        var shape = WheelLayout.NormalizeShape(_settings.Wheel.Shape);
        if (shape == WheelLayout.ShapeRectangle)
        {
            ShapeRectangleRadio.IsChecked = true;
        }
        else
        {
            ShapeCircleRadio.IsChecked = true;
        }
        RefreshSectorCountItems(shape);
        SelectComboValue(SectorCountBox, _settings.Wheel.SectorCount.ToString());

        RadiusSlider.Value = _settings.Wheel.Radius;
        DeadZoneSlider.Value = _settings.Wheel.InnerDeadZoneRadius;
        OpacitySlider.Value = _settings.Wheel.Opacity;
        QuickCopyToggle.IsChecked = _settings.Wheel.QuickCopy;
        CaptureImagesToggle.IsChecked = _settings.Clipboard.CaptureImages;
        AutoStartToggle.IsChecked = _settings.AutoStartEnabled;
        AdministratorModeToggle.IsChecked = _settings.RunAsAdministratorEnabled;
        ExtendedWheelToggle.IsChecked = _settings.Wheel.ExtendedWheel.Enabled;
        AboutAccelerationToggle.IsChecked = _settings.Update.UseAccelerationNodes;
        BreakoutBufferSlider.Value = _settings.Wheel.ExtendedWheel.BreakoutBufferPixels;

        LoadExtendedSlotToUi(0);
        UpdateValueTexts();
        UpdateSwitchTexts();
        _loadingUi = false;
        DrawExtendedWheelPreview();
    }

    private void Shape_Changed(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton rb || rb.IsChecked != true) return;

        var shape = rb == ShapeRectangleRadio
            ? WheelLayout.ShapeRectangle
            : WheelLayout.ShapeCircle;
        RefreshSectorCountItems(shape);
        SelectComboValue(SectorCountBox, GetDefaultSectorCountForShape(shape).ToString());
        if (!_loadingUi)
        {
            DrawExtendedWheelPreview();
        }
    }

    private void WheelPreviewSource_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_loadingUi) return;
        DrawExtendedWheelPreview();
    }

    private void RefreshSectorCountItems(string shape)
    {
        SectorCountBox.Items.Clear();
        foreach (var t in WheelLayout.GetTiersForShape(shape))
        {
            SectorCountBox.Items.Add(new ComboBoxItem { Content = t.ToString() });
        }
    }

    private static int GetDefaultSectorCountForShape(string shape)
    {
        return string.Equals(shape, WheelLayout.ShapeRectangle, StringComparison.OrdinalIgnoreCase)
            ? DefaultRectangleSectorCount
            : DefaultCircleSectorCount;
    }

    private void HookValueTexts()
    {
        RadiusSlider.ValueChanged += (_, _) =>
        {
            UpdateValueTexts();
            DrawExtendedWheelPreview();
        };
        DeadZoneSlider.ValueChanged += (_, _) =>
        {
            UpdateValueTexts();
            DrawExtendedWheelPreview();
        };
        OpacitySlider.ValueChanged += (_, _) => UpdateValueTexts();
        BreakoutBufferSlider.ValueChanged += (_, _) => UpdateValueTexts();
    }

    private void UpdateValueTexts()
    {
        if (RadiusText is null) return;
        RadiusText.Text = $"当前：{RadiusSlider.Value:0}";
        DeadZoneText.Text = $"当前：{DeadZoneSlider.Value:0}";
        OpacityText.Text = $"当前：{OpacitySlider.Value:0.00}";
        BreakoutBufferText.Text = $"当前：{BreakoutBufferSlider.Value:0}px";
    }

    private void Switch_Changed(object sender, RoutedEventArgs e)
    {
        if (_loadingUi) return;
        UpdateSwitchTexts();
        if (sender is System.Windows.Controls.Primitives.ToggleButton toggle)
        {
            AnimateSwitchToggle(toggle);
        }
    }

    private void UpdateSwitchTexts()
    {
        SetSwitchText(QuickCopyToggle);
        SetSwitchText(CaptureImagesToggle);
        SetSwitchText(AutoStartToggle);
        SetSwitchText(AdministratorModeToggle);
        SetSwitchText(ExtendedWheelToggle);
        SetSwitchText(AboutAccelerationToggle);
    }

    private static void SetSwitchText(System.Windows.Controls.Primitives.ToggleButton toggle)
    {
        toggle.Content = toggle.IsChecked == true ? "开" : "关";
    }

    private static void AnimateSwitchToggle(System.Windows.Controls.Primitives.ToggleButton toggle)
    {
        toggle.ApplyTemplate();
        if (toggle.Template.FindName("Thumb", toggle) is not Ellipse thumb)
        {
            return;
        }

        var isOn = toggle.IsChecked == true;
        var animation = new DoubleAnimation
        {
            From = isOn ? 2 : 28,
            To = isOn ? 28 : 2,
            Duration = TimeSpan.FromMilliseconds(110),
            EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut }
        };
        thumb.BeginAnimation(Canvas.LeftProperty, animation);
    }

    private void DrawExtendedWheelPreview()
    {
        if (ExtendedWheelPreviewCanvas is null) return;

        SaveExtendedSlotFromUi();
        ExtendedWheelPreviewCanvas.Children.Clear();
        _previewSlotPaths.Clear();

        var radius = Math.Max(100, RadiusSlider.Value);
        var dead = Math.Clamp(DeadZoneSlider.Value, 0, radius - 20);
        var ringThickness = GetPreviewExtendedRingThickness(radius);
        var contentRadius = radius + PreviewExtendedRingGap + ringThickness;
        var scale = (PreviewCanvasSize / 2 - PreviewOuterMargin) / contentRadius;
        var center = new Point(PreviewCanvasSize / 2, PreviewCanvasSize / 2);
        var scaledRadius = radius * scale;
        var scaledDead = dead * scale;
        var scaledRingGap = PreviewExtendedRingGap * scale;
        var scaledRingThickness = ringThickness * scale;
        var shape = ShapeRectangleRadio.IsChecked == true ? WheelLayout.ShapeRectangle : WheelLayout.ShapeCircle;
        var sectorCount = shape == WheelLayout.ShapeCircle
            ? WheelLayout.NormalizeSectorCount(WheelLayout.ShapeCircle, int.Parse(GetComboValue(SectorCountBox, DefaultCircleSectorCount.ToString())))
            : DefaultCircleSectorCount;

        DrawPreviewInnerWheel(center, scaledDead, scaledRadius, sectorCount, shape == WheelLayout.ShapeCircle);
        DrawPreviewExtendedRing(center, scaledRadius + scaledRingGap, scaledRadius + scaledRingGap + scaledRingThickness);
        DrawPreviewCenterDisk(center, Math.Max(10, scaledDead));

        ExtendedPreviewHintText.Text = shape == WheelLayout.ShapeCircle
            ? "只可选择外圈扩展动作槽位；预览随当前轮盘大小、死区和扇区数缩放。"
            : "矩形模式下不触发突破轮盘；这里仍可提前配置圆形突破槽位。";
    }

    private void DrawPreviewInnerWheel(Point center, double innerRadius, double outerRadius, int sectorCount, bool enabled)
    {
        var sectorAngle = 360.0 / sectorCount;
        var fill = enabled ? BrushFromHex("#E5E5E5") : BrushFromHex("#EEEEEE");
        for (var i = 0; i < sectorCount; i++)
        {
            var start = -90 + i * sectorAngle;
            var end = -90 + (i + 1) * sectorAngle;
            var geometry = CreateSectorGeometry(center, innerRadius, outerRadius, start, end, PreviewSectorCornerRadius);
            var path = new Path
            {
                Data = geometry,
                Fill = fill,
                Stroke = null,
                Opacity = enabled ? 1.0 : 0.55,
                IsHitTestVisible = false
            };
            var centroid = CircleSectorCentroid(center, innerRadius, outerRadius, start, end);
            path.RenderTransform = new ScaleTransform(PreviewBaseSectorScale, PreviewBaseSectorScale, centroid.X, centroid.Y);
            ExtendedWheelPreviewCanvas.Children.Add(path);
        }
    }

    private void DrawPreviewExtendedRing(Point center, double innerRadius, double outerRadius)
    {
        var contentElements = new List<FrameworkElement>();
        for (var i = 0; i < ExtendedWheelSettings.SlotCount; i++)
        {
            var slot = _settings.Wheel.ExtendedWheel.Slots[i];
            var (_, start, end) = GetExtendedSlotGeometry(i, _settings.Wheel.SectorGapDegrees);
            var isSelected = i == _selectedExtendedSlotIndex;
            var geometry = CreateSectorGeometry(center, innerRadius, outerRadius, start, end, PreviewSectorCornerRadius);
            var path = new Path
            {
                Data = geometry,
                Fill = isSelected
                    ? BrushFromHex(_settings.Theme.SectorHoverColor)
                    : slot.IsConfigured
                        ? BrushFromHex("#314A68")
                        : BrushFromHex("#D8D8D8"),
                Stroke = null,
                Opacity = isSelected ? 0.95 : slot.IsConfigured ? 0.82 : 0.52,
                Tag = i,
                Cursor = System.Windows.Input.Cursors.Hand
            };
            path.MouseLeftButtonDown += PreviewSlotPath_MouseLeftButtonDown;
            ExtendedWheelPreviewCanvas.Children.Add(path);
            _previewSlotPaths.Add(path);

            var content = CreatePreviewExtendedSlotContent(slot, center, geometry, innerRadius, outerRadius, start, end);
            if (content is not null)
            {
                content.IsHitTestVisible = false;
                contentElements.Add(content);
            }
        }

        foreach (var content in contentElements)
        {
            ExtendedWheelPreviewCanvas.Children.Add(content);
        }
    }

    private FrameworkElement? CreatePreviewExtendedSlotContent(
        ExtendedWheelActionSlot slot,
        Point wheelCenter,
        Geometry geometry,
        double innerRadius,
        double outerRadius,
        double start,
        double end)
    {
        if (!slot.IsConfigured)
        {
            return null;
        }

        var bounds = geometry.Bounds;
        if (bounds.IsEmpty || bounds.Width < 28 || bounds.Height < 18)
        {
            return null;
        }

        var center = GetPreviewExtendedContentCenter(wheelCenter, innerRadius, outerRadius, start, end);
        var shortcutMode = string.Equals(slot.Mode, ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase);
        var text = TruncatePreviewSlotText(slot.DisplayName);
        var textBrush = Brushes.White;
        var textWidth = Math.Min(58, Math.Max(30, bounds.Width * 0.64));
        var fontSize = Math.Clamp(bounds.Height * 0.15, 6.5, 9.0);
        var textHeight = Math.Clamp(fontSize + 6, 12, 17);
        var canShowIcon = shortcutMode && bounds.Width >= 44 && bounds.Height >= 36;

        FrameworkElement content;
        if (canShowIcon)
        {
            var iconSize = Math.Clamp(Math.Min(bounds.Height * 0.34, bounds.Width * 0.28), 12, 24);
            var panel = new StackPanel
            {
                Width = textWidth,
                Height = iconSize + textHeight + 2,
                Orientation = System.Windows.Controls.Orientation.Vertical,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };
            panel.Children.Add(new System.Windows.Controls.Image
            {
                Width = iconSize,
                Height = iconSize,
                Stretch = Stretch.Uniform,
                Source = LoadShortcutIcon(slot.ShortcutPath),
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                Margin = new Thickness(0, 0, 0, 1)
            });
            panel.Children.Add(new TextBlock
            {
                Text = text,
                Foreground = textBrush,
                FontSize = fontSize,
                FontWeight = FontWeights.SemiBold,
                Width = textWidth,
                Height = textHeight,
                TextAlignment = TextAlignment.Center,
                TextWrapping = TextWrapping.NoWrap,
                TextTrimming = TextTrimming.CharacterEllipsis
            });
            content = panel;
        }
        else
        {
            content = new TextBlock
            {
                Text = text,
                Foreground = textBrush,
                FontSize = Math.Clamp(bounds.Height * 0.16, 7, 9.5),
                FontWeight = FontWeights.SemiBold,
                Width = textWidth,
                Height = Math.Min(30, Math.Max(15, bounds.Height * 0.36)),
                TextAlignment = TextAlignment.Center,
                TextWrapping = TextWrapping.Wrap,
                TextTrimming = TextTrimming.CharacterEllipsis,
                VerticalAlignment = VerticalAlignment.Center
            };
        }

        Canvas.SetLeft(content, center.X - content.Width / 2);
        Canvas.SetTop(content, center.Y - content.Height / 2);
        return content;
    }

    private static Point GetPreviewExtendedContentCenter(
        Point wheelCenter,
        double inner,
        double outer,
        double start,
        double end)
    {
        var thickness = Math.Max(1, outer - inner);
        return PointOnCircle(wheelCenter, inner + thickness / 2.0, (start + end) / 2.0);
    }

    private static string TruncatePreviewSlotText(string text)
    {
        text = text.Trim();
        return text.Length <= 8 ? text : text[..8];
    }

    private void DrawPreviewCenterDisk(Point center, double radius)
    {
        var disk = new Ellipse
        {
            Width = radius * 2,
            Height = radius * 2,
            Fill = BrushFromHex("#383838"),
            IsHitTestVisible = false
        };
        Canvas.SetLeft(disk, center.X - radius);
        Canvas.SetTop(disk, center.Y - radius);
        ExtendedWheelPreviewCanvas.Children.Add(disk);
    }

    private void PreviewSlotPath_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is Path { Tag: int index })
        {
            SaveExtendedSlotFromUi();
            LoadExtendedSlotToUi(index);
            DrawExtendedWheelPreview();
            e.Handled = true;
        }
    }

    private void ExtendedWheelPreviewCanvas_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        e.Handled = true;
    }

    private void LoadExtendedSlotToUi(int index)
    {
        var wasLoading = _loadingUi;
        _loadingUi = true;
        _recordingHotkey = false;
        _recordedHotkeyKeys.Clear();
        _selectedExtendedSlotIndex = Math.Clamp(index, 0, ExtendedWheelSettings.SlotCount - 1);
        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        SelectedSlotTitle.Text = $"槽位 {GetSlotPositionLabel(_selectedExtendedSlotIndex)}";
        SlotNameBox.Text = slot.Name;
        BrowserLaunchUrlBox.Text = slot.BrowserLaunchUrl;
        SelectComboTag(SlotModeBox, NormalizeUiMode(slot.Mode));
        SelectComboTag(SlotSecondBehaviorBox, slot.SecondTriggerBehavior);
        _loadingUi = wasLoading;
        UpdateSlotEditor();
    }

    private void SaveExtendedSlotFromUi()
    {
        if (SlotModeBox is null || _settings.Wheel.ExtendedWheel.Slots.Count == 0)
        {
            return;
        }

        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        slot.Name = SlotNameBox.Text.Trim();
        slot.Mode = GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey);
        slot.SecondTriggerBehavior = GetComboTag(SlotSecondBehaviorBox, ExtendedWheelSecondTriggerBehavior.Minimize);
        slot.BrowserLaunchUrl = BrowserLaunchUrlBox.Text.Trim();
        slot.Enabled = slot.Mode switch
        {
            ExtendedWheelActionMode.Hotkey => !string.IsNullOrWhiteSpace(slot.Hotkey),
            ExtendedWheelActionMode.Shortcut => !string.IsNullOrWhiteSpace(slot.ShortcutPath),
            _ => false
        };
    }

    private void SlotModeBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loadingUi) return;
        _recordingHotkey = false;
        _recordedHotkeyKeys.Clear();
        SaveExtendedSlotFromUi();
        UpdateSlotEditor();
        DrawExtendedWheelPreview();
    }

    private void SlotEditor_Changed(object sender, TextChangedEventArgs e)
    {
        if (_loadingUi) return;
        SaveExtendedSlotFromUi();
        DrawExtendedWheelPreview();
    }

    private void UpdateSlotEditor()
    {
        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        var mode = GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey);
        var hotkeyMode = string.Equals(mode, ExtendedWheelActionMode.Hotkey, StringComparison.OrdinalIgnoreCase);
        var browserShortcut = !hotkeyMode && IsBrowserShortcut(slot.ShortcutPath);
        ShortcutBehaviorPanel.Visibility = hotkeyMode ? Visibility.Collapsed : Visibility.Visible;
        BrowserLaunchUrlPanel.Visibility = browserShortcut ? Visibility.Visible : Visibility.Collapsed;
        ShortcutIconImage.Visibility = hotkeyMode ? Visibility.Collapsed : Visibility.Visible;

        if (hotkeyMode)
        {
            SlotActionButton.Content = _recordingHotkey
                ? "结束录制"
                : string.IsNullOrWhiteSpace(slot.Hotkey)
                    ? "录制"
                    : "清除";
            SlotValueText.Text = _recordingHotkey
                ? BuildRecordedHotkeyText(_recordedHotkeyKeys, "录制中，请按下组合键")
                : string.IsNullOrWhiteSpace(slot.Hotkey)
                    ? "键位显示"
                    : slot.Hotkey;
            SlotValuePanel.AllowDrop = false;
            ShortcutIconImage.Source = null;
        }
        else
        {
            SlotActionButton.Content = string.IsNullOrWhiteSpace(slot.ShortcutPath) ? "选择" : "清除";
            SlotValueText.Text = string.IsNullOrWhiteSpace(slot.ShortcutPath)
                ? ElevationService.IsAdministrator()
                    ? "点击打开普通权限拖放窗口，或点“选择”"
                    : "拖入 .lnk 快捷方式"
                : System.IO.Path.GetFileNameWithoutExtension(slot.ShortcutPath);
            SlotValuePanel.AllowDrop = string.IsNullOrWhiteSpace(slot.ShortcutPath);
            ShortcutIconImage.Source = LoadShortcutIcon(slot.ShortcutPath);
            ShortcutIconImage.Visibility = ShortcutIconImage.Source is null ? Visibility.Collapsed : Visibility.Visible;
        }
    }

    private void SlotActionButton_Click(object sender, RoutedEventArgs e)
    {
        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        var mode = GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey);
        if (string.Equals(mode, ExtendedWheelActionMode.Hotkey, StringComparison.OrdinalIgnoreCase))
        {
            if (_recordingHotkey)
            {
                slot.Hotkey = BuildRecordedHotkeyText(_recordedHotkeyKeys, string.Empty);
                _recordingHotkey = false;
                _recordedHotkeyKeys.Clear();
            }
            else if (!string.IsNullOrWhiteSpace(slot.Hotkey))
            {
                slot.Hotkey = string.Empty;
            }
            else
            {
                _recordingHotkey = true;
                _recordedHotkeyKeys.Clear();
                Keyboard.Focus(this);
            }
        }
        else
        {
            if (!string.IsNullOrWhiteSpace(slot.ShortcutPath))
            {
                slot.ShortcutPath = string.Empty;
                slot.BrowserLaunchUrl = string.Empty;
                // SaveExtendedSlotFromUi reads the editor fields. Clear the
                // hidden browser field as well so an old URL cannot be written
                // back into the now-unbound slot.
                BrowserLaunchUrlBox.Text = string.Empty;
            }
            else
            {
                ChooseShortcutFile(slot);
            }
        }

        SaveExtendedSlotFromUi();
        UpdateSlotEditor();
        DrawExtendedWheelPreview();
    }

    private void SettingsWindow_PreviewKeyDown(object sender, WpfKeyEventArgs e)
    {
        if (!_recordingHotkey)
        {
            return;
        }

        e.Handled = true;
        var key = NormalizeRecordedKey(e);
        if (key == Key.None || key == Key.Clear)
        {
            return;
        }

        AddRecordedKey(key);
        UpdateSlotEditor();
    }

    private void SettingsWindow_PreviewTextInput(object sender, TextCompositionEventArgs e)
    {
        if (_recordingHotkey)
        {
            e.Handled = true;
        }
    }

    private void SettingsWindow_PreviewKeyUp(object sender, WpfKeyEventArgs e)
    {
        if (_recordingHotkey)
        {
            e.Handled = true;
        }
    }

    private void ShortcutDrop_DragOver(object sender, WpfDragEventArgs e)
    {
        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        var shortcutMode = string.Equals(GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey), ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase);
        var canDrop = shortcutMode
            && string.IsNullOrWhiteSpace(slot.ShortcutPath)
            && TryGetShortcutDropPath(e, out _);
        e.Effects = canDrop ? System.Windows.DragDropEffects.Copy : System.Windows.DragDropEffects.None;
        e.Handled = true;
    }

    private void ShortcutDrop_Drop(object sender, WpfDragEventArgs e)
    {
        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        PasteTrace.Mark(
            $"ShortcutDrop_wpf_received slot={_selectedExtendedSlotIndex} " +
            $"configured={!string.IsNullOrWhiteSpace(slot.ShortcutPath)}");
        if (!string.Equals(GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey), ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase)
            || !string.IsNullOrWhiteSpace(slot.ShortcutPath)
            || !TryGetShortcutDropPath(e, out var path))
        {
            PasteTrace.Mark($"ShortcutDrop_wpf_rejected slot={_selectedExtendedSlotIndex}");
            e.Handled = true;
            return;
        }

        slot.ShortcutPath = path;
        PasteTrace.Mark(
            $"ShortcutDrop_wpf_accepted slot={_selectedExtendedSlotIndex} " +
            $"file={System.IO.Path.GetFileName(path)}");
        SaveExtendedSlotFromUi();
        UpdateSlotEditor();
        DrawExtendedWheelPreview();
        e.Handled = true;
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        var previousRunAsAdministrator = _settingsService.Current.RunAsAdministratorEnabled;
        var requestedRunAsAdministrator = AdministratorModeToggle.IsChecked == true;

        SaveExtendedSlotFromUi();
        _settings.Wheel.Shape = ShapeRectangleRadio.IsChecked == true
            ? WheelLayout.ShapeRectangle
            : WheelLayout.ShapeCircle;
        _settings.Wheel.SectorCount = WheelLayout.NormalizeSectorCount(
            _settings.Wheel.Shape,
            int.Parse(GetComboValue(SectorCountBox, "8")));
        _settings.Wheel.Radius = RadiusSlider.Value;
        _settings.Wheel.InnerDeadZoneRadius = DeadZoneSlider.Value;
        _settings.Wheel.Opacity = OpacitySlider.Value;
        _settings.Wheel.QuickCopy = QuickCopyToggle.IsChecked == true;
        _settings.Wheel.ExtendedWheel.Enabled = ExtendedWheelToggle.IsChecked == true;
        _settings.Wheel.ExtendedWheel.BreakoutBufferPixels = BreakoutBufferSlider.Value;

        _settings.Clipboard.LoadWindowsClipboardHistoryOnStartup = true;
        _settings.Clipboard.CapturePlainText = true;
        _settings.Clipboard.CaptureHtml = true;
        _settings.Clipboard.CaptureRtf = true;
        _settings.Clipboard.CaptureCsv = true;
        _settings.Clipboard.CaptureImages = CaptureImagesToggle.IsChecked == true;
        _settings.Clipboard.IgnorePasswordLikeText = false;
        _settings.Clipboard.MaxHistoryItems = 8;

        _settings.Paste.DefaultMode = "smart";
        _settings.Paste.RestoreClipboardAfterPaste = false;
        _settings.Paste.RestoreDelayMs = 150;
        _settings.Paste.AddPasteToClipboardHistory = false;

        _settings.Mouse.DefaultCaptureMode = "always";
        _settings.ProcessRules.Clear();
        _settings.ProcessRules["default"] = _settings.Mouse.DefaultCaptureMode;

        _settings.AutoStartEnabled = AutoStartToggle.IsChecked == true;
        _settings.RunAsAdministratorEnabled = requestedRunAsAdministrator;
        if (!AutoStartService.TryApply(_settings.AutoStartEnabled, out var autoStartError))
        {
            System.Windows.MessageBox.Show(
                this,
                $"无法更新开机自启动设置。{Environment.NewLine}{autoStartError}",
                "超级中键",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }

        _settingsService.Save(_settings);

        if (requestedRunAsAdministrator != ElevationService.IsAdministrator())
        {
            if (ElevationService.TryRestart(requestedRunAsAdministrator, out var restartError))
            {
                Application.Current.Shutdown();
                return;
            }

            _settings.RunAsAdministratorEnabled = previousRunAsAdministrator;
            _settingsService.Save(_settings);
            _loadingUi = true;
            AdministratorModeToggle.IsChecked = previousRunAsAdministrator;
            _loadingUi = false;
            UpdateSwitchTexts();
            AdminRestartStatusText.Text = restartError ?? "权限模式切换失败，当前模式保持不变。";
            return;
        }

        Close();
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    private void AdministratorModeToggle_Changed(object sender, RoutedEventArgs e)
    {
        if (_loadingUi || sender is not System.Windows.Controls.Primitives.ToggleButton toggle)
        {
            return;
        }

        UpdateSwitchTexts();
        AnimateSwitchToggle(toggle);
        AdminRestartStatusText.Text = "点击“保存”后应用此模式。";
    }

    private void GitHubHistory_Click(object sender, RequestNavigateEventArgs e)
    {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
        e.Handled = true;
    }

    private async void CheckUpdate_Click(object sender, RoutedEventArgs e)
    {
        var session = SharedUpdateSession.Snapshot;
        if (_checkingForUpdate || session.State is UpdateDownloadSessionState.Downloading or UpdateDownloadSessionState.Paused)
        {
            return;
        }

        _checkingForUpdate = true;
        AboutCheckUpdateButton.IsEnabled = false;
        UpdateProgressBar.Visibility = Visibility.Collapsed;
        UpdateProgressBar.Value = 0;
        UpdateProgressBar.IsIndeterminate = false;
        _availableUpdate = null;
        AboutAccelerationOptionPanel.Visibility = Visibility.Collapsed;
        AboutInstallUpdateButton.Visibility = Visibility.Collapsed;
        AboutReleaseNotesBorder.Visibility = Visibility.Collapsed;
        AboutUpdateStatusText.Text = "正在检查更新...";

        try
        {
            var release = await SharedUpdateClient.CheckForUpdateAsync();
            if (release is null)
            {
                AboutUpdateStatusText.Text = $"当前版本 {ApplicationVersion.CurrentText}，已是最新版本";
                return;
            }

            ShowAvailableUpdate(release);
        }
        catch (Exception ex)
        {
            AboutUpdateStatusText.Text = GetUpdateErrorText(ex);
        }
        finally
        {
            _checkingForUpdate = false;
            AboutCheckUpdateButton.IsEnabled = true;
        }
    }

    private async void InstallUpdate_Click(object sender, RoutedEventArgs e)
    {
        var session = SharedUpdateSession.Snapshot;
        if (session.State == UpdateDownloadSessionState.Completed && !string.IsNullOrWhiteSpace(session.DownloadedPath))
        {
            AboutInstallUpdateButton.IsEnabled = false;
            AboutUpdateStatusText.Text = "下载和校验完成，正在启动更新助手...";
            try
            {
                await _updateLauncher.LaunchAsync(session.DownloadedPath);
                AboutUpdateStatusText.Text = "更新助手已启动，程序即将退出。";
                Application.Current.Shutdown();
            }
            catch (Exception exception)
            {
                AboutInstallUpdateButton.IsEnabled = true;
                AboutUpdateStatusText.Text = GetUpdateErrorText(exception);
            }

            return;
        }

        if (session.State == UpdateDownloadSessionState.Paused)
        {
            SharedUpdateSession.Resume();
            return;
        }

        if (session.State == UpdateDownloadSessionState.Downloading || _availableUpdate is null)
        {
            return;
        }

        if (!SharedUpdateSession.TryStart(_availableUpdate, _settings.Update.UseAccelerationNodes))
        {
            AboutUpdateStatusText.Text = "已有更新下载任务正在运行。";
        }
    }

    private void PauseResumeDownload_Click(object sender, RoutedEventArgs e)
    {
        if (SharedUpdateSession.Snapshot.State == UpdateDownloadSessionState.Paused)
        {
            SharedUpdateSession.Resume();
            return;
        }

        SharedUpdateSession.Pause();
    }

    private void BackgroundDownload_Click(object sender, RoutedEventArgs e)
    {
        if (SharedUpdateSession.ContinueInBackground())
        {
            Close();
        }
    }

    private void AccelerationToggle_Changed(object sender, RoutedEventArgs e)
    {
        if (_loadingUi)
        {
            return;
        }

        var enabled = AboutAccelerationToggle.IsChecked == true;
        _settings.Update.UseAccelerationNodes = enabled;
        _settingsService.Save(_settings);
        UpdateSwitchTexts();
        if (sender is System.Windows.Controls.Primitives.ToggleButton toggle)
        {
            AnimateSwitchToggle(toggle);
        }
        if (SharedUpdateSession.SetUseAccelerationNodes(enabled))
        {
            AboutUpdateStatusText.Text = enabled
                ? "已启用加速节点，当前下载将切换到加速节点。"
                : "已关闭加速节点，当前下载将切换到 GitHub 官方直连。";
        }
    }

    private void SwitchUpdateNode_Click(object sender, RoutedEventArgs e)
    {
        if (SharedUpdateSession.RequestNextAcceleratedNode())
        {
            AboutSwitchNodeButton.IsEnabled = false;
            AboutUpdateStatusText.Text = "正在强制切换到下一个加速节点...";
        }
    }

    private void CancelDownload_Click(object sender, RoutedEventArgs e)
    {
        if (!SharedUpdateSession.Cancel())
        {
            return;
        }

        AboutCancelDownloadButton.IsEnabled = false;
        AboutPauseResumeButton.IsEnabled = false;
        AboutUpdateStatusText.Text = "正在取消下载并清理临时文件...";
    }

    private void ShowAvailableUpdate(UpdateRelease release)
    {
        _availableUpdate = release;
        var notes = string.IsNullOrWhiteSpace(release.ReleaseNotes)
            ? "此版本没有附加更新说明。"
            : release.ReleaseNotes.Trim();
        if (notes.Length > 1800)
        {
            notes = notes[..1800] + "\n...";
        }

        AboutReleaseNotesText.Text = notes;
        AboutReleaseNotesBorder.Visibility = Visibility.Visible;
        AboutAccelerationOptionPanel.Visibility = Visibility.Visible;
        AboutAccelerationToggle.IsEnabled = true;
        AboutInstallUpdateButton.Content = $"下载更新 {release.Version}";
        AboutInstallUpdateButton.IsEnabled = true;
        AboutInstallUpdateButton.Visibility = Visibility.Visible;
        AboutUpdateStatusText.Text = $"发现新版本 {release.Version}。确认说明后，点击“下载更新”。";
    }

    private void SharedUpdateSession_Changed(object? sender, UpdateDownloadSessionSnapshot snapshot)
    {
        if (!Dispatcher.HasShutdownStarted)
        {
            Dispatcher.BeginInvoke(() => ApplySharedUpdateSession(snapshot));
        }
    }

    private void ApplySharedUpdateSession(UpdateDownloadSessionSnapshot snapshot)
    {
        if (snapshot.Release is not null)
        {
            _availableUpdate = snapshot.Release;
            ShowReleaseNotes(snapshot.Release);
        }

        if (AboutAccelerationToggle.IsChecked != snapshot.UseAccelerationNodes)
        {
            AboutAccelerationToggle.IsChecked = snapshot.UseAccelerationNodes;
        }

        switch (snapshot.State)
        {
            case UpdateDownloadSessionState.Idle:
                return;
            case UpdateDownloadSessionState.Downloading:
                AboutCheckUpdateButton.IsEnabled = false;
                AboutAccelerationOptionPanel.Visibility = Visibility.Collapsed;
                AboutInstallUpdateButton.Visibility = Visibility.Collapsed;
                AboutPauseResumeButton.Content = "暂停下载";
                AboutPauseResumeButton.IsEnabled = true;
                AboutPauseResumeButton.Visibility = Visibility.Visible;
                AboutBackgroundDownloadButton.Content = "转入后台下载";
                AboutBackgroundDownloadButton.Visibility = Visibility.Visible;
                AboutSwitchNodeButton.IsEnabled = snapshot.UseAccelerationNodes;
                AboutSwitchNodeButton.Visibility = snapshot.UseAccelerationNodes ? Visibility.Visible : Visibility.Collapsed;
                AboutCancelDownloadButton.IsEnabled = true;
                AboutCancelDownloadButton.Visibility = Visibility.Visible;
                UpdateProgressBar.Visibility = Visibility.Visible;
                if (snapshot.Progress is not null)
                {
                    ReportDownloadProgress(snapshot.Progress, snapshot.ContinueInBackground);
                }
                else
                {
                    UpdateProgressBar.IsIndeterminate = true;
                    AboutUpdateStatusText.Text = snapshot.ContinueInBackground
                        ? "正在后台下载更新..."
                        : "正在下载并校验更新...";
                }

                return;
            case UpdateDownloadSessionState.Paused:
                AboutCheckUpdateButton.IsEnabled = false;
                AboutAccelerationOptionPanel.Visibility = Visibility.Collapsed;
                AboutInstallUpdateButton.Visibility = Visibility.Collapsed;
                AboutPauseResumeButton.Content = "继续下载";
                AboutPauseResumeButton.IsEnabled = true;
                AboutPauseResumeButton.Visibility = Visibility.Visible;
                AboutBackgroundDownloadButton.Content = "后台继续下载";
                AboutBackgroundDownloadButton.Visibility = Visibility.Visible;
                AboutSwitchNodeButton.IsEnabled = snapshot.UseAccelerationNodes;
                AboutSwitchNodeButton.Visibility = snapshot.UseAccelerationNodes ? Visibility.Visible : Visibility.Collapsed;
                AboutCancelDownloadButton.IsEnabled = true;
                AboutCancelDownloadButton.Visibility = Visibility.Visible;
                UpdateProgressBar.IsIndeterminate = false;
                UpdateProgressBar.Visibility = Visibility.Visible;
                if (snapshot.Progress is not null)
                {
                    ReportDownloadProgress(snapshot.Progress, background: false);
                }

                AboutUpdateStatusText.Text = "下载已暂停。关闭设置不会取消；重新打开后可继续，或选择后台继续下载。";
                return;
            case UpdateDownloadSessionState.Completed:
                AboutCheckUpdateButton.IsEnabled = true;
                AboutAccelerationOptionPanel.Visibility = Visibility.Collapsed;
                AboutPauseResumeButton.Visibility = Visibility.Collapsed;
                AboutBackgroundDownloadButton.Visibility = Visibility.Collapsed;
                AboutSwitchNodeButton.Visibility = Visibility.Collapsed;
                AboutCancelDownloadButton.Visibility = Visibility.Collapsed;
                UpdateProgressBar.IsIndeterminate = false;
                UpdateProgressBar.Value = 1;
                UpdateProgressBar.Visibility = Visibility.Visible;
                AboutInstallUpdateButton.Content = $"立即安装 {snapshot.Release?.Version}";
                AboutInstallUpdateButton.IsEnabled = !string.IsNullOrWhiteSpace(snapshot.DownloadedPath);
                AboutInstallUpdateButton.Visibility = Visibility.Visible;
                AboutUpdateStatusText.Text = "更新已下载并完成校验。点击“立即安装”后才会退出并替换程序。";
                return;
            case UpdateDownloadSessionState.Cancelled:
                ResetDownloadUi();
                AboutUpdateStatusText.Text = "下载已取消。可以重新点击“下载更新”。";
                return;
            case UpdateDownloadSessionState.Failed:
                ResetDownloadUi();
                AboutUpdateStatusText.Text = GetUpdateErrorText(new InvalidOperationException(snapshot.ErrorMessage ?? "下载失败。"));
                return;
        }
    }

    private void ShowReleaseNotes(UpdateRelease release)
    {
        _availableUpdate = release;
        var notes = string.IsNullOrWhiteSpace(release.ReleaseNotes)
            ? "此版本没有附加更新说明。"
            : release.ReleaseNotes.Trim();
        if (notes.Length > 1800)
        {
            notes = notes[..1800] + "\n...";
        }

        AboutReleaseNotesText.Text = notes;
        AboutReleaseNotesBorder.Visibility = Visibility.Visible;
    }

    private void ResetDownloadUi()
    {
        AboutCheckUpdateButton.IsEnabled = true;
        AboutAccelerationOptionPanel.Visibility = _availableUpdate is null
            ? Visibility.Collapsed
            : Visibility.Visible;
        UpdateProgressBar.IsIndeterminate = false;
        UpdateProgressBar.Visibility = Visibility.Collapsed;
        AboutPauseResumeButton.Visibility = Visibility.Collapsed;
        AboutBackgroundDownloadButton.Visibility = Visibility.Collapsed;
        AboutSwitchNodeButton.Visibility = Visibility.Collapsed;
        AboutCancelDownloadButton.Visibility = Visibility.Collapsed;
        AboutInstallUpdateButton.Content = _availableUpdate is null ? "下载更新" : $"下载更新 {_availableUpdate.Version}";
        AboutInstallUpdateButton.IsEnabled = _availableUpdate is not null;
        AboutInstallUpdateButton.Visibility = _availableUpdate is null ? Visibility.Collapsed : Visibility.Visible;
    }

    private void ReportDownloadProgress(UpdateDownloadProgress progress, bool background)
    {
        if (progress.Fraction is double fraction)
        {
            UpdateProgressBar.IsIndeterminate = false;
            UpdateProgressBar.Value = fraction;
        }

        var total = progress.TotalBytes is > 0 ? FormatDataSize(progress.TotalBytes.Value) : "未知大小";
        var node = string.IsNullOrWhiteSpace(progress.NodeId) ? string.Empty : $" · 节点 {progress.NodeId}";
        var connectionCount = progress.IsParallelFallback
            ? " · 4 路失败，已回退单路"
            : progress.ActiveConnectionCount > 1 ? $" · {progress.ActiveConnectionCount} 路" : " · 单路";
        var prefix = background ? "正在后台下载更新" : "正在下载更新";
        AboutUpdateStatusText.Text = $"{prefix}... {FormatDataSize(progress.BytesReceived)} / {total} · {FormatDataSize((long)progress.BytesPerSecond)}/秒{connectionCount}{node}";
    }

    private static string FormatDataSize(long bytes)
    {
        if (bytes < 1024 * 1024)
        {
            return $"{bytes / 1024d:F0} KB";
        }

        return $"{bytes / 1024d / 1024d:F1} MB";
    }

    private static string GetUpdateErrorText(Exception exception)
    {
        if (exception is HttpRequestException { StatusCode: System.Net.HttpStatusCode.Forbidden })
        {
            return "GitHub 更新服务暂时拒绝请求，请稍后重试或切换网络。";
        }

        if (exception is HttpRequestException { StatusCode: System.Net.HttpStatusCode.NotFound })
        {
            return "GitHub 仓库尚未发布可用的正式 Release。";
        }

        if (exception is TaskCanceledException)
        {
            return "检查更新已取消或网络请求超时。";
        }

        if (exception is System.IO.InvalidDataException)
        {
            return $"更新资源无效：{exception.Message}";
        }

        return $"更新失败：{exception.Message}";
    }

    protected override void OnClosed(EventArgs e)
    {
        _shortcutDropHelperCancellation?.Cancel();
        _shortcutDropHelperCancellation = null;
        SharedUpdateSession.Changed -= SharedUpdateSession_Changed;
        SharedUpdateSession.PauseWhenUiCloses();
        base.OnClosed(e);
    }

    public static void StopUpdateDownloadForApplicationExit() => SharedUpdateSession.Dispose();

    private async void ShortcutDropPanel_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (!ElevationService.IsAdministrator())
        {
            return;
        }

        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        if (!string.Equals(
                GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey),
                ExtendedWheelActionMode.Shortcut,
                StringComparison.OrdinalIgnoreCase)
            || !string.IsNullOrWhiteSpace(slot.ShortcutPath))
        {
            return;
        }

        e.Handled = true;
        if (_shortcutDropHelperCancellation is not null)
        {
            SlotValueText.Text = "拖放窗口已打开，请把 .lnk 拖入该窗口";
            return;
        }

        await RunShortcutDropHelperAsync();
    }

    private async Task RunShortcutDropHelperAsync()
    {
        var handoffId = Guid.NewGuid().ToString("N");
        var handoffPath = ShortcutDropHandoff.GetResultPath(handoffId);
        using var cancellation = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        _shortcutDropHelperCancellation = cancellation;

        try
        {
            if (!ElevationService.TryStartUnelevatedProcess(
                    new[] { "--shortcut-drop-helper", handoffId },
                    out var startError))
            {
                PasteTrace.Mark($"ShortcutDrop_helper_start_failed error={startError}");
                WpfMessageBox.Show(
                    this,
                    startError ?? "无法启动普通权限拖放窗口。",
                    "超级中键",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
                return;
            }

            PasteTrace.Mark($"ShortcutDrop_helper_started slot={_selectedExtendedSlotIndex}");
            SlotValueText.Text = "请把 .lnk 拖入已打开的普通权限窗口";
            while (!System.IO.File.Exists(handoffPath))
            {
                await Task.Delay(100, cancellation.Token);
            }

            var shortcutPath = await System.IO.File.ReadAllTextAsync(
                handoffPath,
                cancellation.Token);
            if (!TryApplyShortcutPath(shortcutPath, out var rejectionReason))
            {
                PasteTrace.Mark(
                    $"ShortcutDrop_helper_rejected slot={_selectedExtendedSlotIndex} " +
                    $"reason={rejectionReason}");
                return;
            }

            PasteTrace.Mark(
                $"ShortcutDrop_helper_accepted slot={_selectedExtendedSlotIndex} " +
                $"file={System.IO.Path.GetFileName(shortcutPath)}");
        }
        catch (OperationCanceledException)
        {
            PasteTrace.Mark("ShortcutDrop_helper_cancelled");
        }
        catch (Exception exception)
        {
            PasteTrace.Mark(
                $"ShortcutDrop_helper_exception {exception.GetType().Name}: {exception.Message}");
        }
        finally
        {
            ShortcutDropHandoff.TryDelete(handoffPath);
            if (ReferenceEquals(_shortcutDropHelperCancellation, cancellation))
            {
                _shortcutDropHelperCancellation = null;
            }

            if (IsLoaded)
            {
                UpdateSlotEditor();
            }
        }
    }

    private bool TryApplyShortcutPath(string? shortcutPath, out string rejectionReason)
    {
        rejectionReason = string.Empty;
        var slot = _settings.Wheel.ExtendedWheel.Slots[_selectedExtendedSlotIndex];
        if (!string.Equals(
                GetComboTag(SlotModeBox, ExtendedWheelActionMode.Hotkey),
                ExtendedWheelActionMode.Shortcut,
                StringComparison.OrdinalIgnoreCase))
        {
            rejectionReason = "not_shortcut_mode";
            return false;
        }

        if (!string.IsNullOrWhiteSpace(slot.ShortcutPath))
        {
            rejectionReason = "slot_already_configured";
            return false;
        }

        if (string.IsNullOrWhiteSpace(shortcutPath)
            || !System.IO.File.Exists(shortcutPath)
            || !string.Equals(System.IO.Path.GetExtension(shortcutPath), ".lnk", StringComparison.OrdinalIgnoreCase))
        {
            rejectionReason = "invalid_lnk_path";
            return false;
        }

        slot.ShortcutPath = shortcutPath;
        SaveExtendedSlotFromUi();
        UpdateSlotEditor();
        DrawExtendedWheelPreview();
        rejectionReason = "accepted";
        return true;
    }

    private void ChooseShortcutFile(ExtendedWheelActionSlot slot)
    {
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "Windows 快捷方式 (*.lnk)|*.lnk",
            CheckFileExists = true
        };

        if (dialog.ShowDialog(this) == true)
        {
            slot.ShortcutPath = dialog.FileName;
        }
    }

    private static bool IsBrowserShortcut(string shortcutPath)
    {
        var targetPath = TryResolveShortcutTargetPath(shortcutPath);
        var executableName = string.IsNullOrWhiteSpace(targetPath)
            ? string.Empty
            : System.IO.Path.GetFileName(targetPath);
        return executableName.Equals("msedge.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("chrome.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("brave.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("opera.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("vivaldi.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("firefox.exe", StringComparison.OrdinalIgnoreCase)
            || executableName.Equals("waterfox.exe", StringComparison.OrdinalIgnoreCase);
    }

    private static string? TryResolveShortcutTargetPath(string shortcutPath)
    {
        if (string.IsNullOrWhiteSpace(shortcutPath) || !System.IO.File.Exists(shortcutPath))
        {
            return null;
        }

        try
        {
            var shellType = Type.GetTypeFromProgID("WScript.Shell");
            if (shellType is null)
            {
                return null;
            }

            dynamic shell = Activator.CreateInstance(shellType)!;
            dynamic shortcut = shell.CreateShortcut(shortcutPath);
            string? targetPath = shortcut.TargetPath;
            return string.IsNullOrWhiteSpace(targetPath) ? null : targetPath;
        }
        catch
        {
            return null;
        }
    }

    private static bool TryGetShortcutDropPath(WpfDragEventArgs e, out string path)
    {
        path = string.Empty;
        if (!e.Data.GetDataPresent(DataFormats.FileDrop))
        {
            return false;
        }

        if (e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } files)
        {
            return false;
        }

        var candidate = files[0];
        if (!string.Equals(System.IO.Path.GetExtension(candidate), ".lnk", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        path = candidate;
        return true;
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
                BitmapSizeOptions.FromWidthAndHeight(36, 36));
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
                BitmapSizeOptions.FromWidthAndHeight(36, 36));
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
                BitmapSizeOptions.FromWidthAndHeight(36, 36));
            source.Freeze();
            return source;
        }
        catch
        {
            return null;
        }
    }

    private static Key NormalizeRecordedKey(WpfKeyEventArgs e)
    {
        return e.Key switch
        {
            Key.System => e.SystemKey,
            Key.ImeProcessed => e.ImeProcessedKey,
            Key.DeadCharProcessed => e.DeadCharProcessedKey,
            _ => e.Key
        };
    }

    private void AddRecordedKey(Key key)
    {
        var normalized = NormalizeHotkeyKey(key);
        if (!_recordedHotkeyKeys.Contains(normalized))
        {
            _recordedHotkeyKeys.Add(normalized);
        }
    }

    private static Key NormalizeHotkeyKey(Key key)
    {
        return key switch
        {
            Key.LeftCtrl or Key.RightCtrl => Key.LeftCtrl,
            Key.LeftShift or Key.RightShift => Key.LeftShift,
            Key.LeftAlt or Key.RightAlt => Key.LeftAlt,
            Key.RWin => Key.LWin,
            _ => key
        };
    }

    private static string BuildRecordedHotkeyText(IReadOnlyList<Key> keys, string fallback)
    {
        if (keys.Count == 0)
        {
            return fallback;
        }

        var ordered = keys
            .OrderBy(GetHotkeySortOrder)
            .ThenBy(KeyToText)
            .Select(KeyToText)
            .Where(p => !string.IsNullOrWhiteSpace(p))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
        return ordered.Count == 0 ? fallback : string.Join("+", ordered);
    }

    private static int GetHotkeySortOrder(Key key)
    {
        return key switch
        {
            Key.LeftCtrl => 0,
            Key.LeftShift => 1,
            Key.LeftAlt => 2,
            Key.LWin => 3,
            _ => 10
        };
    }

    private static string KeyToText(Key key)
    {
        return key switch
        {
            Key.LeftCtrl or Key.RightCtrl => "Ctrl",
            Key.LeftShift or Key.RightShift => "Shift",
            Key.LeftAlt or Key.RightAlt => "Alt",
            Key.LWin or Key.RWin => "Win",
            Key.Return => "Enter",
            Key.Escape => "Esc",
            Key.Space => "Space",
            Key.Back => "Backspace",
            Key.Delete => "Delete",
            Key.Insert => "Insert",
            Key.PageUp => "PageUp",
            Key.PageDown => "PageDown",
            >= Key.D0 and <= Key.D9 => key.ToString()[1..],
            >= Key.NumPad0 and <= Key.NumPad9 => "Num" + key.ToString()[^1],
            Key.OemPlus => "=",
            Key.OemMinus => "-",
            Key.OemComma => ",",
            Key.OemPeriod => ".",
            Key.OemQuestion => "/",
            Key.OemSemicolon => ";",
            Key.OemQuotes => "'",
            Key.OemOpenBrackets => "[",
            Key.OemCloseBrackets => "]",
            Key.OemPipe => "\\",
            Key.OemTilde => "`",
            _ => key.ToString()
        };
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

    private static string NormalizeUiMode(string mode)
    {
        return string.Equals(mode, ExtendedWheelActionMode.Shortcut, StringComparison.OrdinalIgnoreCase)
            ? ExtendedWheelActionMode.Shortcut
            : ExtendedWheelActionMode.Hotkey;
    }

    private static string GetSlotPositionLabel(int slotIndex)
    {
        var group = slotIndex switch
        {
            <= 2 => "上",
            <= 5 => "右",
            <= 8 => "下",
            _ => "左"
        };
        var indexInGroup = slotIndex % 3 + 1;
        return $"{group}{indexInGroup}";
    }

    private static void SelectComboValue(ComboBox box, string value)
    {
        foreach (ComboBoxItem item in box.Items)
        {
            if (string.Equals(item.Content?.ToString(), value, StringComparison.OrdinalIgnoreCase))
            {
                box.SelectedItem = item;
                return;
            }
        }
        box.SelectedIndex = 0;
    }

    private static string GetComboValue(ComboBox box, string fallback)
    {
        return (box.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? fallback;
    }

    private static void SelectComboTag(ComboBox box, string value)
    {
        foreach (ComboBoxItem item in box.Items)
        {
            if (string.Equals(item.Tag?.ToString(), value, StringComparison.OrdinalIgnoreCase))
            {
                box.SelectedItem = item;
                return;
            }
        }
        box.SelectedIndex = 0;
    }

    private static string GetComboTag(ComboBox box, string fallback)
    {
        return (box.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? fallback;
    }

    private static double GetPreviewExtendedRingThickness(double wheelRadius)
    {
        return Math.Max(PreviewExtendedRingMinThickness, wheelRadius * 0.44);
    }

    private static (int Direction, double StartAngle, double EndAngle) GetExtendedSlotGeometry(int slotIndex, double gapDegrees)
    {
        var (direction, start, end) = slotIndex switch
        {
            0 => (0, -135.0, -105.0),
            1 => (0, -105.0, -75.0),
            2 => (0, -75.0, -45.0),
            3 => (1, -45.0, -15.0),
            4 => (1, -15.0, 15.0),
            5 => (1, 15.0, 45.0),
            6 => (2, 45.0, 75.0),
            7 => (2, 75.0, 105.0),
            8 => (2, 105.0, 135.0),
            9 => (3, 135.0, 165.0),
            10 => (3, 165.0, 195.0),
            _ => (3, 195.0, 225.0)
        };
        var gap = Math.Clamp(gapDegrees * 0.5, 0, 4);
        return (direction, start + gap, end - gap);
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

    private static Point CircleSectorCentroid(Point center, double innerRadius, double outerRadius, double startAngleDeg, double endAngleDeg)
    {
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
}
