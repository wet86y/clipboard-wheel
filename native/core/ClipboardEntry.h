#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace smk::core {

struct ClipboardEntry {
    std::wstring id;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::wstring source_process_name;
    std::wstring display_text;
    std::wstring plain_text;
    std::wstring html_text;
    std::wstring rtf_text;
    std::wstring csv_text;
    std::wstring tsv_text;
    std::shared_ptr<const std::vector<std::uint8_t>> image_png_bytes;
    std::shared_ptr<const std::vector<std::uint8_t>> preview_image_png_bytes;
    std::wstring image_hash;
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
    std::wstring preferred_paste_mode = L"smart";
    bool is_image_content = false;
    bool looks_like_spreadsheet = false;
    bool looks_like_single_cell = false;
    bool is_quick_copy = false;
    bool is_locked = false;

    [[nodiscard]] bool has_image() const noexcept { return image_png_bytes && !image_png_bytes->empty(); }
    [[nodiscard]] bool has_image_preview() const noexcept {
        return preview_image_png_bytes && !preview_image_png_bytes->empty();
    }
    [[nodiscard]] const std::vector<std::uint8_t>& image_bytes() const noexcept {
        static const std::vector<std::uint8_t> empty;
        return image_png_bytes ? *image_png_bytes : empty;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& preview_image_bytes() const noexcept {
        static const std::vector<std::uint8_t> empty;
        return preview_image_png_bytes ? *preview_image_png_bytes : empty;
    }
    [[nodiscard]] bool has_formatted_payload() const noexcept {
        return !html_text.empty() || !rtf_text.empty();
    }
};

} // namespace smk::core
