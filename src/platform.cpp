#include "platform.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <array>
#include <commctrl.h>
#include <fstream>
#include <shlwapi.h>
#include <stdexcept>
#include <unordered_set>
#include <wincodec.h>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace deskedge {
static void check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("DirectX: " + win_error(static_cast<DWORD>(result)));
}
std::vector<Monitor> monitors() {
    std::vector<Monitor> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(h, &info);
            UINT x = 96, y = 96;
            GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &x, &y);
            reinterpret_cast<std::vector<Monitor> *>(data)->push_back(
                {utf8(info.szDevice), info.rcWork, info.rcMonitor, x / 96.f});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}
Monitor monitor(std::string name, bool at_cursor) {
    auto list = monitors();
    if (list.empty())
        throw std::runtime_error("No display available");
    POINT cursor{};
    GetCursorPos(&cursor);
    for (auto &m : list) {
        if ((at_cursor && PtInRect(&m.bounds, cursor)) || (!at_cursor && m.name == name))
            return m;
    }
    for (auto &m : list)
        if (m.bounds.left == 0 && m.bounds.top == 0)
            return m;
    return list[0];
}
HWND desktop_view() {
    HWND found = FindWindowExW(FindWindowW(L"Progman", nullptr), nullptr, L"SHELLDLL_DefView", nullptr);
    if (found)
        return found;
    EnumWindows(
        [](HWND hwnd, LPARAM ptr) -> BOOL {
            auto child = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
            if (child) {
                *reinterpret_cast<HWND *>(ptr) = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}
HWND desktop_icons() {
    return FindWindowExW(desktop_view(), nullptr, L"SysListView32", nullptr);
}
bool system_dark() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}
void blur(HWND hwnd, bool dark) {
    struct Accent {
        int state, flags;
        DWORD color;
        int animation;
    };
    struct Data {
        int attr;
        void *data;
        size_t size;
    };
    auto user = GetModuleHandleW(L"user32.dll");
    using Fn = BOOL(WINAPI *)(HWND, Data *);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(user, "SetWindowCompositionAttribute"));
    if (fn) {
        Accent accent{3, 2, dark ? 0x80242422U : 0x60F3F3F3U, 0};
        Data data{19, &accent, sizeof(accent)};
        fn(hwnd, &data);
    }
    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}
fs::path known_folder(REFKNOWNFOLDERID ident) {
    PWSTR path = nullptr;
    check(SHGetKnownFolderPath(ident, 0, nullptr, &path));
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}
void shell_open(std::string path, HWND owner) {
    auto target = wide(path);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.hwnd = owner;
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = target.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        throw std::runtime_error(win_error());
}
void reveal(std::string path) {
    auto arg = L"/select,\"" + wide(path) + L"\"";
    if (reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Cannot show the file in Explorer");
}
std::vector<fs::path> pick_files(HWND owner, bool folder) {
    ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)));
    DWORD flags = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                  (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
    check(dialog->SetOptions(flags));
    auto result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    check(result);
    ComPtr<IShellItemArray> items;
    check(dialog->GetResults(&items));
    DWORD count = 0;
    items->GetCount(&count);
    std::vector<fs::path> paths;
    for (DWORD n = 0; n < count; n++) {
        ComPtr<IShellItem> item;
        items->GetItemAt(n, &item);
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return paths;
}
std::vector<Json> discover_apps() {
    std::vector<Json> result;
    std::unordered_set<std::string> names;
    for (auto folder : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
        try {
            auto root = known_folder(folder);
            std::error_code ec;
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            for (auto &e : it) {
                if (e.is_directory(ec)) {
                    if (e.is_symlink(ec))
                        it.disable_recursion_pending();
                    continue;
                }
                if (lower(pathstr(e.path().extension())) != ".lnk")
                    continue;
                auto name = filename(e.path()), key = lower(name);
                if (key.find("uninstall") != std::string::npos || key.find("卸载") != std::string::npos ||
                    !names.insert(key).second)
                    continue;
                result.push_back({{"id", id()},
                                  {"name", name},
                                  {"path", pathstr(e.path())},
                                  {"kind", "app"},
                                  {"uses", 0},
                                  {"used", 0}});
            }
        } catch (...) {
        }
    }
    return result;
}
void startup(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot update startup preference");
    if (enabled) {
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        std::wstring value = L"\"" + std::wstring(exe) + L"\"";
        auto error =
            RegSetValueExW(key, L"DeskEdge", 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        if (error != ERROR_SUCCESS)
            throw std::runtime_error(win_error(error));
    } else {
        RegDeleteValueW(key, L"DeskEdge");
        RegCloseKey(key);
    }
}
void write_log(const fs::path &folder, std::string message) {
    try {
        fs::create_directories(folder);
        auto file = folder / L"DeskEdge.log";
        std::error_code ec;
        if (fs::exists(file) && fs::file_size(file, ec) > 2 * 1024 * 1024)
            fs::rename(file, folder / L"DeskEdge.previous.log", ec);
        std::ofstream stream(file, std::ios::app);
        stream << now() << " " << message << "\n";
    } catch (...) {
    }
}

struct MappedFont {
    HANDLE mapping = nullptr;
    void *data = nullptr;
    int size = 0;
    explicit MappedFont(const wchar_t *name) {
        auto path = known_folder(FOLDERID_Fonts) / name;
        HANDLE file =
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        LARGE_INTEGER length{};
        if (GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart < INT_MAX) {
            mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping) {
                data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
                if (data)
                    size = static_cast<int>(length.QuadPart);
            }
        }
        CloseHandle(file);
    }
    ~MappedFont() {
        if (data)
            UnmapViewOfFile(data);
        if (mapping)
            CloseHandle(mapping);
    }
};
static void fonts() {
    // Shared, read-only mappings keep unused CJK font pages out of the process's working set.
    static MappedFont latin(L"segoeui.ttf"), chinese(L"msyh.ttc"), symbols(L"seguisym.ttf");
    auto &io = ImGui::GetIO();
    bool first = true;
    for (auto *data : {&latin, &chinese, &symbols})
        if (data->data) {
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;
            cfg.MergeMode = !first;
            io.Fonts->AddFontFromMemoryTTF(data->data, data->size, 13, &cfg);
            first = false;
        }
    if (first)
        io.Fonts->AddFontDefault();
}
Window::Window(Application &a, Kind type, std::string ident) : app(a), kind(type), zone_id(std::move(ident)) {
    screen = monitor();
    scale = screen.scale;
}
Window::~Window() {
    closing = true;
    if (hwnd && IsWindow(hwnd)) {
        if (drop_target)
            RevokeDragDrop(hwnd);
        if (desktop_child) {
            ShowWindow(hwnd, SW_HIDE);
            SetParent(hwnd, nullptr);
        }
        DestroyWindow(hwnd);
    }
    if (drop_target) {
        drop_target->Release();
        drop_target = nullptr;
    }
    if (context) {
        ImGui::SetCurrentContext(context);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(context);
    }
}
void Window::create(bool child) {
    desktop_child = child && desktop_view();
    DWORD style = desktop_child ? WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN : WS_POPUP;
    DWORD ex = ((app.demo && app.no_desktop && kind != Kind::Zone) ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW) |
               WS_EX_NOREDIRECTIONBITMAP;
    std::wstring title = wide("DeskEdge · " + (kind == Kind::Panel      ? app.tr("桌沿", "Panel")
                                               : kind == Kind::Launcher ? app.tr("启动器", "Launcher")
                                               : kind == Kind::Settings ? app.tr("设置", "Settings")
                                               : kind == Kind::Draw     ? app.tr("绘制区域", "Draw zone")
                                                                        : "Zone " + zone_id));
    hwnd =
        CreateWindowExW(ex, L"DeskEdge.Window", title.c_str(), style, 0, 0, 100, 100,
                        desktop_child ? desktop_view() : nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd)
        throw std::runtime_error(win_error());
    scale = GetDpiForWindow(hwnd) / 96.f;
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.MouseDoubleClickTime = GetDoubleClickTime() / 1000.f;
    io.MouseDoubleClickMaxDist =
        static_cast<float>(std::max(GetSystemMetrics(SM_CXDOUBLECLK), GetSystemMetrics(SM_CYDOUBLECLK))) /
        2.f;
    fonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(app.device.Get(), app.device_context.Get());
    theme();
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 100;
        desc.BufferCount = 2;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SampleDesc.Count = 1;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        ComPtr<IDXGIDevice> dxgi;
        check(app.device.As(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        check(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGISwapChain1> chain;
        check(factory->CreateSwapChainForComposition(app.device.Get(), &desc, nullptr, &chain));
        check(chain.As(&swap));
        check(app.composition->CreateTargetForHwnd(hwnd, TRUE, &composition_target));
        check(app.composition->CreateVisual(&visual));
        check(app.composition->CreateEffectGroup(&opacity_effect));
        check(visual->SetEffect(opacity_effect.Get()));
        check(app.composition->CreateRectangleClip(&clip));
        check(visual->SetContent(swap.Get()));
        check(visual->SetClip(clip.Get()));
        check(composition_target->SetRoot(visual.Get()));
        check(app.composition->Commit());
        if (!desktop_child && kind != Kind::Zone)
            blur(hwnd, app.dark);
    }
    drop_target = create_drop_target(*this);
    if (kind != Kind::Draw)
        RegisterDragDrop(hwnd, drop_target);
    resize(100, 100);
}
void Window::resize(int w, int h) {
    if (w <= 0 || h <= 0 || !context || (w == width && h == height))
        return;
    width = w;
    height = h;
    surface_dirty = true;
    invalidate();
}
void Window::update_surface() {
    if (!surface_dirty || !swap)
        return;
    int w = width, h = height;
    app.device_context->OMSetRenderTargets(0, nullptr, nullptr);
    target.Reset();
    texture.Reset();
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(app.device->CreateTexture2D(&desc, nullptr, &texture));
    }
    check(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0));
    if (texture)
        check(app.device->CreateRenderTargetView(texture.Get(), nullptr, &target));
    clip->SetLeft(0.f);
    clip->SetTop(0.f);
    clip->SetRight(static_cast<float>(w));
    clip->SetBottom(static_cast<float>(h));
    float radius = kind == Kind::Draw ? 0.f : 8.f * scale;
    clip->SetTopLeftRadiusX(radius);
    clip->SetTopLeftRadiusY(radius);
    clip->SetTopRightRadiusX(radius);
    clip->SetTopRightRadiusY(radius);
    clip->SetBottomLeftRadiusX(radius);
    clip->SetBottomLeftRadiusY(radius);
    clip->SetBottomRightRadiusX(radius);
    clip->SetBottomRightRadiusY(radius);
    check(app.composition->Commit());
    surface_dirty = false;
    ++surface_allocations;
}
void Window::theme() {
    if (!context)
        return;
    ImGui::SetCurrentContext(context);
    if (app.dark)
        ImGui::StyleColorsDark();
    else
        ImGui::StyleColorsLight();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {12, 10};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {6, 5};
    s.WindowRounding = 8;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 7;
    s.WindowBorderSize = 1;
    s.ScrollbarSize = 7;
    s.GrabRounding = 4;
    s.Colors[ImGuiCol_Text] = app.dark ? ImVec4(.95f, .95f, .95f, 1) : ImVec4(.106f, .106f, .106f, 1);
    s.Colors[ImGuiCol_TextDisabled] = app.dark ? ImVec4(.72f, .72f, .74f, 1) : ImVec4(.365f, .365f, .365f, 1);
    s.Colors[ImGuiCol_WindowBg] =
        app.dark ? ImVec4(.133f, .133f, .141f, .88f) : ImVec4(.953f, .953f, .953f, .86f);
    s.Colors[ImGuiCol_Border] = app.dark ? ImVec4(1, 1, 1, .10f) : ImVec4(0, 0, 0, .09f);
    s.Colors[ImGuiCol_Button] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ButtonHovered] = app.dark ? ImVec4(1, 1, 1, .10f) : ImVec4(0, 0, 0, .06f);
    s.Colors[ImGuiCol_ButtonActive] = app.dark ? ImVec4(1, 1, 1, .16f) : ImVec4(0, 0, 0, .10f);
    s.Colors[ImGuiCol_FrameBg] = app.dark ? ImVec4(1, 1, 1, .07f) : ImVec4(1, 1, 1, .82f);
    s.Colors[ImGuiCol_FrameBgHovered] = app.dark ? ImVec4(1, 1, 1, .12f) : ImVec4(1, 1, 1, .94f);
    s.Colors[ImGuiCol_FrameBgActive] = s.Colors[ImGuiCol_FrameBgHovered];
    s.Colors[ImGuiCol_CheckMark] = app.dark ? ImVec4(.55f, .74f, .95f, 1) : ImVec4(.078f, .416f, .71f, 1);
    s.Colors[ImGuiCol_Header] = s.Colors[ImGuiCol_ButtonHovered];
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
    if (hwnd && !desktop_child && kind != Kind::Zone)
        blur(hwnd, app.dark);
    invalidate();
}
void Window::place(float x, float y, float w, float h, Monitor m, bool activate) {
    if (geometry_capture)
        return;
    screen = m;
    const auto new_scale = m.scale;
    if (scale != new_scale) {
        scale = new_scale;
        theme();
    }
    POINT p{m.work.left + static_cast<LONG>(x * m.scale), m.work.top + static_cast<LONG>(y * m.scale)};
    RECT desired{p.x, p.y, p.x + static_cast<LONG>(w * m.scale), p.y + static_cast<LONG>(h * m.scale)};
    RECT current{};
    GetWindowRect(hwnd, &current);
    if (size_animating && EqualRect(&desired, &layout_bounds))
        return;
    layout_bounds = desired;
    if (EqualRect(&current, &desired))
        return;
    if (kind == Kind::Zone && visible() && animations_enabled() && current.left == desired.left &&
        current.top == desired.top && current.right == desired.right && current.bottom != desired.bottom) {
        size_from = current;
        size_to = desired;
        size_started = static_cast<double>(GetTickCount64());
        size_animating = true;
        SetTimer(hwnd, 5, 16, nullptr);
        return;
    }
    size_animating = false;
    KillTimer(hwnd, 5);
    if (desktop_child)
        ScreenToClient(GetParent(hwnd), &p);
    auto after = desktop_child ? HWND_TOP : kind == Kind::Zone ? HWND_BOTTOM : HWND_TOPMOST;
    SetWindowPos(hwnd, after, p.x, p.y, static_cast<int>(w * m.scale), static_cast<int>(h * m.scale),
                 activate ? 0 : SWP_NOACTIVATE);
    resize(static_cast<int>(w * m.scale), static_cast<int>(h * m.scale));
    ++geometry_updates;
    invalidate();
}
bool Window::visible() const {
    return hwnd && IsWindow(hwnd) && requested_visible;
}
void Window::show(bool activate) {
    bool was_visible = requested_visible;
    requested_visible = true;
    fading_out = false;
    KillTimer(hwnd, 4);
    if (!was_visible && visual) {
        if (animations_enabled())
            animate_opacity(0.f, 1.f, kind == Kind::Panel ? .28f : .20f);
        else {
            opacity_effect->SetOpacity(1.f);
            visual->SetOffsetX(0.f);
            visual->SetOffsetY(0.f);
            opacity_seconds = 0;
            app.composition->Commit();
        }
    }
    ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
    if (!was_visible || activate)
        invalidate();
}
void Window::hide() {
    if (!requested_visible)
        return;
    requested_visible = false;
    if (hwnd && animations_enabled() && visual) {
        fading_out = true;
        animate_opacity(1.f, 0.f, .14f);
        SetTimer(hwnd, 4, 150, nullptr);
    } else if (hwnd)
        ShowWindow(hwnd, SW_HIDE);
}
void Window::invalidate() {
    dirty = true;
    if (hwnd)
        InvalidateRect(hwnd, nullptr, FALSE);
    if (app.controller)
        PostMessageW(app.controller, MSG_WAKE, 0, 0);
}
void Window::render() {
    if (!context || !visible())
        return;
    update_surface();
    if (!target)
        return;
    dirty = false;
    ImGui::SetCurrentContext(context);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (test_input)
        test_input(ImGui::GetIO());
    ImGui::NewFrame();
    hotspots.clear();
    test_controls.clear();
    draw_ui(*this);
    ImGui::Render();
    const float transparent[4] = {0, 0, 0, 0};
    auto rtv = target.Get();
    app.device_context->OMSetRenderTargets(1, &rtv, nullptr);
    app.device_context->ClearRenderTargetView(rtv, transparent);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (swap) {
        ComPtr<ID3D11Texture2D> backbuffer;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));
        app.device_context->CopyResource(backbuffer.Get(), texture.Get());
        auto hr = swap->Present(0, 0);
        if (hr != DXGI_STATUS_OCCLUDED && FAILED(hr))
            throw std::runtime_error("Display device lost: " + win_error(hr));
    }
    ++app.rendered_frames;
    auto &io = ImGui::GetIO();
    if (io.WantTextInput && GetFocus() == hwnd)
        SetTimer(hwnd, 2, 500, nullptr);
    else
        KillTimer(hwnd, 2);
}
void Window::export_preview(const fs::path &path) {
    // Export this application's own render target, without capturing other windows or the desktop.
    if (!texture || !app.demo)
        return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    check(app.device->CreateTexture2D(&desc, nullptr, &copy));
    app.device_context->CopyResource(copy.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    check(app.device_context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const BYTE *src =
                static_cast<const BYTE *>(map.pData) + static_cast<size_t>(y) * map.RowPitch + x * 4;
            BYTE *dst = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            int a = src[3];
            dst[0] = a ? static_cast<BYTE>(std::min(255, src[0] * 255 / a)) : 0;
            dst[1] = a ? static_cast<BYTE>(std::min(255, src[1] * 255 / a)) : 0;
            dst[2] = a ? static_cast<BYTE>(std::min(255, src[2] * 255 / a)) : 0;
            dst[3] = static_cast<BYTE>(a);
        }
    app.device_context->Unmap(copy.Get(), 0);
    fs::create_directories(path.parent_path());
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format));
    check(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    check(frame->Commit());
    check(encoder->Commit());
}
LRESULT CALLBACK Window::procedure(HWND handle, UINT message, WPARAM wp, LPARAM lp) {
    // IME and focus changes can send synchronous messages to another DeskEdge HWND during Render().
    // Restore the caller's ImGui context on every return, including backend-handled messages.
    struct RestoreContext {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        ~RestoreContext() {
            ImGui::SetCurrentContext(previous);
        }
    } restore_context;
    Window *w = reinterpret_cast<Window *>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        w = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        w->hwnd = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    }
    if (!w)
        return DefWindowProcW(handle, message, wp, lp);
    try {
        if ((w->kind == Kind::Zone || w->kind == Kind::Launcher || w->kind == Kind::Settings) &&
            !w->app.modal && !w->drag_in_progress) {
            POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            bool header = client.y >= 0 && client.y < 32 * w->scale && client.x < w->width - 58 * w->scale &&
                          w->rename_id.empty();
            if (w->kind == Kind::Launcher)
                header = client.y >= 0 && client.y < 36 * w->scale && client.x < 130 * w->scale;
            if (w->kind == Kind::Settings)
                header = client.y >= 0 && client.y < 36 * w->scale && client.x < w->width - 48 * w->scale;
            auto zone = w->app.engine.zone(w->zone_id);
            bool corner = zone && !zone->value("collapsed", false) && client.x >= w->width - 18 * w->scale &&
                          client.y >= w->height - 18 * w->scale;
            if (message == WM_LBUTTONDBLCLK && header && w->kind == Kind::Zone) {
                w->end_geometry(true);
                w->rename_id = w->zone_id;
                if (zone)
                    w->rename_buffer = w->app.engine.name(*zone);
                w->focus_search = true;
                w->invalidate();
                return 0;
            }
            if (message == WM_LBUTTONDOWN && (header || corner)) {
                ClientToScreen(handle, &client);
                w->begin_geometry(client, corner);
                return 0;
            }
            if (message == WM_MOUSEMOVE && w->geometry_capture) {
                ClientToScreen(handle, &client);
                w->update_geometry(client);
                return 0;
            }
            if (message == WM_LBUTTONUP && w->geometry_capture) {
                ClientToScreen(handle, &client);
                w->update_geometry(client);
                w->end_geometry();
                return 0;
            }
            if ((message == WM_KEYDOWN && wp == VK_ESCAPE) || message == WM_CANCELMODE ||
                (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lp) != handle)) {
                if (w->geometry_capture) {
                    w->end_geometry(true);
                    return 0;
                }
            }
        }
        if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message == WM_IME_COMPOSITION) {
            w->settle_frames = 2;
            SetTimer(handle, 3, 16, nullptr);
        }
        if (message == WM_SETCURSOR) {
            if (LOWORD(lp) == HTCLIENT && w->kind == Kind::Zone) {
                POINT mouse{};
                GetCursorPos(&mouse);
                ScreenToClient(handle, &mouse);
                auto zone = w->app.engine.zone(w->zone_id);
                if (zone && !zone->value("collapsed", false) && mouse.x >= w->width - 18 * w->scale &&
                    mouse.y >= w->height - 18 * w->scale) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                    return TRUE;
                }
            }
        }
        if (w->context) {
            ImGui::SetCurrentContext(w->context);
            if (ImGui_ImplWin32_WndProcHandler(handle, message, wp, lp)) {
                w->invalidate();
                return 1;
            }
        }
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(handle, &paint);
            EndPaint(handle, &paint);
            w->dirty = true;
            return 0;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED)
                w->resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_DPICHANGED: {
            w->scale = HIWORD(wp) / 96.f;
            w->theme();
            if (!w->desktop_child) {
                auto r = reinterpret_cast<RECT *>(lp);
                SetWindowPos(handle, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == 4) {
                w->finish_motion();
                return 0;
            }
            if (wp == 5 && w->size_animating) {
                float t =
                    std::clamp(static_cast<float>((GetTickCount64() - w->size_started) / 180.), 0.f, 1.f);
                float ease = 1 - (1 - t) * (1 - t) * (1 - t);
                LONG h = static_cast<LONG>((w->size_from.bottom - w->size_from.top) +
                                           (w->size_to.bottom - w->size_from.bottom) * ease);
                SetWindowPos(handle, nullptr, 0, 0, w->width, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (t >= 1) {
                    w->size_animating = false;
                    KillTimer(handle, 5);
                }
                return 0;
            }
            if (wp == 2) {
                w->invalidate();
                return 0;
            }
            if (wp == 3) {
                if (--w->settle_frames <= 0)
                    KillTimer(handle, 3);
                w->invalidate();
                return 0;
            }
            break;
        case WM_CLOSE:
            if (w->kind == Kind::Draw)
                w->app.defer([app = &w->app] { app->cancel_draw(); });
            else
                w->hide();
            return 0;
        case WM_KILLFOCUS:
            if (w->kind == Kind::Launcher && !w->app.modal && !w->drag_in_progress && !w->app.drawing)
                w->hide();
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_IME_COMPOSITION:
        case WM_SETFOCUS:
            w->settle_frames = 2;
            SetTimer(handle, 3, 16, nullptr);
            w->invalidate();
            break;
        case WM_DESTROY:
            if (w->drop_target)
                RevokeDragDrop(handle);
            if (!w->closing) {
                w->hwnd = nullptr;
                PostMessageW(w->app.controller, MSG_WAKE, 0, 0);
            }
            return 0;
        }
    } catch (const std::exception &ex) {
        w->app.error(ex.what());
    }
    return DefWindowProcW(handle, message, wp, lp);
}

IconCache::IconCache(ID3D11Device *dev, HWND host) : device(dev), notify(host) {
    worker = std::thread([this] { run(); });
}
IconCache::~IconCache() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    if (worker.joinable())
        worker.join();
}
static std::vector<uint8_t> icon_pixels(const std::string &path, std::string &name) {
    SHFILEINFOW info{};
    SHGetFileInfoW(wide(path).c_str(), 0, &info, sizeof(info),
                   SHGFI_ICON | SHGFI_LARGEICON | SHGFI_DISPLAYNAME);
    name = utf8(info.szDisplayName);
    if (!info.hIcon)
        return {};
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bmp{};
    bmp.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmp.bmiHeader.biWidth = 32;
    bmp.bmiHeader.biHeight = -32;
    bmp.bmiHeader.biPlanes = 1;
    bmp.bmiHeader.biBitCount = 32;
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmp, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        DestroyIcon(info.hIcon);
        return {};
    }
    auto old = SelectObject(dc, bitmap);
    memset(pixels, 0, 32 * 32 * 4);
    DrawIconEx(dc, 0, 0, info.hIcon, 32, 32, 0, nullptr, DI_NORMAL);
    auto raw = static_cast<uint8_t *>(pixels);
    std::vector<uint8_t> output(32 * 32 * 4);
    bool has_alpha = false;
    for (size_t n = 0; n < output.size(); n += 4) {
        output[n] = raw[n + 2];
        output[n + 1] = raw[n + 1];
        output[n + 2] = raw[n];
        output[n + 3] = raw[n + 3];
        has_alpha |= raw[n + 3] > 0;
    }
    if (!has_alpha) {
        for (size_t n = 0; n < output.size(); n += 4)
            output[n + 3] = (output[n] || output[n + 1] || output[n + 2]) ? 255 : 0;
    } else
        for (size_t n = 0; n < output.size(); n += 4) {
            auto alpha = output[n + 3];
            if (alpha && alpha < 255)
                for (int c = 0; c < 3; c++)
                    output[n + c] = static_cast<uint8_t>(std::min(255, output[n + c] * 255 / alpha));
        }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyIcon(info.hIcon);
    return output;
}
void IconCache::run() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        std::string path;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [this] { return stopping || !requests.empty(); });
            if (stopping)
                break;
            path = std::move(requests.front());
            requests.pop_front();
        }
        std::string name;
        auto pixels = icon_pixels(path, name);
        {
            std::lock_guard lock(mutex);
            if (auto it = entries.find(path); it != entries.end()) {
                it->second.pixels = std::move(pixels);
                it->second.display_name = std::move(name);
                it->second.ready = true;
            }
        }
        PostMessageW(notify, MSG_ICONS, 0, 0);
    }
    CoUninitialize();
}
ID3D11ShaderResourceView *IconCache::get(const std::string &path) {
    std::lock_guard lock(mutex);
    auto &e = entries[path];
    e.touched = ++sequence;
    if (!e.queued) {
        e.queued = true;
        requests.push_back(path);
        condition.notify_one();
    }
    if (e.ready && !e.view && !e.pixels.empty()) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 32;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{e.pixels.data(), 32 * 4, 0};
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &tex)))
            device->CreateShaderResourceView(tex.Get(), nullptr, &e.view);
        e.pixels.clear();
        e.pixels.shrink_to_fit();
    }
    auto result = e.view.Get();
    if (entries.size() > 512) {
        auto oldest = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->first != path && it->second.ready &&
                (oldest == entries.end() || it->second.touched < oldest->second.touched))
                oldest = it;
        if (oldest != entries.end())
            entries.erase(oldest);
    }
    return result;
}
std::string IconCache::display_name(const std::string &path, const std::string &fallback) {
    std::lock_guard lock(mutex);
    auto item = entries.find(path);
    return item != entries.end() && !item->second.display_name.empty() ? item->second.display_name : fallback;
}
size_t IconCache::size() {
    std::lock_guard lock(mutex);
    return entries.size();
}
void IconCache::clear() {
    std::lock_guard lock(mutex);
    entries.clear();
    requests.clear();
}

DirectoryWatch::DirectoryWatch(fs::path root, HWND target) {
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    directory =
        CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return;
    worker = std::thread([this, root = std::move(root), target] {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::array<BYTE, 65536> buffer{};
        std::wstring old_name;
        while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
            OVERLAPPED read{};
            read.hEvent = event;
            ResetEvent(event);
            if (!ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size()), FALSE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                       nullptr, &read, nullptr))
                break;
            HANDLE events[] = {stop_event, event};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                CancelIoEx(directory, &read);
                DWORD ignored = 0;
                GetOverlappedResult(directory, &read, &ignored, TRUE);
                break;
            }
            DWORD bytes = 0;
            if (GetOverlappedResult(directory, &read, &bytes, FALSE) && bytes) {
                for (DWORD offset = 0;;) {
                    auto info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer.data() + offset);
                    std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    if (info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                        old_name = name;
                    else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME && !old_name.empty()) {
                        auto value =
                            new Json{{"from", pathstr(root / old_name)}, {"to", pathstr(root / name)}};
                        if (!PostMessageW(target, MSG_FILES, 1, reinterpret_cast<LPARAM>(value)))
                            delete value;
                        old_name.clear();
                    }
                    if (!info->NextEntryOffset)
                        break;
                    offset += info->NextEntryOffset;
                }
                PostMessageW(target, MSG_FILES, 0, 0);
            }
        }
        CloseHandle(event);
    });
}
DirectoryWatch::~DirectoryWatch() {
    if (stop_event)
        SetEvent(stop_event);
    if (directory != INVALID_HANDLE_VALUE)
        CancelIoEx(directory, nullptr);
    if (worker.joinable())
        worker.join();
    if (directory != INVALID_HANDLE_VALUE)
        CloseHandle(directory);
    if (stop_event)
        CloseHandle(stop_event);
}
} // namespace deskedge
