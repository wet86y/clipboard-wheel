using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using ClipboardWheel.Models;
using ClipboardWheel.Native;
// Use the WPF Clipboard API. See WindowsClipboardHistoryService.cs for the WinRT
// counterpart used by the Win+V history import path.
using Clipboard = System.Windows.Clipboard;

namespace ClipboardWheel.Services;

public sealed class PasteService
{
    private readonly SettingsService _settingsService;
    private readonly ClipboardHistoryService _historyService;

    public PasteService(SettingsService settingsService, ClipboardHistoryService historyService)
    {
        _settingsService = settingsService;
        _historyService = historyService;
    }

    public async Task PasteAsync(ClipboardEntry entry, PasteMode mode)
    {
        var settings = _settingsService.Current;
        PasteTrace.Mark($"PasteAsync settings RestoreClipboardAfterPaste={settings.Paste.RestoreClipboardAfterPaste} AddPasteToClipboardHistory={settings.Paste.AddPasteToClipboardHistory}");

        // WPF's Clipboard API requires STA; the mouse hook thread has no
        // SynchronizationContext, so async continuations land on the thread
        // pool (MTA). Marshal the entire snapshot-and-write phase onto the
        // UI dispatcher in a single round trip, so we don't pay two
        // cross-thread hops for Get + Set.
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is null)
        {
            PasteTrace.Mark("PasteAsync_early_exit_no_dispatcher");
            return;
        }

        PasteTrace.Mark("PasteAsync_entered");

        var formattedPayloadRequired = RequiresFormattedClipboardPayload(entry, mode);

        // Snapshot the previous clipboard content AND arm the self-capture
        // suppression in one UI-thread round trip, before we start our own
        // SetDataObject loop. The suppression must be active by the time
        // SetDataObject triggers WM_CLIPBOARDUPDATE, otherwise our own write
        // would be re-captured into history.
        //
        // We also detect the common "user just copied this, wants to paste
        // it again" case: when the current clipboard's UnicodeText already
        // matches the wheel entry's PlainText, there is no reason to write
        // to the clipboard at all - the data is already there. Skipping the
        // SetDataObject in that case keeps Windows clipboard history (Win+V)
        // from accumulating a redundant entry on every wheel paste, which
        // would otherwise crowd out the user's actual copy actions.
        IDataObject? previous = null;
        bool clipboardAlreadyHasContent = false;
        try
        {
            PasteTrace.Mark("InvokeAsync_snapshot_start");
            await dispatcher.InvokeAsync(new Action(() =>
            {
                PasteTrace.Mark("snapshot_on_UI_thread");
                try { previous = Clipboard.GetDataObject(); }
                catch (Exception ex) { PasteTrace.Mark($"GetDataObject_ex {ex.GetType().Name}: {ex.Message}"); }

                try
                {
                    if (previous != null && previous.GetDataPresent(DataFormats.UnicodeText))
                    {
                        var currentText = previous.GetData(DataFormats.UnicodeText) as string;
                        var targetText = !string.IsNullOrEmpty(entry.PlainText)
                            ? entry.PlainText
                            : entry.DisplayText;
                        if (!formattedPayloadRequired && !string.IsNullOrEmpty(targetText) && currentText == targetText)
                        {
                            clipboardAlreadyHasContent = true;
                            PasteTrace.Mark("snapshot_content_match");
                        }
                    }
                }
                catch (Exception ex)
                {
                    PasteTrace.Mark($"snapshot_content_match_ex {ex.GetType().Name}: {ex.Message}");
                }

                try
                {
                    _historyService.SuppressCaptureFor(
                        TimeSpan.FromMilliseconds(settings.Paste.RestoreDelayMs + 250));
                    PasteTrace.Mark("suppression_set");
                }
                catch (Exception ex) { PasteTrace.Mark($"suppression_ex {ex.GetType().Name}: {ex.Message}"); }
            }), DispatcherPriority.Send);
            PasteTrace.Mark("InvokeAsync_snapshot_returned");
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"InvokeAsync_snapshot_exception {ex.GetType().Name}: {ex.Message}");
            // Fall through; we can still attempt paste.
        }

        // Build the new payload outside the dispatcher; clipboard writes
        // happen inside the retry loop on UI thread. Pass the settings
        // through so BuildDataObject can decide whether to add the
        // "CanIncludeInClipboardHistory" opt-out format.
        var newData = BuildDataObject(entry, mode, settings.Paste);

        // copy=false (no WPF-internal 10 x 100 ms retry) + our own retry loop.
        // When OleSetClipboard fails with CLIPBRD_E_CANT_OPEN - which happens
        // for ~1 s after a copy in real user environments because shell
        // extensions / antivirus / target-app post-paste bookkeeping hold the
        // clipboard - we sleep ~20 ms and try again until either SetDataObject
        // succeeds or the 1500 ms budget is exhausted. Plain text entries
        // always reach the clipboard (CF_UNICODETEXT); table entries also
        // register CF_HTML so Excel pastes them as cells, not as the latest
        // unrelated copy that happens to be on the clipboard.
        //
        // Skip the whole SetDataObject loop when the clipboard already
        // contains the same UnicodeText (see snapshot_content_match above).
        // This is the dominant case for a clipboard wheel tool - paste what
        // you just copied - and skipping the write keeps Windows clipboard
        // history from blowing up on repetitive pastes.
        if (clipboardAlreadyHasContent)
        {
            PasteTrace.Mark("clipboard_set_skipped content already on clipboard");
        }
        else
        {
            PasteTrace.Mark("clipboard_set_loop_start");
            var setOk = await TrySetClipboardOnDispatcherAsync(newData, maxWaitMs: 1500);
            PasteTrace.Mark($"clipboard_set_loop_returned ok={setOk}");
            if (!setOk)
            {
                PasteTrace.Mark("PasteAsync_abort clipboard_set_failed");
                return;
            }
        }

        // Give the target app's clipboard listener a moment to pick up the new
        // payload, then inject Ctrl+V. SendInput works from any thread.
        PasteTrace.Mark("TaskDelay_20_start");
        await Task.Delay(20);
        PasteTrace.Mark("TaskDelay_20_returned");

        PasteTrace.Mark("SendCtrlV_calling");
        var sendInputOk = NativeMethods.SendCtrlV();
        PasteTrace.Mark($"SendCtrlV_returned ok={sendInputOk}");

        if (settings.Paste.RestoreClipboardAfterPaste && previous is not null)
        {
            // OPT-IN only. The restore races with the target app's clipboard
            // read: we sleep Task.Delay(restoreDelayMs) and then write
            // previous back, but Excel/WPS reading CF_HTML / CF_CSV for a
            // table can take longer than the delay, so it ends up reading
            // the restored previous content instead of our wheel selection.
            // Symptom: paste starts working when the app is fresh, then
            // "reverts to previous version behavior" after the target app
            // gets slower (memory pressure, GC, workbook state). Default is
            // off for that reason. Toggle via settings.json or the Settings
            // window if you want the old behaviour back, knowing the race.
            PasteTrace.Mark("TaskDelay_Restore_start");
            await Task.Delay(settings.Paste.RestoreDelayMs);
            PasteTrace.Mark("TaskDelay_Restore_returned");

            PasteTrace.Mark("restore_clipboard_loop_start");
            var restoreOk = await TrySetClipboardOnDispatcherAsync(previous, maxWaitMs: 1500);
            PasteTrace.Mark($"restore_clipboard_loop_returned ok={restoreOk}");
        }
        else
        {
            PasteTrace.Mark("restore_skipped (race fix or no previous)");
        }

        PasteTrace.Mark("PasteAsync_end");
    }

    // Repeatedly tries Clipboard.SetDataObject on the UI thread, sleeping
    // ~20 ms between failed attempts, until either it succeeds or the total
    // budget is exhausted. Must run with copy=false - copy=true triggers
    // WPF's hardcoded 10 x 100 ms retry loop which makes each outer attempt
    // cost ~1 s and defeats the point of retrying.
    private async Task<bool> TrySetClipboardOnDispatcherAsync(IDataObject data, int maxWaitMs)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher is null) return false;

        var sw = Stopwatch.StartNew();
        var attempt = 0;
        while (sw.ElapsedMilliseconds < maxWaitMs)
        {
            attempt++;
            try
            {
                await dispatcher.InvokeAsync(new Action(() =>
                {
                    PasteTrace.Mark($"clipboard_set_attempt_{attempt}_calling");
                    try
                    {
                        Clipboard.SetDataObject(data, false);
                        PasteTrace.Mark($"clipboard_set_attempt_{attempt}_ok");
                    }
                    catch (Exception ex)
                    {
                        PasteTrace.Mark($"clipboard_set_attempt_{attempt}_ex {ex.GetType().Name}: {ex.Message}");
                        throw;
                    }
                }), DispatcherPriority.Send);
                PasteTrace.Mark($"clipboard_set_succeeded attempts={attempt} elapsed_ms={(int)sw.ElapsedMilliseconds}");
                return true;
            }
            catch
            {
                // Swallow the failure for this attempt; the loop will retry
                // until the budget is exhausted.
                await Task.Delay(20);
            }
        }

        PasteTrace.Mark($"clipboard_set_failed attempts={attempt} elapsed_ms={(int)sw.ElapsedMilliseconds}");
        return false;
    }

    public PasteMode ResolveMode(ClipboardEntry entry)
    {
        var settings = _settingsService.Current;

        // Legacy modifier overrides are intentionally disabled. Paste mode is
        // fixed to the app's default smart mode, while the actual clipboard
        // payload still preserves rich formats when smart mode needs them.
        //
        // var ctrl = NativeMethods.IsKeyDown(NativeMethods.VK_CONTROL);
        // var shift = NativeMethods.IsKeyDown(NativeMethods.VK_SHIFT);
        // if (ctrl && settings.Paste.CtrlModifierMode.Equals("plainText", StringComparison.OrdinalIgnoreCase))
        // {
        //     return PasteMode.PlainText;
        // }
        // if (shift && settings.Paste.ShiftModifierMode.Equals("formatted", StringComparison.OrdinalIgnoreCase))
        // {
        //     return PasteMode.Formatted;
        // }

        return settings.Paste.DefaultMode.ToLowerInvariant() switch
        {
            "plaintext" or "plainText" => PasteMode.PlainText,
            "formatted" => PasteMode.Formatted,
            _ => PasteMode.Smart
        };
    }

    private static DataObject BuildDataObject(ClipboardEntry entry, PasteMode mode, PasteSettings settings)
    {
        PasteTrace.Mark($"BuildDataObject entry_mode_requested={mode} add_to_history_setting={settings.AddPasteToClipboardHistory} is_image_content={entry.IsImageContent} has_image={entry.HasImage} has_html={!string.IsNullOrWhiteSpace(entry.HtmlText)} has_rtf={!string.IsNullOrWhiteSpace(entry.RtfText)} has_csv={!string.IsNullOrWhiteSpace(entry.CsvText)}");

        var effective = GetEffectiveMode(entry, mode);

        var data = new DataObject();
        if (entry.IsImageContent)
        {
            var image = DecodePng(entry.ImagePngBytes);
            if (image is not null)
            {
                data.SetImage(image);
                ApplyClipboardHistoryOptOut(data, settings);
                return data;
            }
        }

        var text = !string.IsNullOrEmpty(entry.PlainText) ? entry.PlainText : entry.DisplayText;

        data.SetData(DataFormats.UnicodeText, text);
        data.SetData(DataFormats.Text, text);

        if (effective == PasteMode.Formatted)
        {
            if (!string.IsNullOrWhiteSpace(entry.HtmlText))
            {
                data.SetData(DataFormats.Html, entry.HtmlText);
            }

            if (!string.IsNullOrWhiteSpace(entry.RtfText))
            {
                data.SetData(DataFormats.Rtf, entry.RtfText);
            }

            if (!string.IsNullOrWhiteSpace(entry.CsvText))
            {
                data.SetData(DataFormats.CommaSeparatedValue, entry.CsvText);
            }
        }

        // Opt out of Windows clipboard history (Win+V) unless the user has
        // explicitly enabled it via settings. The format is the documented
        // "CanIncludeInClipboardHistory" with VT_BOOL=false; only affects
        // Win+V's display, does not change what target apps read from the
        // clipboard.
        ApplyClipboardHistoryOptOut(data, settings);

        return data;
    }

    private static void ApplyClipboardHistoryOptOut(DataObject data, PasteSettings settings)
    {
        if (!settings.AddPasteToClipboardHistory)
        {
            data.SetData("CanIncludeInClipboardHistory", false);
        }
    }

    private static BitmapSource? DecodePng(byte[]? bytes)
    {
        if (bytes is null || bytes.Length == 0)
        {
            return null;
        }

        try
        {
            using var stream = new MemoryStream(bytes);
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            frame?.Freeze();
            return frame;
        }
        catch (Exception ex)
        {
            PasteTrace.Mark($"PasteDecodeImage_exception {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    private static bool RequiresFormattedClipboardPayload(ClipboardEntry entry, PasteMode mode)
    {
        return entry.IsImageContent
            || GetEffectiveMode(entry, mode) == PasteMode.Formatted
            && (!string.IsNullOrWhiteSpace(entry.HtmlText)
                || !string.IsNullOrWhiteSpace(entry.RtfText)
                || !string.IsNullOrWhiteSpace(entry.CsvText));
    }

    private static PasteMode GetEffectiveMode(ClipboardEntry entry, PasteMode mode)
    {
        if (mode != PasteMode.Smart)
        {
            return mode;
        }

        // A single-cell entry (no tabs / newlines in its plain text) is
        // pasted as plain text so it lands cleanly in a text box. But when
        // the entry also carries HTML <table> markup it is actually a
        // spreadsheet copy (e.g. Excel merged cells) and needs the formatted
        // path to preserve cell structure.
        var hasTableHtml = !string.IsNullOrWhiteSpace(entry.HtmlText)
                        && entry.HtmlText.Contains("<table", StringComparison.OrdinalIgnoreCase);
        return entry.LooksLikeSingleCell && !hasTableHtml
            ? PasteMode.PlainText
            : PasteMode.Formatted;
    }
}
