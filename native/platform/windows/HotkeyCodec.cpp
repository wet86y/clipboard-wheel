#include "platform/windows/HotkeyCodec.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwctype>
#include <unordered_map>

namespace smk::windows {
namespace {

std::wstring trim(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return std::wstring(value.substr(first, last - first + 1));
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

int modifier_order(WORD key) noexcept {
    switch (key) {
    case VK_CONTROL: return 0;
    case VK_SHIFT: return 1;
    case VK_MENU: return 2;
    case VK_LWIN: return 3;
    default: return 10;
    }
}

struct NamedKey {
    WORD key;
    const wchar_t* canonical;
};

constexpr std::array kNamedKeys{
    NamedKey{VK_CONTROL, L"Ctrl"}, NamedKey{VK_SHIFT, L"Shift"},
    NamedKey{VK_MENU, L"Alt"}, NamedKey{VK_LWIN, L"Win"},
    NamedKey{VK_RETURN, L"Enter"}, NamedKey{VK_ESCAPE, L"Esc"},
    NamedKey{VK_TAB, L"Tab"}, NamedKey{VK_SPACE, L"Space"},
    NamedKey{VK_BACK, L"Backspace"}, NamedKey{VK_DELETE, L"Delete"},
    NamedKey{VK_INSERT, L"Insert"}, NamedKey{VK_HOME, L"Home"},
    NamedKey{VK_END, L"End"}, NamedKey{VK_PRIOR, L"PageUp"},
    NamedKey{VK_NEXT, L"PageDown"}, NamedKey{VK_UP, L"Up"},
    NamedKey{VK_DOWN, L"Down"}, NamedKey{VK_LEFT, L"Left"},
    NamedKey{VK_RIGHT, L"Right"}, NamedKey{VK_CAPITAL, L"CapsLock"},
    NamedKey{VK_NUMLOCK, L"NumLock"}, NamedKey{VK_SCROLL, L"ScrollLock"},
    NamedKey{VK_SNAPSHOT, L"PrintScreen"}, NamedKey{VK_PAUSE, L"Pause"},
    NamedKey{VK_APPS, L"Apps"}, NamedKey{VK_MULTIPLY, L"NumMultiply"},
    NamedKey{VK_ADD, L"NumAdd"}, NamedKey{VK_SEPARATOR, L"NumSeparator"},
    NamedKey{VK_SUBTRACT, L"NumSubtract"}, NamedKey{VK_DECIMAL, L"NumDecimal"},
    NamedKey{VK_DIVIDE, L"NumDivide"}, NamedKey{VK_OEM_PLUS, L"="},
    NamedKey{VK_OEM_MINUS, L"-"}, NamedKey{VK_OEM_COMMA, L","},
    NamedKey{VK_OEM_PERIOD, L"."}, NamedKey{VK_OEM_2, L"/"},
    NamedKey{VK_OEM_1, L";"}, NamedKey{VK_OEM_7, L"'"},
    NamedKey{VK_OEM_4, L"["}, NamedKey{VK_OEM_6, L"]"},
    NamedKey{VK_OEM_5, L"\\"}, NamedKey{VK_OEM_3, L"`"},
    NamedKey{VK_VOLUME_MUTE, L"VolumeMute"}, NamedKey{VK_VOLUME_DOWN, L"VolumeDown"},
    NamedKey{VK_VOLUME_UP, L"VolumeUp"}, NamedKey{VK_MEDIA_NEXT_TRACK, L"MediaNext"},
    NamedKey{VK_MEDIA_PREV_TRACK, L"MediaPrevious"}, NamedKey{VK_MEDIA_STOP, L"MediaStop"},
    NamedKey{VK_MEDIA_PLAY_PAUSE, L"MediaPlayPause"},
};

} // namespace

WORD normalize_extended_hotkey_key(UINT virtual_key, LPARAM key_data) noexcept {
    if (virtual_key == VK_SHIFT && key_data != 0) {
        const UINT scan_code = static_cast<UINT>((key_data >> 16) & 0xff);
        virtual_key = MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX);
    }
    switch (virtual_key) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return VK_MENU;
    case VK_LWIN: case VK_RWIN: return VK_LWIN;
    default: return static_cast<WORD>(virtual_key);
    }
}

std::wstring extended_hotkey_key_name(WORD key) {
    key = normalize_extended_hotkey_key(key);
    if (key >= L'A' && key <= L'Z') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= L'0' && key <= L'9') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return L"Num" + std::to_wstring(key - VK_NUMPAD0);
    if (key >= VK_F1 && key <= VK_F24)
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    const auto found = std::find_if(kNamedKeys.begin(), kNamedKeys.end(), [key](const NamedKey& item) {
        return item.key == key;
    });
    return found == kNamedKeys.end() ? std::wstring{} : std::wstring(found->canonical);
}

std::optional<WORD> extended_hotkey_key_from_name(std::wstring_view raw_value) {
    auto value = lower(trim(raw_value));
    if (value.empty()) return std::nullopt;

    static const std::unordered_map<std::wstring, int> aliases{
        {L"control", VK_CONTROL}, {L"windows", VK_LWIN},
        {L"return", VK_RETURN}, {L"escape", VK_ESCAPE},
        {L"back", VK_BACK}, {L"del", VK_DELETE}, {L"ins", VK_INSERT},
        {L"prior", VK_PRIOR}, {L"next", VK_NEXT},
        {L"add", VK_ADD}, {L"subtract", VK_SUBTRACT}, {L"multiply", VK_MULTIPLY},
        {L"divide", VK_DIVIDE}, {L"decimal", VK_DECIMAL}, {L"separator", VK_SEPARATOR},
    };
    if (const auto found = aliases.find(value); found != aliases.end())
        return static_cast<WORD>(found->second);
    for (const auto& item : kNamedKeys) {
        if (lower(item.canonical) == value) return item.key;
    }
    if (value.size() == 1 && ((value[0] >= L'a' && value[0] <= L'z')
        || (value[0] >= L'0' && value[0] <= L'9')))
        return static_cast<WORD>(std::towupper(value[0]));
    if (value.starts_with(L"num") && value.size() == 4 && value[3] >= L'0' && value[3] <= L'9')
        return static_cast<WORD>(VK_NUMPAD0 + value[3] - L'0');
    if (value.starts_with(L'f') && value.size() >= 2) {
        wchar_t* end = nullptr;
        const long number = std::wcstol(value.c_str() + 1, &end, 10);
        if (end && *end == 0 && number >= 1 && number <= 24)
            return static_cast<WORD>(VK_F1 + number - 1);
    }
    return std::nullopt;
}

std::vector<WORD> canonicalize_extended_hotkey_keys(std::span<const WORD> keys) {
    std::vector<WORD> result;
    result.reserve(keys.size());
    for (const WORD raw_key : keys) {
        const WORD key = normalize_extended_hotkey_key(raw_key);
        if (extended_hotkey_key_name(key).empty()
            || std::find(result.begin(), result.end(), key) != result.end()) continue;
        result.push_back(key);
    }
    std::sort(result.begin(), result.end(), [](WORD left, WORD right) {
        const int left_order = modifier_order(left), right_order = modifier_order(right);
        if (left_order != right_order) return left_order < right_order;
        return lower(extended_hotkey_key_name(left)) < lower(extended_hotkey_key_name(right));
    });
    return result;
}

std::vector<WORD> parse_extended_hotkey(std::wstring_view value) {
    std::vector<WORD> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(L'+', start);
        const auto part = value.substr(start,
            end == std::wstring_view::npos ? value.size() - start : end - start);
        if (const auto key = extended_hotkey_key_from_name(part)) result.push_back(*key);
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::wstring format_extended_hotkey(std::span<const WORD> keys) {
    const auto canonical = canonicalize_extended_hotkey_keys(keys);
    std::wstring result;
    for (const WORD key : canonical) {
        const auto name = extended_hotkey_key_name(key);
        if (name.empty()) continue;
        if (!result.empty()) result.push_back(L'+');
        result += name;
    }
    return result;
}

void HotkeyRecordingSession::begin() {
    recording_ = true;
    keys_.clear();
}

void HotkeyRecordingSession::cancel() {
    recording_ = false;
    keys_.clear();
}

bool HotkeyRecordingSession::add(UINT virtual_key, LPARAM key_data) {
    if (!recording_) return false;
    const WORD key = normalize_extended_hotkey_key(virtual_key, key_data);
    if (extended_hotkey_key_name(key).empty()) return false;
    if (std::find(keys_.begin(), keys_.end(), key) != keys_.end()) return false;
    keys_.push_back(key);
    keys_ = canonicalize_extended_hotkey_keys(keys_);
    return true;
}

std::wstring HotkeyRecordingSession::finish() {
    const auto result = format_extended_hotkey(keys_);
    recording_ = false;
    keys_.clear();
    return result;
}

std::wstring HotkeyRecordingSession::display_text() const {
    return format_extended_hotkey(keys_);
}

} // namespace smk::windows
