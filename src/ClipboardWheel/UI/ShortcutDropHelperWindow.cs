using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using ClipboardWheel.Services;
using WpfDragEventArgs = System.Windows.DragEventArgs;

namespace ClipboardWheel.UI;

internal sealed class ShortcutDropHelperWindow : Window
{
    private readonly string _handoffId;
    private readonly TextBlock _statusText;
    private bool _completed;

    public ShortcutDropHelperWindow(string handoffId)
    {
        _handoffId = handoffId;
        Title = "超级中键 - 快捷方式拖放";
        Width = 440;
        Height = 190;
        ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Topmost = true;
        ShowInTaskbar = true;
        Background = new SolidColorBrush(Color.FromRgb(245, 248, 253));
        AllowDrop = true;

        _statusText = new TextBlock
        {
            Text = "把一个 .lnk 快捷方式拖到这里",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Color.FromRgb(35, 61, 102)),
            HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
            VerticalAlignment = System.Windows.VerticalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.Wrap
        };

        Content = new Border
        {
            Margin = new Thickness(18),
            Padding = new Thickness(24),
            CornerRadius = new CornerRadius(12),
            BorderThickness = new Thickness(2),
            BorderBrush = new SolidColorBrush(Color.FromRgb(91, 132, 199)),
            Background = Brushes.White,
            Child = _statusText
        };

        DragOver += Helper_DragOver;
        Drop += Helper_Drop;
        Closed += Helper_Closed;
    }

    private void Helper_DragOver(object sender, WpfDragEventArgs e)
    {
        e.Effects = !_completed && TryGetShortcutPath(e.Data, out _)
            ? System.Windows.DragDropEffects.Copy
            : System.Windows.DragDropEffects.None;
        e.Handled = true;
    }

    private async void Helper_Drop(object sender, WpfDragEventArgs e)
    {
        e.Handled = true;
        if (_completed || !TryGetShortcutPath(e.Data, out var shortcutPath))
        {
            _statusText.Text = "只支持现有的 .lnk 快捷方式";
            return;
        }

        try
        {
            ShortcutDropHandoff.WriteResult(_handoffId, shortcutPath);
            _completed = true;
            PasteTrace.Mark($"ShortcutDrop_helper_sent file={Path.GetFileName(shortcutPath)}");
            _statusText.Text = "导入成功";
            await Task.Delay(300);
            Close();
        }
        catch (Exception exception)
        {
            PasteTrace.Mark(
                $"ShortcutDrop_helper_send_exception {exception.GetType().Name}: {exception.Message}");
            _statusText.Text = $"无法交回快捷方式：{exception.Message}\n请关闭此窗口后重试";
        }
    }

    private void Helper_Closed(object? sender, EventArgs e)
    {
        if (!_completed)
        {
            try
            {
                ShortcutDropHandoff.WriteResult(_handoffId, string.Empty);
            }
            catch
            {
                // The main window also has a bounded timeout and cancellation.
            }
        }

        Application.Current.Shutdown();
    }

    private static bool TryGetShortcutPath(IDataObject data, out string shortcutPath)
    {
        shortcutPath = string.Empty;
        if (!data.GetDataPresent(DataFormats.FileDrop)
            || data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } files)
        {
            return false;
        }

        var candidate = files[0];
        if (!File.Exists(candidate)
            || !string.Equals(Path.GetExtension(candidate), ".lnk", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        shortcutPath = candidate;
        return true;
    }
}
