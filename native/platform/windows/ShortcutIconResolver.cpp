#include "platform/windows/ShortcutIconResolver.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace smk::windows {
namespace {

using Microsoft::WRL::ComPtr;

struct CacheEntry {
    HICON icon = nullptr;

    CacheEntry() = default;
    explicit CacheEntry(HICON value) noexcept : icon(value) {}
    CacheEntry(const CacheEntry&) = delete;
    CacheEntry& operator=(const CacheEntry&) = delete;
    CacheEntry(CacheEntry&& other) noexcept : icon(std::exchange(other.icon, nullptr)) {}
    CacheEntry& operator=(CacheEntry&& other) noexcept {
        if (this != &other) {
            if (icon) DestroyIcon(icon);
            icon = std::exchange(other.icon, nullptr);
        }
        return *this;
    }
    ~CacheEntry() { if (icon) DestroyIcon(icon); }
};

std::mutex cache_mutex;
std::unordered_map<std::wstring, CacheEntry> icon_cache;

std::wstring expand_environment(const std::wstring& value) {
    if (value.empty()) return {};
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) return value;
    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (written == 0 || written > required) return value;
    expanded.resize(written > 0 ? written - 1 : 0);
    return expanded;
}

std::wstring normalized_path(const std::wstring& value, const std::filesystem::path& base = {}) {
    if (value.empty()) return {};
    std::wstring expanded = expand_environment(value);
    if (expanded.size() >= 2 && expanded.front() == L'"' && expanded.back() == L'"')
        expanded = expanded.substr(1, expanded.size() - 2);
    std::filesystem::path path(expanded);
    if (path.is_relative() && !base.empty()) path = base / path;
    std::error_code ignored;
    const auto absolute = std::filesystem::absolute(path, ignored);
    if (!ignored) path = absolute;
    path = path.lexically_normal();
    auto result = path.wstring();
    std::transform(result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

unsigned long long write_stamp(const std::wstring& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
}

std::wstring cache_key(const std::wstring& shortcut_path, int size) {
    const auto normalized = normalized_path(shortcut_path);
    return normalized + L"|" + std::to_wstring(size) + L"|" +
        std::to_wstring(write_stamp(normalized));
}

HICON extract_resource_icon(const std::wstring& path, int index, int size) noexcept {
    if (path.empty()) return nullptr;
    HICON large = nullptr;
    HICON small_icon = nullptr;
    const UINT packed_size = MAKELONG(static_cast<WORD>(size), static_cast<WORD>(size));
    if (FAILED(SHDefExtractIconW(path.c_str(), index, 0, &large, &small_icon, packed_size))) return nullptr;
    HICON result = large ? large : small_icon;
    if (large && result != large) DestroyIcon(large);
    if (small_icon && result != small_icon) DestroyIcon(small_icon);
    return result;
}

HICON shell_icon(const std::wstring& path, bool use_file_attributes) noexcept {
    if (path.empty()) return nullptr;
    SHFILEINFOW information{};
    UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    if (use_file_attributes) {
        flags |= SHGFI_USEFILEATTRIBUTES;
        if (std::filesystem::path(path).extension().empty()) attributes = FILE_ATTRIBUTE_DIRECTORY;
    }
    if (!SHGetFileInfoW(path.c_str(), attributes, &information, sizeof(information), flags)) return nullptr;
    return information.hIcon;
}

std::wstring shortcut_target(IShellLinkW* link) {
    wchar_t target[32768]{};
    WIN32_FIND_DATAW data{};
    if (SUCCEEDED(link->GetPath(target, static_cast<int>(std::size(target)), &data, SLGP_RAWPATH))
        && target[0] != L'\0') return expand_environment(target);

    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(link->GetIDList(&item)) && item) {
        wchar_t resolved[32768]{};
        const bool ok = SHGetPathFromIDListW(item, resolved) != FALSE;
        CoTaskMemFree(item);
        if (ok) return resolved;
    }
    return {};
}

HICON resolve_uncached(const std::wstring& shortcut_path, int size) noexcept {
    try {
        const auto link_path = normalized_path(shortcut_path);
        const auto link_directory = std::filesystem::path(link_path).parent_path();
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> persist;
        std::wstring target;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(link.GetAddressOf())))
            && SUCCEEDED(link.As(&persist))
            && SUCCEEDED(persist->Load(link_path.c_str(), STGM_READ))) {
            wchar_t icon_location[32768]{};
            int icon_index = 0;
            if (SUCCEEDED(link->GetIconLocation(icon_location,
                    static_cast<int>(std::size(icon_location)), &icon_index))
                && icon_location[0] != L'\0') {
                const auto location = normalized_path(icon_location, link_directory);
                if (auto icon = extract_resource_icon(location, icon_index, size)) return icon;
            }
            target = normalized_path(shortcut_target(link.Get()), link_directory);
        }

        if (!target.empty()) {
            if (auto icon = shell_icon(target, false)) return icon;
            if (auto icon = shell_icon(target, true)) return icon;
        }
        if (auto icon = shell_icon(link_path, false)) return icon;
        return shell_icon(link_path, true);
    } catch (...) {
        return nullptr;
    }
}

} // namespace

HICON resolve_shortcut_icon(const std::wstring& shortcut_path, int desired_size_pixels) noexcept {
    if (shortcut_path.empty()) return nullptr;
    const int size = std::clamp(desired_size_pixels, 16, 256);
    try {
        const auto key = cache_key(shortcut_path, size);
        {
            const std::scoped_lock lock(cache_mutex);
            const auto found = icon_cache.find(key);
            if (found != icon_cache.end()) return found->second.icon ? CopyIcon(found->second.icon) : nullptr;
        }
        HICON resolved = resolve_uncached(shortcut_path, size);
        if (!resolved) return nullptr;
        const std::scoped_lock lock(cache_mutex);
        auto [position, inserted] = icon_cache.try_emplace(key, resolved);
        if (!inserted) DestroyIcon(resolved);
        return position->second.icon ? CopyIcon(position->second.icon) : nullptr;
    } catch (...) {
        return nullptr;
    }
}

void clear_shortcut_icon_cache() noexcept {
    const std::scoped_lock lock(cache_mutex);
    icon_cache.clear();
}

} // namespace smk::windows
