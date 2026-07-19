#pragma once

#include "core/ClipboardEntry.h"
#include "core/PastePolicy.h"

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smk::windows {

struct ImageClipboardPayload {
    static constexpr std::size_t kMaxEncodedBytes = 32U * 1024U * 1024U;
    static constexpr std::uint64_t kMaxPixels = 64'000'000ULL;
    static constexpr UINT kPreviewMaxEdge = 360;

    std::vector<std::uint8_t> png;
    std::vector<std::uint8_t> preview_png;
    UINT width = 0;
    UINT height = 0;
    std::wstring sha256;

    [[nodiscard]] bool valid() const noexcept { return !png.empty() && width > 0 && height > 0; }
};

[[nodiscard]] std::optional<ImageClipboardPayload> read_image_payload(IDataObject* data);
[[nodiscard]] std::optional<ImageClipboardPayload> normalize_png_payload(const std::vector<std::uint8_t>& png);
[[nodiscard]] std::optional<ImageClipboardPayload> normalize_bgra_payload(
    UINT width, UINT height, const std::vector<std::uint8_t>& pixels);
[[nodiscard]] Microsoft::WRL::ComPtr<IDataObject> create_image_data_object(const ImageClipboardPayload& payload);
[[nodiscard]] Microsoft::WRL::ComPtr<IDataObject> create_clipboard_data_object(
    const smk::core::ClipboardEntry& entry, smk::core::PasteMode mode,
    std::optional<std::wstring> plain_text_override = std::nullopt);

} // namespace smk::windows
