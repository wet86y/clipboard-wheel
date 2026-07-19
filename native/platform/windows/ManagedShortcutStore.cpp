#include "platform/windows/ManagedShortcutStore.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <set>

namespace smk::windows {
namespace {

std::filesystem::path default_root() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result / L"超级中键" / L"shortcuts";
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::filesystem::path normalized_absolute(const std::filesystem::path& value) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(value, error);
    if (!error) return canonical;
    error.clear();
    auto absolute = std::filesystem::absolute(value, error);
    return (error ? value : absolute).lexically_normal();
}

bool path_within(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    const auto child = lower(normalized_absolute(candidate).native());
    auto parent = lower(normalized_absolute(root).native());
    if (parent.empty() || child.size() <= parent.size()) return false;
    if (parent.back() != L'\\' && parent.back() != L'/') parent.push_back(L'\\');
    return child.starts_with(parent);
}

std::wstring make_generation() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return std::to_wstring(GetTickCount64());
    wchar_t value[40]{};
    if (!StringFromGUID2(guid, value, static_cast<int>(std::size(value))))
        return std::to_wstring(GetTickCount64());
    std::wstring result(value);
    std::erase_if(result, [](wchar_t ch) { return ch == L'{' || ch == L'}' || ch == L'-'; });
    return result;
}

std::set<std::wstring> managed_references(
    const smk::core::AppSettings& settings, const std::filesystem::path& root) {
    std::set<std::wstring> result;
    for (const auto& slot : settings.wheel.extended_wheel.slots) {
        if (!slot.shortcut_path.empty() && path_within(slot.shortcut_path, root))
            result.insert(lower(normalized_absolute(slot.shortcut_path).native()));
    }
    return result;
}

} // namespace

ManagedShortcutStore::ManagedShortcutStore() : root_(default_root()) {}

ManagedShortcutStore::ManagedShortcutStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::optional<std::wstring> ManagedShortcutStore::create_candidate(
    const std::wstring& source_path, int slot_index, std::wstring& error) const {
    error.clear();
    try {
        const std::filesystem::path source(source_path);
        std::error_code file_error;
        if (root_.empty() || source.is_relative() || !std::filesystem::exists(source, file_error) || file_error) {
            error = L"拖入的程序、文件或文件夹不存在。";
            return std::nullopt;
        }

        const bool source_is_directory = std::filesystem::is_directory(source, file_error);
        auto display_name = source.filename().wstring();
        if (display_name.empty()) display_name = L"快捷方式";
        if (!source_is_directory && source.has_extension()) display_name = source.stem().wstring();
        if (slot_index < 0 || slot_index >= smk::core::kExtendedSlotCount) {
            error = L"快捷方式槽位无效。";
            return std::nullopt;
        }
        const auto slot_directory = root_ / std::format(L"slot-{:02}", slot_index + 1);
        std::filesystem::create_directories(slot_directory, file_error);
        if (!file_error && !path_within(slot_directory, root_))
            file_error = std::make_error_code(std::errc::permission_denied);
        if (file_error) {
            error = L"无法创建超级中键快捷方式目录。";
            return std::nullopt;
        }
        const auto directory = slot_directory / make_generation();
        std::filesystem::create_directory(directory, file_error);
        if (file_error || !path_within(directory, root_)) {
            error = L"无法创建超级中键快捷方式目录。";
            return std::nullopt;
        }
        const auto shortcut = directory / (display_name + L".lnk");

        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persist;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(link.GetAddressOf())))
            || FAILED(link.As(&persist))
            || FAILED(link->SetPath(source.c_str()))) {
            error = L"无法创建 Windows 快捷方式。";
            discard(shortcut.wstring());
            return std::nullopt;
        }
        if (!source_is_directory)
            (void)link->SetWorkingDirectory(source.parent_path().c_str());
        (void)link->SetDescription(L"由超级中键自动创建");
        if (FAILED(persist->Save(shortcut.c_str(), TRUE))) {
            error = L"无法保存 Windows 快捷方式。";
            discard(shortcut.wstring());
            return std::nullopt;
        }
        return shortcut.wstring();
    } catch (...) {
        error = L"创建 Windows 快捷方式时发生错误。";
        return std::nullopt;
    }
}

bool ManagedShortcutStore::is_managed(const std::wstring& path) const noexcept {
    try { return !path.empty() && path_within(path, root_); }
    catch (...) { return false; }
}

void ManagedShortcutStore::discard(const std::wstring& path) const noexcept {
    try {
        if (!is_managed(path)) return;
        std::error_code error;
        std::filesystem::remove(path, error);
        auto directory = std::filesystem::path(path).parent_path();
        while (path_within(directory, root_)) {
            error.clear();
            if (!std::filesystem::remove(directory, error) || error) break;
            directory = directory.parent_path();
        }
    } catch (...) {}
}

void ManagedShortcutStore::reconcile(const smk::core::AppSettings& previous,
    const smk::core::AppSettings& accepted,
    const std::vector<std::wstring>& candidates) const noexcept {
    try {
        const auto retained = managed_references(accepted, root_);
        for (const auto& slot : previous.wheel.extended_wheel.slots) {
            if (is_managed(slot.shortcut_path)
                && !retained.contains(lower(normalized_absolute(slot.shortcut_path).native())))
                discard(slot.shortcut_path);
        }
        for (const auto& candidate : candidates) {
            if (is_managed(candidate)
                && !retained.contains(lower(normalized_absolute(candidate).native()))) discard(candidate);
        }
    } catch (...) {}
}

void ManagedShortcutStore::cleanup_unreferenced(const smk::core::AppSettings& settings) const noexcept {
    try {
        if (root_.empty() || !std::filesystem::exists(root_)) return;
        const auto retained = managed_references(settings, root_);
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(root_, error), end; it != end && !error; it.increment(error)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                && it->is_directory(error)) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(error) && !error && path_within(it->path(), root_))
                files.push_back(it->path());
        }
        for (const auto& file : files) {
            if (!retained.contains(lower(normalized_absolute(file).native()))) discard(file.wstring());
        }
    } catch (...) {}
}

} // namespace smk::windows
