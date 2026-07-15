#pragma once

#include <windows.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace smk::windows {

[[nodiscard]] WORD normalize_extended_hotkey_key(UINT virtual_key, LPARAM key_data = 0) noexcept;
[[nodiscard]] std::optional<WORD> extended_hotkey_key_from_name(std::wstring_view value);
[[nodiscard]] std::wstring extended_hotkey_key_name(WORD key);
[[nodiscard]] std::vector<WORD> canonicalize_extended_hotkey_keys(std::span<const WORD> keys);
[[nodiscard]] std::vector<WORD> parse_extended_hotkey(std::wstring_view value);
[[nodiscard]] std::wstring format_extended_hotkey(std::span<const WORD> keys);

class HotkeyRecordingSession final {
public:
    void begin();
    void cancel();
    [[nodiscard]] bool add(UINT virtual_key, LPARAM key_data = 0);
    [[nodiscard]] std::wstring finish();

    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] const std::vector<WORD>& keys() const noexcept { return keys_; }
    [[nodiscard]] std::wstring display_text() const;

private:
    bool recording_ = false;
    std::vector<WORD> keys_;
};

} // namespace smk::windows
