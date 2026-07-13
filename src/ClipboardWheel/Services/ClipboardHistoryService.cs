using System.Collections.ObjectModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Threading;
using System.Windows.Media.Imaging;
using ClipboardWheel.Models;
using DrawingBitmap = System.Drawing.Bitmap;
using DrawingImageFormat = System.Drawing.Imaging.ImageFormat;
// Use the WPF Clipboard API (GetDataObject / SetDataObject) which returns IDataObject
// compatible with the rest of the file. WinForms / WinRT variants live in other
// namespaces and would clash with implicit global usings.
using Clipboard = System.Windows.Clipboard;

namespace ClipboardWheel.Services;

public sealed class ClipboardHistoryService
{
    private const int ClipboardBusyHResult = unchecked((int)0x800401D0); // CLIPBRD_E_CANT_OPEN
    private const int CaptureRetryCount = 6;
    private const int CaptureRetryDelayMs = 50;
    private const int MaxImageBytes = 32 * 1024 * 1024;
    private const int MaxImagePixels = 64_000_000;
    private const int PreviewImageMaxEdgePixels = 360;
    private static readonly string[] EncodedImageFormats =
    {
        "PNG",
        "image/png",
        "JFIF",
        "JPEG",
        "JPG",
        "image/jpeg",
        "image/jpg",
        "GIF",
        "image/gif",
        "BMP",
        "image/bmp"
    };

    private readonly SettingsService _settingsService;
    private int _captureRetryGeneration;
    private DateTime _suppressUntilUtc = DateTime.MinValue;
    private ClipboardEntry[] _wheelSnapshot = Array.Empty<ClipboardEntry>();

    public ObservableCollection<ClipboardEntry> Entries { get; } = new();

    public ClipboardHistoryService(SettingsService settingsService)
    {
        _settingsService = settingsService;
        _settingsService.SettingsChanged += SettingsService_SettingsChanged;
    }

    public IReadOnlyList<ClipboardEntry> Snapshot()
    {
        return Volatile.Read(ref _wheelSnapshot).ToList();
    }

    public IReadOnlyList<ClipboardEntry> SnapshotForWheel(int visibleHistoryCapacity)
    {
        return Volatile.Read(ref _wheelSnapshot)
            .Take(Math.Max(0, visibleHistoryCapacity))
            .ToList();
    }

    public void Clear()
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is null)
        {
            return;
        }

        dispatcher.Invoke(() =>
        {
            Entries.Clear();
            PublishSnapshotOnDispatcher();
        });
    }

    public void SuppressCaptureFor(TimeSpan duration)
    {
        _suppressUntilUtc = DateTime.UtcNow.Add(duration);
        Interlocked.Increment(ref _captureRetryGeneration);
    }

    public void CaptureCurrentClipboard()
    {
        var generation = Interlocked.Increment(ref _captureRetryGeneration);
        CaptureCurrentClipboard(generation, attempt: 1);
    }

    private void CaptureCurrentClipboard(int generation, int attempt)
    {
        if (generation != Volatile.Read(ref _captureRetryGeneration))
        {
            return;
        }

        if (DateTime.UtcNow < _suppressUntilUtc)
        {
            return;
        }

        try
        {
            var dataObject = Clipboard.GetDataObject();
            if (dataObject is null)
            {
                return;
            }

            PasteTrace.Mark($"CaptureCurrentClipboard formats={string.Join("|", dataObject.GetFormats(autoConvert: false))}");
            var entry = CreateEntryFromDataObject(dataObject, ForegroundWindowService.GetForegroundProcessName());
            if (entry is not null)
            {
                AddOrMoveToTop(entry);
                PasteTrace.Mark($"CaptureCurrentClipboard_added is_image={entry.IsImageContent} has_image={entry.HasImage} image_bytes={entry.ImagePngBytes?.Length ?? 0} has_text={!string.IsNullOrWhiteSpace(entry.PlainText)} has_html={!string.IsNullOrWhiteSpace(entry.HtmlText)} has_csv={!string.IsNullOrWhiteSpace(entry.CsvText)}");
            }
            else
            {
                PasteTrace.Mark("CaptureCurrentClipboard_no_entry");
            }
        }
        catch (COMException ex) when (IsClipboardBusy(ex) && attempt < CaptureRetryCount)
        {
            PasteTrace.Mark($"CaptureCurrentClipboard_busy_retry attempt={attempt}");
            ScheduleCaptureRetry(generation, attempt + 1);
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"CaptureCurrentClipboard_exception {ex.GetType().Name}: {ex.Message}");
        }
    }

    private void ScheduleCaptureRetry(int generation, int attempt)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is null)
        {
            return;
        }

        try
        {
            dispatcher.BeginInvoke(async () =>
            {
                await Task.Delay(CaptureRetryDelayMs);
                CaptureCurrentClipboard(generation, attempt);
            }, DispatcherPriority.Background);
        }
        catch (InvalidOperationException)
        {
            // The dispatcher is shutting down.
        }
    }

    public void Import(ClipboardEntry entry)
    {
        AddOrMoveToTop(entry);
    }

    public void ImportAsBackfill(ClipboardEntry entry)
    {
        Application.Current.Dispatcher.Invoke(() =>
        {
            if (Entries.Any(x => IsSame(x, entry)))
            {
                return;
            }

            var max = _settingsService.Current.Clipboard.MaxHistoryItems;
            if (Entries.Count >= max)
            {
                return;
            }

            Entries.Add(entry);
            NormalizeLockedSlots(GetCurrentVisibleHistoryCapacity());
            PublishSnapshotOnDispatcher();
        });
    }

    public bool ToggleLock(ClipboardEntry entry)
    {
        var toggled = false;
        var dispatcher = Application.Current.Dispatcher;
        void Toggle()
        {
            var match = Entries.FirstOrDefault(x => string.Equals(x.Id, entry.Id, StringComparison.Ordinal));
            if (match is null)
            {
                return;
            }

            match.IsLocked = !match.IsLocked;
            toggled = true;
            PublishSnapshotOnDispatcher();
        }

        if (dispatcher.CheckAccess())
        {
            Toggle();
        }
        else
        {
            dispatcher.Invoke(Toggle);
        }

        return toggled;
    }

    public ClipboardEntry? CreateEntryFromDataObject(IDataObject dataObject, string? sourceProcessName)
    {
        var settings = _settingsService.Current;

        string? plainText = null;
        string? htmlText = null;
        string? rtfText = null;
        string? csvText = null;
        string? tsvText = null;
        byte[]? imagePngBytes = null;
        byte[]? previewImagePngBytes = null;
        string? imageHash = null;

        try
        {
            if (settings.Clipboard.CapturePlainText)
            {
                if (dataObject.GetDataPresent(DataFormats.UnicodeText))
                {
                    plainText = dataObject.GetData(DataFormats.UnicodeText) as string;
                }
                else if (dataObject.GetDataPresent(DataFormats.Text))
                {
                    plainText = dataObject.GetData(DataFormats.Text) as string;
                }
            }

            if (settings.Clipboard.CaptureHtml && dataObject.GetDataPresent(DataFormats.Html))
            {
                htmlText = dataObject.GetData(DataFormats.Html) as string;
            }

            if (settings.Clipboard.CaptureRtf && dataObject.GetDataPresent(DataFormats.Rtf))
            {
                rtfText = dataObject.GetData(DataFormats.Rtf) as string;
            }

            if (settings.Clipboard.CaptureCsv && dataObject.GetDataPresent(DataFormats.CommaSeparatedValue))
            {
                csvText = dataObject.GetData(DataFormats.CommaSeparatedValue) as string;
            }

            // Excel/WPS copied ranges usually expose tab-separated UnicodeText. Keep it explicitly as TSV.
            if (!string.IsNullOrWhiteSpace(plainText) && plainText.Contains('\t'))
            {
                tsvText = plainText;
            }

            if (ShouldCaptureImage(settings, dataObject, plainText, htmlText, csvText))
            {
                PasteTrace.Mark("CreateEntry_image_candidate");
                imagePngBytes = TryReadImagePng(dataObject);
                if (imagePngBytes is not null)
                {
                    imageHash = Convert.ToHexString(SHA256.HashData(imagePngBytes));
                    previewImagePngBytes = CreatePreviewImageBytes(imagePngBytes);
                    PasteTrace.Mark($"CreateEntry_image_captured bytes={imagePngBytes.Length}");
                }
                else
                {
                    PasteTrace.Mark("CreateEntry_image_read_failed");
                }
            }
        }
        catch (COMException ex) when (IsClipboardBusy(ex))
        {
            throw;
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"CreateEntry_exception {ex.GetType().Name}: {ex.Message}");
            return null;
        }

        if (string.IsNullOrWhiteSpace(plainText)
            && string.IsNullOrWhiteSpace(htmlText)
            && string.IsNullOrWhiteSpace(rtfText)
            && string.IsNullOrWhiteSpace(csvText)
            && imagePngBytes is null)
        {
            return null;
        }

        if (!string.IsNullOrWhiteSpace(plainText) && settings.Clipboard.IgnorePasswordLikeText && LooksLikeSecret(plainText))
        {
            return null;
        }

        var displayText = imagePngBytes is not null
            ? "[图片]"
            : BuildDisplayText(plainText, htmlText, settings.Wheel.MaxPreviewChars);
        var looksLikeSpreadsheet = LooksLikeSpreadsheet(plainText, htmlText);
        var looksLikeSingleCell = LooksLikeSingleCell(plainText);

        return new ClipboardEntry
        {
            SourceProcessName = sourceProcessName,
            DisplayText = displayText,
            PlainText = NormalizeLineEndings(plainText),
            HtmlText = htmlText,
            RtfText = rtfText,
            CsvText = csvText,
            TsvText = tsvText,
            ImagePngBytes = imagePngBytes,
            PreviewImagePngBytes = previewImagePngBytes,
            ImageHash = imageHash,
            IsImageContent = imagePngBytes is not null,
            LooksLikeSpreadsheet = looksLikeSpreadsheet,
            LooksLikeSingleCell = looksLikeSingleCell,
            PreferredPasteMode = looksLikeSingleCell ? "plainText" : "smart"
        };
    }

    private void AddOrMoveToTop(ClipboardEntry entry)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is null)
        {
            return;
        }

        if (dispatcher.CheckAccess())
        {
            AddOrMoveToTopOnDispatcher(entry);
            return;
        }

        dispatcher.Invoke(() => AddOrMoveToTopOnDispatcher(entry));
    }

    private void AddOrMoveToTopOnDispatcher(ClipboardEntry entry)
    {
        var duplicate = Entries.FirstOrDefault(x => IsSame(x, entry));
        if (duplicate is { IsLocked: true })
        {
            return;
        }

        var max = _settingsService.Current.Clipboard.MaxHistoryItems;
        var lockedByIndex = Entries
            .Select((existing, index) => (existing, index))
            .Where(x => x.existing.IsLocked && x.index < max)
            .ToDictionary(x => x.index, x => x.existing);

        var unlockedEntries = new List<ClipboardEntry> { entry };
        foreach (var existing in Entries)
        {
            if (existing.IsLocked)
            {
                continue;
            }

            if (duplicate is not null && string.Equals(existing.Id, duplicate.Id, StringComparison.Ordinal))
            {
                continue;
            }

            unlockedEntries.Add(existing);
        }

        var reordered = new List<ClipboardEntry>(max);
        var nextUnlocked = 0;
        for (var index = 0; index < max; index++)
        {
            if (lockedByIndex.TryGetValue(index, out var locked))
            {
                reordered.Add(locked);
                continue;
            }

            if (nextUnlocked < unlockedEntries.Count)
            {
                reordered.Add(unlockedEntries[nextUnlocked]);
                nextUnlocked++;
                continue;
            }

            if (!lockedByIndex.Keys.Any(lockedIndex => lockedIndex > index))
            {
                break;
            }
        }

        Entries.Clear();
        foreach (var item in reordered)
        {
            Entries.Add(item);
        }

        NormalizeLockedSlots(GetCurrentVisibleHistoryCapacity());
        PublishSnapshotOnDispatcher();
    }

    private int GetCurrentVisibleHistoryCapacity()
    {
        var settings = _settingsService.Current;
        var shape = WheelLayout.NormalizeShape(settings.Wheel.Shape);
        var sectorCount = WheelLayout.NormalizeSectorCount(shape, settings.Wheel.SectorCount);
        return settings.Wheel.QuickCopy && sectorCount > 1
            ? sectorCount - 1
            : sectorCount;
    }

    private void NormalizeLockedSlots(int visibleHistoryCapacity)
    {
        var max = _settingsService.Current.Clipboard.MaxHistoryItems;
        var visibleCapacity = Math.Clamp(visibleHistoryCapacity, 0, max);
        if (visibleCapacity <= 0 || Entries.Count == 0 || !Entries.Any(x => x.IsLocked))
        {
            return;
        }

        var visibleLocks = new ClipboardEntry?[visibleCapacity];
        var overflowLocks = new List<ClipboardEntry>();
        var unlocked = new List<ClipboardEntry>();

        for (var index = 0; index < Entries.Count; index++)
        {
            var entry = Entries[index];
            if (!entry.IsLocked)
            {
                unlocked.Add(entry);
                continue;
            }

            if (index < visibleCapacity && visibleLocks[index] is null)
            {
                visibleLocks[index] = entry;
            }
            else
            {
                overflowLocks.Add(entry);
            }
        }

        var nextOverflowLock = 0;
        for (var index = visibleCapacity - 1; index >= 0 && nextOverflowLock < overflowLocks.Count; index--)
        {
            if (visibleLocks[index] is null)
            {
                visibleLocks[index] = overflowLocks[nextOverflowLock];
                nextOverflowLock++;
            }
        }

        var reordered = new List<ClipboardEntry>(max);
        var nextUnlocked = 0;
        for (var index = 0; index < visibleCapacity && reordered.Count < max; index++)
        {
            if (visibleLocks[index] is not null)
            {
                reordered.Add(visibleLocks[index]!);
                continue;
            }

            if (nextUnlocked < unlocked.Count)
            {
                reordered.Add(unlocked[nextUnlocked]);
                nextUnlocked++;
            }
        }

        while (nextOverflowLock < overflowLocks.Count && reordered.Count < max)
        {
            reordered.Add(overflowLocks[nextOverflowLock]);
            nextOverflowLock++;
        }

        while (nextUnlocked < unlocked.Count && reordered.Count < max)
        {
            reordered.Add(unlocked[nextUnlocked]);
            nextUnlocked++;
        }

        if (reordered.SequenceEqual(Entries))
        {
            return;
        }

        Entries.Clear();
        foreach (var entry in reordered)
        {
            Entries.Add(entry);
        }
    }

    private void SettingsService_SettingsChanged(object? sender, AppSettings settings)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is null)
        {
            return;
        }

        void Refresh()
        {
            NormalizeLockedSlots(GetCurrentVisibleHistoryCapacity());
            PublishSnapshotOnDispatcher();
        }

        if (dispatcher.CheckAccess())
        {
            Refresh();
            return;
        }

        try
        {
            dispatcher.BeginInvoke(new Action(Refresh), DispatcherPriority.DataBind);
        }
        catch (InvalidOperationException)
        {
            // The dispatcher is shutting down.
        }
    }

    private void PublishSnapshotOnDispatcher()
    {
        Volatile.Write(ref _wheelSnapshot, Entries.ToArray());
    }

    public void Dispose()
    {
        _settingsService.SettingsChanged -= SettingsService_SettingsChanged;
        Interlocked.Increment(ref _captureRetryGeneration);

        // History is intentionally runtime-only. Release image buffers and the
        // lock-free wheel snapshot before process shutdown; settings are the
        // sole application state that remains on disk.
        if (Application.Current?.Dispatcher.CheckAccess() == true)
        {
            Entries.Clear();
            Volatile.Write(ref _wheelSnapshot, Array.Empty<ClipboardEntry>());
        }
    }

    private static bool IsSame(ClipboardEntry a, ClipboardEntry b)
    {
        if (!string.IsNullOrWhiteSpace(a.ImageHash) && !string.IsNullOrWhiteSpace(b.ImageHash))
        {
            return string.Equals(a.ImageHash, b.ImageHash, StringComparison.Ordinal);
        }

        if (!string.IsNullOrWhiteSpace(a.PlainText) && !string.IsNullOrWhiteSpace(b.PlainText))
        {
            return string.Equals(a.PlainText.Trim(), b.PlainText.Trim(), StringComparison.Ordinal);
        }
        return string.Equals(a.HtmlText, b.HtmlText, StringComparison.Ordinal);
    }

    private static byte[]? TryReadImagePng(IDataObject dataObject)
    {
        foreach (var format in EncodedImageFormats)
        {
            var encoded = TryReadEncodedImagePng(dataObject, format);
            if (encoded is not null)
            {
                PasteTrace.Mark($"TryReadImagePng_format={format} bytes={encoded.Length}");
                return encoded;
            }
        }

        return TryReadBitmapPng(dataObject) ?? TryReadDibPng(dataObject);
    }

    private static byte[]? TryReadEncodedImagePng(IDataObject dataObject, string format)
    {
        if (!dataObject.GetDataPresent(format))
        {
            return null;
        }

        var imageData = dataObject.GetData(format, autoConvert: false);
        if (imageData is BitmapSource bitmapSource)
        {
            return EncodeBitmapSourceAsPng(bitmapSource);
        }

        if (imageData is DrawingBitmap drawingBitmap)
        {
            using var stream = new MemoryStream();
            drawingBitmap.Save(stream, DrawingImageFormat.Png);
            var drawingBytes = stream.ToArray();
            return IsUsableImageBytes(drawingBytes) ? drawingBytes : null;
        }

        var bytes = TryReadBytes(imageData);
        if (bytes is null || !IsUsableImageBytes(bytes))
        {
            return null;
        }

        if (string.Equals(format, "PNG", StringComparison.OrdinalIgnoreCase)
            || string.Equals(format, "image/png", StringComparison.OrdinalIgnoreCase))
        {
            return bytes;
        }

        var bitmap = DecodeBitmap(bytes);
        return bitmap is null ? null : EncodeBitmapSourceAsPng(bitmap);
    }

    private static byte[]? TryReadBitmapPng(IDataObject dataObject)
    {
        if (!dataObject.GetDataPresent(DataFormats.Bitmap))
        {
            return null;
        }

        var imageData = dataObject.GetData(DataFormats.Bitmap, autoConvert: true);
        if (imageData is BitmapSource bitmapSource)
        {
            PasteTrace.Mark($"TryReadBitmapPng_bitmapSource {bitmapSource.PixelWidth}x{bitmapSource.PixelHeight}");
            return EncodeBitmapSourceAsPng(bitmapSource);
        }

        if (imageData is DrawingBitmap drawingBitmap)
        {
            PasteTrace.Mark($"TryReadBitmapPng_drawingBitmap {drawingBitmap.Width}x{drawingBitmap.Height}");
            using var stream = new MemoryStream();
            drawingBitmap.Save(stream, DrawingImageFormat.Png);
            var bytes = stream.ToArray();
            return IsUsableImageBytes(bytes) ? bytes : null;
        }

        PasteTrace.Mark($"TryReadBitmapPng_unsupported_type {imageData?.GetType().FullName ?? "<null>"}");

        return null;
    }

    private static byte[]? TryReadDibPng(IDataObject dataObject)
    {
        foreach (var format in new[] { DataFormats.Dib, "DeviceIndependentBitmap", "DeviceIndependentBitmapV5" })
        {
            if (!dataObject.GetDataPresent(format))
            {
                continue;
            }

            var dibBytes = TryReadBytes(dataObject.GetData(format, autoConvert: false));
            var bmpBytes = TryWrapDibAsBmp(dibBytes);
            if (bmpBytes is null)
            {
                continue;
            }

            var bitmap = DecodeBitmap(bmpBytes);
            if (bitmap is null)
            {
                PasteTrace.Mark($"TryReadDibPng_decode_failed format={format} bytes={dibBytes?.Length ?? 0}");
                continue;
            }

            PasteTrace.Mark($"TryReadDibPng_format={format} size={bitmap.PixelWidth}x{bitmap.PixelHeight}");
            return EncodeBitmapSourceAsPng(bitmap);
        }

        return null;
    }

    private static byte[]? TryWrapDibAsBmp(byte[]? dibBytes)
    {
        if (dibBytes is not { Length: >= 12 })
        {
            return null;
        }

        var headerSize = ReadInt32LittleEndian(dibBytes, 0);
        if (headerSize < 12 || headerSize > dibBytes.Length)
        {
            return null;
        }

        var bitCount = headerSize == 12
            ? ReadUInt16LittleEndian(dibBytes, 10)
            : ReadUInt16LittleEndian(dibBytes, 14);
        var compression = headerSize >= 40 ? ReadUInt32LittleEndian(dibBytes, 16) : 0;
        var clrUsed = headerSize >= 40 ? ReadUInt32LittleEndian(dibBytes, 32) : 0;
        var colorCount = clrUsed > 0
            ? clrUsed
            : bitCount <= 8
                ? 1u << bitCount
                : 0;
        var colorTableBytes = colorCount * (headerSize == 12 ? 3u : 4u);
        var maskBytes = headerSize == 40 && compression is 3 or 6 ? 12u : 0u;
        var pixelOffset = 14u + (uint)headerSize + maskBytes + colorTableBytes;

        if (pixelOffset > int.MaxValue || pixelOffset - 14u > dibBytes.Length)
        {
            return null;
        }

        var bmpBytes = new byte[14 + dibBytes.Length];
        bmpBytes[0] = (byte)'B';
        bmpBytes[1] = (byte)'M';
        WriteUInt32LittleEndian(bmpBytes, 2, (uint)bmpBytes.Length);
        WriteUInt32LittleEndian(bmpBytes, 10, pixelOffset);
        Buffer.BlockCopy(dibBytes, 0, bmpBytes, 14, dibBytes.Length);
        return bmpBytes;
    }

    private static int ReadInt32LittleEndian(byte[] bytes, int offset)
    {
        return bytes[offset]
            | bytes[offset + 1] << 8
            | bytes[offset + 2] << 16
            | bytes[offset + 3] << 24;
    }

    private static ushort ReadUInt16LittleEndian(byte[] bytes, int offset)
    {
        return (ushort)(bytes[offset] | bytes[offset + 1] << 8);
    }

    private static uint ReadUInt32LittleEndian(byte[] bytes, int offset)
    {
        return (uint)(bytes[offset]
            | bytes[offset + 1] << 8
            | bytes[offset + 2] << 16
            | bytes[offset + 3] << 24);
    }

    private static void WriteUInt32LittleEndian(byte[] bytes, int offset, uint value)
    {
        bytes[offset] = (byte)(value & 0xFF);
        bytes[offset + 1] = (byte)((value >> 8) & 0xFF);
        bytes[offset + 2] = (byte)((value >> 16) & 0xFF);
        bytes[offset + 3] = (byte)((value >> 24) & 0xFF);
    }

    private static byte[]? EncodeBitmapSourceAsPng(BitmapSource image)
    {
        if (image.PixelWidth <= 0
            || image.PixelHeight <= 0
            || (long)image.PixelWidth * image.PixelHeight > MaxImagePixels)
        {
            return null;
        }

        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(image));
        using var stream = new MemoryStream();
        encoder.Save(stream);
        var bytes = stream.ToArray();
        return IsUsableImageBytes(bytes) ? bytes : null;
    }

    private static BitmapSource? DecodeBitmap(byte[] bytes)
    {
        try
        {
            using var stream = new MemoryStream(bytes);
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            frame?.Freeze();
            return frame;
        }
        catch
        {
            return null;
        }
    }

    internal static byte[]? NormalizeImageBytes(byte[] bytes)
    {
        var decoded = DecodeBitmap(bytes);
        return decoded is null ? null : EncodeBitmapSourceAsPng(decoded);
    }

    internal static byte[]? CreatePreviewImageBytes(byte[]? bytes)
    {
        if (bytes is not { Length: > 0 })
        {
            return null;
        }

        var image = DecodeBitmap(bytes);
        if (image is null)
        {
            return null;
        }

        var maxEdge = Math.Max(image.PixelWidth, image.PixelHeight);
        if (maxEdge <= PreviewImageMaxEdgePixels)
        {
            return bytes.Length <= MaxImageBytes ? bytes : null;
        }

        var scale = PreviewImageMaxEdgePixels / (double)maxEdge;
        var scaled = new TransformedBitmap(image, new System.Windows.Media.ScaleTransform(scale, scale));
        scaled.Freeze();
        return EncodeBitmapSourceAsPng(scaled);
    }

    private static bool IsUsableImageBytes(byte[]? bytes)
    {
        if (bytes is not { Length: > 0 } || bytes.Length > MaxImageBytes)
        {
            return false;
        }

        var decoded = DecodeBitmap(bytes);
        return decoded is not null
            && decoded.PixelWidth > 0
            && decoded.PixelHeight > 0
            && (long)decoded.PixelWidth * decoded.PixelHeight <= MaxImagePixels;
    }

    private static byte[]? TryReadBytes(object? data)
    {
        try
        {
            switch (data)
            {
                case byte[] bytes:
                    return bytes;
                case MemoryStream memoryStream:
                    return memoryStream.ToArray();
                case Stream stream:
                    using (var copy = new MemoryStream())
                    {
                        if (stream.CanSeek)
                        {
                            stream.Position = 0;
                        }
                        stream.CopyTo(copy);
                        return copy.ToArray();
                    }
                default:
                    PasteTrace.Mark($"TryReadBytes_unsupported_type {data?.GetType().FullName ?? "<null>"}");
                    return null;
            }
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"TryReadBytes_exception {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    private static bool ShouldCaptureImage(
        AppSettings settings,
        IDataObject dataObject,
        string? plainText,
        string? htmlText,
        string? csvText)
    {
        if (!settings.Clipboard.CaptureImages)
        {
            PasteTrace.Mark("ShouldCaptureImage_skip disabled");
            return false;
        }

        if (!HasSupportedImageFormat(dataObject))
        {
            PasteTrace.Mark("ShouldCaptureImage_skip no_supported_format");
            return false;
        }

        if (LooksLikeSpreadsheetPayload(plainText, htmlText, csvText))
        {
            PasteTrace.Mark("ShouldCaptureImage_skip spreadsheet_payload");
            return false;
        }

        return true;
    }

    private static bool HasSupportedImageFormat(IDataObject dataObject)
    {
        return EncodedImageFormats.Any(dataObject.GetDataPresent)
            || dataObject.GetDataPresent(DataFormats.Dib)
            || dataObject.GetDataPresent("DeviceIndependentBitmap")
            || dataObject.GetDataPresent("DeviceIndependentBitmapV5")
            || dataObject.GetDataPresent(DataFormats.Bitmap);
    }

    private static bool LooksLikeSpreadsheetPayload(string? plainText, string? htmlText, string? csvText)
    {
        if (!string.IsNullOrWhiteSpace(csvText))
        {
            return true;
        }

        if (!string.IsNullOrWhiteSpace(plainText) && (plainText.Contains('\t') || plainText.Split('\n').Length > 1))
        {
            return true;
        }

        return !string.IsNullOrWhiteSpace(htmlText) && htmlText.Contains("<table", StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeLineEndings(string? text)
    {
        if (text is null)
        {
            return string.Empty;
        }
        return text.Replace("\r\n", "\n").Replace("\r", "\n").TrimEnd('\n');
    }

    private static string BuildDisplayText(string? plainText, string? htmlText, int maxChars)
    {
        var source = !string.IsNullOrWhiteSpace(plainText)
            ? plainText
            : Regex.Replace(htmlText ?? string.Empty, "<.*?>", " ");

        source = Regex.Replace(source.Replace("\r", " ").Replace("\n", " ").Replace("\t", " "), "\\s+", " ").Trim();
        if (string.IsNullOrWhiteSpace(source))
        {
            source = "[非文本剪切板]";
        }
        return source.Length <= maxChars ? source : source[..maxChars] + "…";
    }

    private static bool LooksLikeSpreadsheet(string? plainText, string? htmlText)
    {
        if (!string.IsNullOrWhiteSpace(plainText) && (plainText.Contains('\t') || plainText.Split('\n').Length > 1))
        {
            return true;
        }

        return !string.IsNullOrWhiteSpace(htmlText) && htmlText.Contains("<table", StringComparison.OrdinalIgnoreCase);
    }

    private static bool LooksLikeSingleCell(string? plainText)
    {
        if (string.IsNullOrWhiteSpace(plainText))
        {
            return false;
        }
        var trimmed = plainText.Trim();
        return !trimmed.Contains('\t') && !trimmed.Contains('\n') && !trimmed.Contains('\r');
    }

    private static bool LooksLikeSecret(string text)
    {
        var t = text.Trim();
        if (t.Length is < 8 or > 4096)
        {
            return false;
        }

        if (Regex.IsMatch(t, "(?i)(api[_-]?key|secret|token|password|passwd|bearer)\\s*[:=]"))
        {
            return true;
        }

        if (Regex.IsMatch(t, "^(sk-|pk_|ghp_|gho_|xox[baprs]-)[A-Za-z0-9_\\-]{16,}$"))
        {
            return true;
        }

        return false;
    }

    private static bool IsClipboardBusy(COMException ex)
    {
        return ex.HResult == ClipboardBusyHResult;
    }
}
