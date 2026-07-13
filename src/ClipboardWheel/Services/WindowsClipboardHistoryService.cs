using ClipboardWheel.Models;
using System.IO;
using System.Security.Cryptography;
using System.Runtime.InteropServices.WindowsRuntime;
// This file uses the WinRT Clipboard API (IsHistoryEnabled / GetHistoryItemsAsync),
// not the WPF one.
using Clipboard = Windows.ApplicationModel.DataTransfer.Clipboard;
using Windows.ApplicationModel.DataTransfer;

namespace ClipboardWheel.Services;

public sealed class WindowsClipboardHistoryService
{
    private readonly ClipboardHistoryService _historyService;
    private readonly SettingsService _settingsService;

    public WindowsClipboardHistoryService(ClipboardHistoryService historyService, SettingsService settingsService)
    {
        _historyService = historyService;
        _settingsService = settingsService;
    }

    public async Task TryImportAsync()
    {
        try
        {
            if (!Clipboard.IsHistoryEnabled())
            {
                return;
            }

            var result = await Clipboard.GetHistoryItemsAsync();
            if (result.Status != ClipboardHistoryItemsResultStatus.Success)
            {
                return;
            }

            foreach (var item in result.Items.Take(_settingsService.Current.Clipboard.MaxHistoryItems).Reverse())
            {
                var view = item.Content;
                string? text = null;
                string? html = null;
                string? rtf = null;
                byte[]? imagePngBytes = null;
                byte[]? previewImagePngBytes = null;
                string? imageHash = null;

                if (view.Contains(StandardDataFormats.Text))
                {
                    text = await view.GetTextAsync();
                }

                if (view.Contains(StandardDataFormats.Html))
                {
                    html = await view.GetHtmlFormatAsync();
                }

                if (view.Contains(StandardDataFormats.Rtf))
                {
                    rtf = await view.GetRtfAsync();
                }

                if (_settingsService.Current.Clipboard.CaptureImages && view.Contains(StandardDataFormats.Bitmap))
                {
                    imagePngBytes = await TryReadImagePngAsync(view);
                    if (imagePngBytes is not null)
                    {
                        imageHash = Convert.ToHexString(SHA256.HashData(imagePngBytes));
                        previewImagePngBytes = ClipboardHistoryService.CreatePreviewImageBytes(imagePngBytes);
                    }
                }

                if (string.IsNullOrWhiteSpace(text)
                    && string.IsNullOrWhiteSpace(html)
                    && string.IsNullOrWhiteSpace(rtf)
                    && imagePngBytes is null)
                {
                    continue;
                }

                var display = imagePngBytes is not null
                    ? "[图片]"
                    : BuildDisplay(text, html, _settingsService.Current.Wheel.MaxPreviewChars);
                _historyService.ImportAsBackfill(new ClipboardEntry
                {
                    SourceProcessName = "WindowsClipboardHistory",
                    DisplayText = display,
                    PlainText = text,
                    HtmlText = html,
                    RtfText = rtf,
                    ImagePngBytes = imagePngBytes,
                    PreviewImagePngBytes = previewImagePngBytes,
                    ImageHash = imageHash,
                    IsImageContent = imagePngBytes is not null,
                    LooksLikeSpreadsheet = !string.IsNullOrWhiteSpace(text) && text.Contains('\t'),
                    LooksLikeSingleCell = !string.IsNullOrWhiteSpace(text) && !text.Contains('\t') && !text.Contains('\n') && !text.Contains('\r')
                });
            }
        }
        catch
        {
            // Win+V history import is optional. Failure must not block the app.
        }
    }

    private static string BuildDisplay(string? text, string? html, int maxChars)
    {
        var value = !string.IsNullOrWhiteSpace(text) ? text : html ?? "[历史剪切板]";
        value = value.Replace("\r", " ").Replace("\n", " ").Replace("\t", " ").Trim();
        return value.Length <= maxChars ? value : value[..maxChars] + "…";
    }

    private static async Task<byte[]?> TryReadImagePngAsync(DataPackageView view)
    {
        try
        {
            var reference = await view.GetBitmapAsync();
            using var randomAccessStream = await reference.OpenReadAsync();
            using var input = randomAccessStream.AsStreamForRead();
            using var memory = new MemoryStream();
            await input.CopyToAsync(memory);
            return ClipboardHistoryService.NormalizeImageBytes(memory.ToArray());
        }
        catch
        {
            return null;
        }
    }
}
