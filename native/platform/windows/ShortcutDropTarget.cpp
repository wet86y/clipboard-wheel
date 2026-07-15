#include "platform/windows/ShortcutDropTarget.h"

#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <new>
#include <optional>
#include <utility>

namespace smk::windows {
namespace {

std::optional<std::wstring> shortcut_from_data_object(IDataObject* data) noexcept {
    if (!data) return std::nullopt;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) return std::nullopt;
    std::optional<std::wstring> result;
    const auto drop = reinterpret_cast<HDROP>(medium.hGlobal);
    if (drop) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        if (count == 1) {
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            if (DragQueryFileW(drop, 0, path.data(), length + 1) == length) {
                path.resize(length);
                if (is_valid_shortcut_drop_path(path)) result = std::move(path);
            }
        }
    }
    ReleaseStgMedium(&medium);
    return result;
}

class ShortcutDropTarget final : public IDropTarget {
public:
    explicit ShortcutDropTarget(ShortcutDropCallbacks callbacks) : callbacks_(std::move(callbacks)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        pending_path_ = !callbacks_.enabled || callbacks_.enabled()
            ? shortcut_from_data_object(data) : std::nullopt;
        set_state(pending_path_ ? ShortcutDropVisualState::accept : ShortcutDropVisualState::reject);
        if (effect) *effect = pending_path_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect) *effect = pending_path_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        pending_path_.reset();
        set_state(ShortcutDropVisualState::idle);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        auto path = !callbacks_.enabled || callbacks_.enabled()
            ? shortcut_from_data_object(data) : std::nullopt;
        pending_path_.reset();
        if (!path) {
            set_state(ShortcutDropVisualState::reject);
            if (effect) *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        set_state(ShortcutDropVisualState::success);
        if (effect) *effect = DROPEFFECT_COPY;
        if (callbacks_.shortcut_dropped) callbacks_.shortcut_dropped(*path);
        return S_OK;
    }

private:
    ~ShortcutDropTarget() = default;
    void set_state(ShortcutDropVisualState state) {
        if (state_ == state) return;
        state_ = state;
        if (callbacks_.state_changed) callbacks_.state_changed(state);
    }

    std::atomic<ULONG> references_{1};
    ShortcutDropCallbacks callbacks_;
    std::optional<std::wstring> pending_path_;
    ShortcutDropVisualState state_ = ShortcutDropVisualState::idle;
};

} // namespace

bool is_valid_shortcut_drop_path(const std::wstring& path) noexcept {
    try {
        if (path.empty() || std::filesystem::path(path).is_relative()) return false;
        const auto extension = std::filesystem::path(path).extension().wstring();
        if (_wcsicmp(extension.c_str(), L".lnk") != 0) return false;
        std::error_code ignored;
        return std::filesystem::is_regular_file(path, ignored) && !ignored;
    } catch (...) {
        return false;
    }
}

IDropTarget* create_shortcut_drop_target(ShortcutDropCallbacks callbacks) noexcept {
    return new (std::nothrow) ShortcutDropTarget(std::move(callbacks));
}

ShortcutDropRegistration::~ShortcutDropRegistration() { reset(); }

ShortcutDropRegistration::ShortcutDropRegistration(ShortcutDropRegistration&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)), target_(std::exchange(other.target_, nullptr)) {}

ShortcutDropRegistration& ShortcutDropRegistration::operator=(ShortcutDropRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        window_ = std::exchange(other.window_, nullptr);
        target_ = std::exchange(other.target_, nullptr);
    }
    return *this;
}

bool ShortcutDropRegistration::register_window(
    HWND window, ShortcutDropCallbacks callbacks, HRESULT* error) noexcept {
    reset();
    if (!IsWindow(window)) {
        if (error) *error = E_INVALIDARG;
        return false;
    }
    auto* target = create_shortcut_drop_target(std::move(callbacks));
    if (!target) {
        if (error) *error = E_OUTOFMEMORY;
        return false;
    }
    const HRESULT result = RegisterDragDrop(window, target);
    if (FAILED(result)) {
        target->Release();
        if (error) *error = result;
        return false;
    }
    window_ = window;
    target_ = target;
    if (error) *error = S_OK;
    return true;
}

void ShortcutDropRegistration::reset() noexcept {
    if (window_) (void)RevokeDragDrop(window_);
    if (target_) target_->Release();
    window_ = nullptr;
    target_ = nullptr;
}

} // namespace smk::windows
