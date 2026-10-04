#include "platform.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace deskedge {
void native_drag(HWND owner, const Json &payload, const std::vector<std::string> &paths) {
    auto data = shell_data(paths, payload);
    DWORD effect = 0;
    auto hr = SHDoDragDrop(owner, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
                           &effect);
    if (FAILED(hr) && hr != DRAGDROP_S_CANCEL)
        throw std::runtime_error("Windows drag: " + win_error(static_cast<DWORD>(hr)));
}
class DropTarget final : public IDropTarget {
    LONG refs = 1;
    Window &window;
    ComPtr<IDataObject> object;
    ComPtr<IDropTarget> native;
    Json payload;
    bool internal = false;
    std::vector<std::string> files;
    fs::path destination;
    std::unordered_set<std::string> before;
    std::unordered_map<std::string, bool> native_handlers;
    DWORD allowed = 0;
    DWORD drag_button = MK_LBUTTON;
    void hover(POINTL p) {
        window.drop_point = {p.x, p.y};
        ScreenToClient(window.hwnd, &window.drop_point);
        window.drop_hover = true;
        window.invalidate();
    }
    bool native_handler(const Hotspot &spot) {
        if (spot.folder)
            return true;
        auto [entry, inserted] = native_handlers.try_emplace(spot.path, false);
        if (inserted)
            try {
                entry->second = shell_drop_target(wide(spot.path), window.hwnd) != nullptr;
            } catch (...) {
                // Ordinary files without a Shell drop handler are grid occupants.
            }
        return entry->second;
    }
    bool metadata(DWORD keys) {
        if (!internal || keys & (MK_CONTROL | MK_SHIFT) || GetKeyState(VK_MENU) & 0x8000)
            return false;
        if (window.kind == Kind::Zone)
            for (auto &spot : window.hotspots)
                if (!spot.path.empty() && PtInRect(&spot.rect, window.drop_point) &&
                    std::none_of(files.begin(), files.end(),
                                 [&](auto &path) { return path_equal(path, spot.path); }) &&
                    native_handler(spot))
                    return false;
        return true;
    }
    fs::path folder_at(POINTL p) {
        POINT point{p.x, p.y};
        ScreenToClient(window.hwnd, &point);
        if (window.kind == Kind::Zone)
            for (auto &spot : window.hotspots)
                if (!spot.path.empty() && PtInRect(&spot.rect, point))
                    return wide(spot.path);
        return window.app.roots.at(0);
    }
    HRESULT target(DWORD keys, POINTL p, DWORD *effect) {
        auto path = folder_at(p);
        if (native && path_equal(pathstr(path), pathstr(destination)))
            return native->DragOver(keys, p, effect);
        if (native)
            native->DragLeave();
        native.Reset();
        destination = path;
        try {
            native = shell_drop_target(destination, window.hwnd);
        } catch (...) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        return native->DragEnter(object.Get(), keys, p, effect);
    }
    void regroup(DWORD effect) {
        auto &a = window.app;
        if (a.self_test_ui)
            write_log(a.engine.data_dir, "Shell drop: syncing model");
        a.engine.sync(a.roots, a.archive_root);
        std::vector<std::string> imported;
        if (window.kind == Kind::Zone && path_equal(pathstr(destination), pathstr(a.roots.at(0)))) {
            std::error_code ec;
            for (auto &e : fs::directory_iterator(destination, ec)) {
                auto path = pathstr(e.path());
                bool present = before.contains(lower(path));
                bool source =
                    (effect & DROPEFFECT_MOVE) &&
                    std::any_of(files.begin(), files.end(), [&](auto &f) { return path_equal(f, path); });
                if (!present || source) {
                    auto ident = a.engine.add_file(e.path(), window.zone_id);
                    if (!ident.empty())
                        imported.push_back(ident);
                }
            }
            if (auto cell = window.grid_at(window.drop_point); cell && !imported.empty())
                a.engine.place_grid(imported, window.zone_id, *cell, window.grid_columns);
        }
        a.commit();
    }

  public:
    explicit DropTarget(Window &w) : window(w) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ptr) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *ptr = this;
            AddRef();
            return S_OK;
        }
        *ptr = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = InterlockedDecrement(&refs);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL p, DWORD *effect) override {
        try {
            object = data;
            native_handlers.clear();
            payload = data_payload(data);
            files = data_files(data);
            auto type = payload.is_object() ? payload.value("type", "") : "";
            internal = (window.kind == Kind::Zone && type == "desktop") ||
                       (window.kind == Kind::Launcher && type == "launcher");
            allowed = *effect;
            drag_button = keys & (MK_LBUTTON | MK_RBUTTON);
            if (!drag_button)
                drag_button = MK_LBUTTON;
            hover(p);
            if (window.kind == Kind::Launcher || metadata(keys)) {
                *effect = files.empty() ? DROPEFFECT_NONE
                                        : allowed & (internal ? DROPEFFECT_MOVE : DROPEFFECT_LINK);
                return S_OK;
            }
            return target(keys, p, effect);
        } catch (...) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL p, DWORD *effect) override {
        if (keys & (MK_LBUTTON | MK_RBUTTON))
            drag_button = keys & (MK_LBUTTON | MK_RBUTTON);
        hover(p);
        if (!object) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        if (window.kind == Kind::Launcher || metadata(keys)) {
            if (native) {
                native->DragLeave();
                native.Reset();
            }
            *effect =
                files.empty() ? DROPEFFECT_NONE : allowed & (internal ? DROPEFFECT_MOVE : DROPEFFECT_LINK);
            return S_OK;
        }
        return target(keys, p, effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (native)
            native->DragLeave();
        native.Reset();
        object.Reset();
        window.drop_hover = false;
        window.invalidate();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL p, DWORD *effect) override {
        auto &a = window.app;
        window.drop_point = {p.x, p.y};
        ScreenToClient(window.hwnd, &window.drop_point);
        window.drop_hover = false;
        try {
            if (window.kind == Kind::Launcher || metadata(keys)) {
                POINT point{p.x, p.y};
                ScreenToClient(window.hwnd, &point);
                a.drop(window, payload, files, point);
                *effect = files.empty() ? DROPEFFECT_NONE
                                        : allowed & (internal ? DROPEFFECT_MOVE : DROPEFFECT_LINK);
            } else {
                *effect = allowed;
                // Preserve the Shell's last DragOver state. Calling DragOver after the mouse button
                // has been released makes the Shell treat a left drag as a right drag.
                if (!native)
                    target(keys | drag_button, p, effect);
                before.clear();
                std::error_code ec;
                if (fs::is_directory(destination, ec))
                    for (auto &entry : fs::directory_iterator(destination, ec))
                        before.insert(lower(pathstr(entry.path())));
                if (native && *effect) {
                    a.modal = true;
                    if (a.self_test_ui)
                        write_log(a.engine.data_dir, "Shell drop: invoking native Drop");
                    auto hr = native->Drop(data, keys | drag_button, p, effect);
                    if (a.self_test_ui)
                        write_log(a.engine.data_dir,
                                  "Shell drop: native Drop returned " + std::to_string(hr));
                    a.modal = false;
                    if (FAILED(hr))
                        throw std::runtime_error("Windows drop: " + win_error(static_cast<DWORD>(hr)));
                    regroup(*effect);
                }
            }
        } catch (const std::exception &ex) {
            a.modal = false;
            a.error(ex.what());
            *effect = DROPEFFECT_NONE;
        }
        native.Reset();
        object.Reset();
        window.invalidate();
        return S_OK;
    }
};
IDropTarget *create_drop_target(Window &window) {
    return new DropTarget(window);
}
} // namespace deskedge
