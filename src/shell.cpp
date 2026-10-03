#include "platform.h"
#include <commctrl.h>
#include <stdexcept>

namespace deskedge {
namespace {
void shell_check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Windows Shell: " + win_error(static_cast<DWORD>(hr)));
}
struct Pidls {
    std::vector<PIDLIST_ABSOLUTE> values;
    explicit Pidls(const std::vector<std::string> &paths) {
        try {
            for (auto &path : paths) {
                PIDLIST_ABSOLUTE item = nullptr;
                shell_check(SHParseDisplayName(wide(path).c_str(), nullptr, &item, 0, nullptr));
                values.push_back(item);
            }
        } catch (...) {
            for (auto item : values)
                CoTaskMemFree(item);
            throw;
        }
    }
    ~Pidls() {
        for (auto item : values)
            CoTaskMemFree(item);
    }
    ComPtr<IShellItemArray> array() {
        ComPtr<IShellItemArray> result;
        shell_check(SHCreateShellItemArrayFromIDLists(
            static_cast<UINT>(values.size()), const_cast<PCIDLIST_ABSOLUTE *>(values.data()), &result));
        return result;
    }
};
void put_bytes(IDataObject *data, UINT format, const void *bytes, size_t length) {
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, length);
    if (!medium.hGlobal)
        throw std::bad_alloc();
    auto target = GlobalLock(medium.hGlobal);
    memcpy(target, bytes, length);
    GlobalUnlock(medium.hGlobal);
    FORMATETC f{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    auto hr = data->SetData(&f, &medium, TRUE);
    if (FAILED(hr))
        ReleaseStgMedium(&medium);
    shell_check(hr);
}
struct MenuMessages {
    ComPtr<IContextMenu2> two;
    ComPtr<IContextMenu3> three;
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        auto self = reinterpret_cast<MenuMessages *>(ref);
        if (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR) {
            LRESULT result = 0;
            if (self->three && SUCCEEDED(self->three->HandleMenuMsg2(msg, wp, lp, &result)))
                return result;
            if (msg != WM_MENUCHAR && self->two && SUCCEEDED(self->two->HandleMenuMsg(msg, wp, lp)))
                return msg == WM_INITMENUPOPUP ? 0 : TRUE;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
};
// Shell extensions can retain pointers into the selection until the menu is released.
// Keep the folder/selection/PIDL storage alive for the entire COM menu lifetime.
class ContextOwner final : public IContextMenu3 {
    LONG refs = 1;

  public:
    std::unique_ptr<Pidls> pidls;
    ComPtr<IShellItemArray> items;
    ComPtr<IShellItem> folder;
    ComPtr<IShellFolder> shell_folder;
    ComPtr<IContextMenu> context;
    ComPtr<IContextMenu2> two;
    ComPtr<IContextMenu3> three;
    ~ContextOwner() {
        three.Reset();
        two.Reset();
        context.Reset();
        items.Reset();
        shell_folder.Reset();
        folder.Reset();
        pidls.reset();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IContextMenu || (iid == IID_IContextMenu2 && two) ||
            (iid == IID_IContextMenu3 && three)) {
            *out = static_cast<IContextMenu3 *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = InterlockedDecrement(&refs);
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last,
                                               UINT flags) override {
        return context->QueryContextMenu(menu, index, first, last, flags);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO *info) override {
        return context->InvokeCommand(info);
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR command, UINT flags, UINT *reserved, LPSTR text,
                                               UINT count) override {
        return context->GetCommandString(command, flags, reserved, text, count);
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT msg, WPARAM wp, LPARAM lp) override {
        return two ? two->HandleMenuMsg(msg, wp, lp) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) override {
        return three ? three->HandleMenuMsg2(msg, wp, lp, result) : E_NOTIMPL;
    }
};
void invoke(HWND owner, IContextMenu *context, const char *verb, UINT offset = 0, bool numeric = false) {
    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        info.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        info.fMask |= CMIC_MASK_SHIFT_DOWN;
    info.hwnd = owner;
    auto unicode = numeric ? std::wstring() : wide(verb);
    info.lpVerb = numeric ? MAKEINTRESOURCEA(offset) : verb;
    info.lpVerbW = numeric ? MAKEINTRESOURCEW(offset) : unicode.c_str();
    info.nShow = SW_SHOWNORMAL;
    GetCursorPos(&info.ptInvoke);
    shell_check(context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO *>(&info)));
}
} // namespace

ComPtr<IDataObject> shell_data(const std::vector<std::string> &paths, const Json &payload) {
    Pidls pidls(paths);
    auto items = pidls.array();
    ComPtr<IDataObject> result;
    shell_check(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&result)));
    if (payload.is_object()) {
        auto text = payload.dump();
        put_bytes(result.Get(), RegisterClipboardFormatW(L"DeskEdge.Item"), text.c_str(), text.size() + 1);
    }
    return result;
}
ComPtr<IContextMenu> shell_context(HWND owner, const std::vector<std::string> &paths,
                                   const fs::path &background) {
    auto storage = std::make_unique<ContextOwner>();
    ComPtr<IContextMenu> result;
    if (!paths.empty()) {
        storage->pidls = std::make_unique<Pidls>(paths);
        bool same_parent = true;
        auto parent = fs::path(wide(paths[0])).parent_path();
        for (auto &path : paths)
            if (!path_equal(pathstr(parent), pathstr(fs::path(wide(path)).parent_path())))
                same_parent = false;
        if (same_parent) {
            PCUITEMID_CHILD child = nullptr;
            shell_check(
                SHBindToParent(storage->pidls->values[0], IID_PPV_ARGS(&storage->shell_folder), &child));
            std::vector<PCUITEMID_CHILD> children;
            for (auto pidl : storage->pidls->values)
                children.push_back(ILFindLastID(pidl));
            shell_check(storage->shell_folder->GetUIObjectOf(
                owner, static_cast<UINT>(children.size()), children.data(), IID_IContextMenu, nullptr,
                reinterpret_cast<void **>(result.GetAddressOf())));
        } else {
            storage->items = storage->pidls->array();
            shell_check(storage->items->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&result)));
        }
    } else {
        shell_check(SHCreateItemFromParsingName(background.c_str(), nullptr, IID_PPV_ARGS(&storage->folder)));
        shell_check(
            storage->folder->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&storage->shell_folder)));
        shell_check(storage->shell_folder->CreateViewObject(owner, IID_PPV_ARGS(&result)));
    }
    storage->context = result;
    result.As(&storage->two);
    result.As(&storage->three);
    result.Reset();
    result.Attach(storage.release());
    return result;
}
ComPtr<IDropTarget> shell_drop_target(const fs::path &folder, HWND owner) {
    Pidls pidls({pathstr(folder)});
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    shell_check(SHBindToParent(pidls.values[0], IID_PPV_ARGS(&parent), &child));
    ComPtr<IDropTarget> result;
    shell_check(parent->GetUIObjectOf(owner, 1, &child, IID_IDropTarget, nullptr,
                                      reinterpret_cast<void **>(result.GetAddressOf())));
    return result;
}
UINT shell_menu(HWND owner, IContextMenu *context, HMENU extra, std::string *verb) {
    constexpr UINT first = 1, last = 0x6FFF;
    auto menu = CreatePopupMenu();
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        flags |= CMF_EXTENDEDVERBS;
    auto hr = context->QueryContextMenu(menu, 0, first, last, flags);
    if (FAILED(hr)) {
        DestroyMenu(menu);
        if (extra)
            DestroyMenu(extra);
        shell_check(hr);
    }
    if (extra) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(extra), L"DeskEdge");
    }
    MenuMessages messages;
    context->QueryInterface(IID_PPV_ARGS(&messages.two));
    context->QueryInterface(IID_PPV_ARGS(&messages.three));
    SetWindowSubclass(owner, MenuMessages::proc, 0xDE5C, reinterpret_cast<DWORD_PTR>(&messages));
    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(owner);
    auto selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, owner, nullptr);
    RemoveWindowSubclass(owner, MenuMessages::proc, 0xDE5C);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (selected >= first && selected <= last) {
        char name[256]{};
        context->GetCommandString(selected - first, GCS_VERBA, nullptr, name, sizeof(name));
        if (verb)
            *verb = name;
        // Rename is an inline operation supplied by the view, just as in Explorer.
        if (strcmp(name, "rename") != 0)
            invoke(owner, context, nullptr, selected - first, true);
        return 0;
    }
    return selected;
}
void shell_verb(HWND owner, const std::vector<std::string> &paths, const char *verb) {
    auto context = shell_context(owner, paths);
    auto menu = CreatePopupMenu();
    auto hr = context->QueryContextMenu(menu, 0, 1, 0x6FFF, CMF_NORMAL | CMF_CANRENAME);
    DestroyMenu(menu);
    shell_check(hr);
    invoke(owner, context.Get(), verb);
}
void shell_folder_verb(HWND owner, const fs::path &folder, const char *verb) {
    auto context = shell_context(owner, {}, folder);
    auto menu = CreatePopupMenu();
    auto hr = context->QueryContextMenu(menu, 0, 1, 0x6FFF, CMF_NORMAL);
    DestroyMenu(menu);
    shell_check(hr);
    for (UINT n = 0; n < static_cast<UINT>(HRESULT_CODE(hr)); ++n) {
        char value[256]{};
        if (SUCCEEDED(context->GetCommandString(n, GCS_VERBA, nullptr, value, sizeof(value))) &&
            strcmp(value, verb) == 0) {
            invoke(owner, context.Get(), verb);
            break;
        }
    }
}
void shell_rename(HWND owner, const fs::path &path, const std::string &name) {
    shell_rename_batch(owner, {{path, name}});
}
void shell_rename_batch(HWND owner, const std::vector<std::pair<fs::path, std::string>> &files) {
    ComPtr<IFileOperation> operation;
    shell_check(
        CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation)));
    shell_check(operation->SetOwnerWindow(owner));
    shell_check(operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR));
    for (auto &[path, name] : files) {
        ComPtr<IShellItem> item;
        shell_check(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)));
        shell_check(operation->RenameItem(item.Get(), wide(name).c_str(), nullptr));
    }
    shell_check(operation->PerformOperations());
}
std::vector<std::string> data_files(IDataObject *data) {
    std::vector<std::string> files;
    ComPtr<IShellItemArray> items;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items)))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD n = 0; n < count; ++n) {
            ComPtr<IShellItem> item;
            items->GetItemAt(n, &item);
            PWSTR path = nullptr;
            if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                files.push_back(utf8(path));
                CoTaskMemFree(path);
            }
        }
    }
    return files;
}
Json data_payload(IDataObject *data) {
    FORMATETC f{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DeskEdge.Item")), nullptr,
                DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    Json result;
    if (SUCCEEDED(data->GetData(&f, &medium))) {
        auto bytes = static_cast<const char *>(GlobalLock(medium.hGlobal));
        auto size = GlobalSize(medium.hGlobal);
        if (bytes && size) {
            auto end = static_cast<const char *>(memchr(bytes, 0, size));
            if (end)
                result = Json::parse(bytes, end, nullptr, false);
        }
        if (bytes)
            GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    return result;
}
} // namespace deskedge
