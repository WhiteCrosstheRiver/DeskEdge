#include "imgui_impl_win32.h"
#include "platform.h"
#include <dbghelp.h>
#include <fstream>
#include <iomanip>
#include <psapi.h>
#include <sstream>
#include <unordered_set>

namespace deskedge {
static Application *current_app = nullptr;
static fs::path crash_folder;
static LONG WINAPI crash_report(EXCEPTION_POINTERS *exception) {
    if (crash_folder.empty())
        return EXCEPTION_EXECUTE_HANDLER;
    auto process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    std::ofstream output(crash_folder / L"crash.txt");
    output << "Exception " << std::hex << exception->ExceptionRecord->ExceptionCode << " at "
           << exception->ExceptionRecord->ExceptionAddress << "\n";
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    for (int i = 0;
         i < 30 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                               nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         i++) {
        char buffer[sizeof(SYMBOL_INFO) + 1024]{};
        auto symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 1023;
        DWORD64 offset = 0;
        output << std::hex << frame.AddrPC.Offset;
        if (SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol))
            output << " " << symbol->Name << " + " << offset;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD displacement = 0;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &displacement, &line))
            output << " " << line.FileName << ":" << std::dec << line.LineNumber;
        output << "\n";
    }
    output.close();
    SymCleanup(process);
    return EXCEPTION_EXECUTE_HANDLER;
}
Application::Application(fs::path data, bool test, bool unhosted)
    : engine(std::move(data)), demo(test), no_desktop(unhosted) {
    auto pref = engine.state["settings"].value("theme", "auto");
    dark = pref == "dark" || (pref == "auto" && system_dark());
    if (demo) {
        roots = {engine.data_dir / L"demo-desktop"};
        fs::create_directories(roots[0]);
    } else
        roots = {known_folder(FOLDERID_Desktop), known_folder(FOLDERID_PublicDesktop)};
    archive_root = roots[0] / L"DeskEdge Archive";
    seed();
    engine.sync(roots, archive_root);
    engine.save();
    active_tab = engine.state["tabs"][0]["id"];
}
void Application::seed() {
    if (engine.state.value("initialized", false))
        return;
    if (demo) {
        auto root = roots[0];
        for (auto name : {L"Exp_A_spectra", L"Thesis_ch3", L"Grant_2026"})
            fs::create_directories(root / name);
        for (auto [name, text] : std::vector<std::pair<std::wstring, std::string>>{
                 {L"run_0923.csv", "sample,intensity\nA,42\nB,31\n"},
                 {L"XRD_batch4.csv", "angle,intensity\n10,23\n20,71\n"},
                 {L"plot_fig2.py", "# Example research script for the DeskEdge demonstration\n"},
                 {L"reading_notes.txt", "DeskEdge demonstration: research notes.\n"},
                 {L"screenshot_notes.txt", "DeskEdge demo fixture — this is not a user file.\n"},
                 {L"tmp_calc.txt", "Temporary calculation\n"},
                 {L"新建文本文档.txt", "DeskEdge 中文输入与文件名测试\n"},
                 {L"draft_email.txt", "Draft example\n"},
                 {L"copy_of_run.csv", "x,y\n1,2\n"}}) {
            auto p = root / name;
            if (!fs::exists(p)) {
                std::ofstream out(p, std::ios::binary);
                out << text;
            }
        }
        auto pdf = [](const fs::path &path) {
            std::ofstream f(path, std::ios::binary);
            std::string s = "%PDF-1.4\n";
            std::vector<size_t> offsets{0};
            auto add = [&](int n, std::string v) {
                offsets.push_back(s.size());
                s += std::to_string(n) + " 0 obj\n" + v + "\nendobj\n";
            };
            add(1, "<< /Type /Catalog /Pages 2 0 R >>");
            add(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
            add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> "
                   ">> /Contents 5 0 R >>");
            add(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
            std::string content = "BT /F1 18 Tf 60 700 Td (DeskEdge demonstration document) Tj ET\n";
            add(5, "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
            auto xref = s.size();
            s += "xref\n0 6\n0000000000 65535 f \n";
            for (size_t n = 1; n < offsets.size(); n++) {
                std::ostringstream row;
                row << std::setw(10) << std::setfill('0') << offsets[n] << " 00000 n \n";
                s += row.str();
            }
            s += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
            f << s;
        };
        for (auto name : {L"Zhang2024_perovskite.pdf", L"Review_ML_catalysis.pdf", L"Smith_Nature_2025.pdf"})
            if (!fs::exists(root / name))
                pdf(root / name);
        for (auto name :
             {L"screenshot_notes.txt", L"tmp_calc.txt", L"新建文本文档.txt", L"copy_of_run.csv"}) {
            auto p = root / name;
            HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                ULARGE_INTEGER t{};
                t.QuadPart = static_cast<uint64_t>(now() - 18 * 86400 + 11644473600LL) * 10000000ULL;
                FILETIME ft{t.LowPart, t.HighPart};
                SetFileTime(h, nullptr, nullptr, &ft);
                CloseHandle(h);
            }
        }
    }
    auto apps = discover_apps();
    auto choose = [&](std::string fragment) -> std::optional<Json> {
        std::optional<Json> best;
        size_t distance = SIZE_MAX;
        auto token = lower(fragment);
        for (auto &i : apps)
            if (auto n = lower(i.value("name", "")); n.find(token) != std::string::npos) {
                bool auxiliary = false;
                for (auto excluded : {"示例", "帮助", "卸载", "文档", "uninstall", "sample", "example",
                                      "help", "documentation"})
                    if (n.find(excluded) != std::string::npos)
                        auxiliary = true;
                if (auxiliary)
                    continue;
                size_t cost = n.size() - token.size() + (n.starts_with(token) ? 0 : 20);
                if (cost < distance) {
                    distance = cost;
                    best = i;
                }
            }
        return best;
    };
    std::unordered_set<std::string> chosen;
    for (auto fragment : {"Origin", "MATLAB", "Visual Studio Code", "Zotero", "ImageJ", "Chrome", "Excel",
                          "Snipaste", "Everything", "Notepad++", "7-Zip"})
        if (auto app = choose(fragment)) {
            if (chosen.insert((*app)["path"].get<std::string>()).second) {
                engine.add_to_tab(fragment == std::string("Snipaste") ||
                                          fragment == std::string("Everything") ||
                                          fragment == std::string("7-Zip")
                                      ? "tools"
                                      : "software",
                                  wide((*app)["path"].get<std::string>()), (*app)["name"]);
                (*app)["name"] = fragment;
                if (engine.state["favorites"].size() < 6)
                    engine.state["favorites"].push_back(*app);
            }
        }
    auto windows = known_folder(FOLDERID_Windows);
    for (auto [label, path] :
         std::vector<std::pair<std::string, fs::path>>{{"File Explorer", windows / L"explorer.exe"},
                                                       {"Notepad", windows / L"System32" / L"notepad.exe"},
                                                       {"Calculator", windows / L"System32" / L"calc.exe"}})
        if (fs::exists(path)) {
            auto key = pathstr(path);
            if (chosen.insert(key).second) {
                engine.add_to_tab("tools", path, label);
                if (engine.state["favorites"].size() < 6)
                    engine.state["favorites"].push_back({{"id", id()},
                                                         {"name", label},
                                                         {"path", key},
                                                         {"kind", "app"},
                                                         {"uses", 0},
                                                         {"used", 0}});
            }
        }
    engine.sync(roots, archive_root);
    for (auto &i : engine.state["items"]) {
        auto z = i["zone"].get<std::string>();
        auto target = z == "papers" ? "reading" : z == "projects" || z == "data" ? "current" : "";
        if (target[0])
            engine.add_to_tab(target, wide(i["path"].get<std::string>()));
    }
    if (demo) {
        for (auto &i : engine.state["items"]) {
            if (i["name"] == "copy_of_run.csv")
                i["zone"] = "temp";
            if (i["name"] == "reading_notes.txt")
                i["zone"] = "papers";
        }
        auto find = [&](std::string n) {
            for (auto &i : engine.state["items"])
                if (i["name"] == n)
                    return i["path"].get<std::string>();
            return std::string();
        };
        engine.state["todos"] = Json::array(
            {{{"id", id()},
              {"text", "把 XRD 数据发给李老师"},
              {"due", "2026-10-09"},
              {"link", find("XRD_batch4.csv")},
              {"done", false}},
             {{"id", id()},
              {"text", "修改图 2 坐标轴标签"},
              {"due", ""},
              {"link", find("plot_fig2.py")},
              {"done", false}},
             {{"id", id()}, {"text", "预约显微镜机时"}, {"due", "2026-10-03"}, {"link", ""}, {"done", false}},
             {{"id", id()},
              {"text", "读 Zhang 2024"},
              {"due", ""},
              {"link", find("Zhang2024_perovskite.pdf")},
              {"done", true}}});
    }
    engine.state["initialized"] = true;
}
Application::~Application() {
    quit = true;
    DWORD clipboard_pid = 0;
    GetWindowThreadProcessId(GetClipboardOwner(), &clipboard_pid);
    if (clipboard_pid == GetCurrentProcessId())
        OleFlushClipboard();
    if (mouse_hook)
        UnhookWindowsHookEx(mouse_hook);
    watches.clear();
    icons.reset();
    draw_windows.clear();
    zones.clear();
    settings.reset();
    launcher.reset();
    panel.reset();
    restore_icons();
    if (controller) {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = controller;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        DestroyWindow(controller);
    }
    if (singleton)
        CloseHandle(singleton);
    current_app = nullptr;
}
std::string Application::tr(const char *zh, const char *en) const {
    return engine.state["settings"].value("language", "zh") == "en" ? en : zh;
}
void Application::defer(std::function<void()> action) {
    actions.push_back(std::move(action));
    PostMessageW(controller, MSG_WAKE, 0, 0);
}
void Application::perform_actions() {
    bool changed = false;
    while (!actions.empty()) {
        auto action = std::move(actions.front());
        actions.pop_front();
        try {
            action();
            changed = true;
        } catch (const std::exception &ex) {
            error(ex.what());
        }
    }
    if (changed)
        try {
            bool geometry_active = false;
            for (auto &zone : zones)
                geometry_active |= zone->moving || zone->resizing;
            if (geometry_active)
                synchronize();
            else
                commit();
        } catch (const std::exception &ex) {
            error(ex.what());
        }
}
void Application::commit() {
    engine.save();
    synchronize();
}
void Application::synchronize() {
    auto pref = engine.state["settings"].value("theme", "auto");
    bool effective = pref == "dark" || (pref == "auto" && system_dark());
    bool theme_changed = dark != effective;
    dark = effective;
    for (auto it = zones.begin(); it != zones.end();) {
        if (!engine.zone((*it)->zone_id) || !(*it)->hwnd || !IsWindow((*it)->hwnd))
            it = zones.erase(it);
        else
            ++it;
    }
    for (auto &z : engine.state["zones"]) {
        auto ident = z["id"].get<std::string>();
        Window *window = nullptr;
        for (auto &w : zones)
            if (w->zone_id == ident)
                window = w.get();
        if (!window) {
            auto w = std::make_unique<Window>(*this, Kind::Zone, ident);
            w->create(!no_desktop);
            window = w.get();
            zones.push_back(std::move(w));
        }
        auto screen = monitor(z.value("monitor", ""));
        float w = std::min(z.value("w", 316.f), screen.width()),
              h = z.value("collapsed", false) ? 34.f : z.value("h", 196.f);
        float x = std::clamp(z.value("x", 24.f), 0.f, std::max(0.f, screen.width() - w)),
              y = std::clamp(z.value("y", 24.f), 0.f, std::max(0.f, screen.height() - 34));
        z["x"] = x;
        z["y"] = y;
        z["w"] = w;
        window->place(x, y, w, h, screen);
        if (theme_changed)
            window->theme();
        if (peek || drawing)
            window->hide();
        else
            window->show();
        window->invalidate();
    }
    if (panel) {
        auto m = monitor(engine.state["settings"].value("monitor", ""));
        panel->place(std::max(0.f, m.width() - 350), 10, 340, std::max(280.f, m.height() - 20), m);
        if (theme_changed)
            panel->theme();
        panel->invalidate();
    }
    if (launcher) {
        if (!engine.tab(active_tab))
            active_tab = engine.state["tabs"][0]["id"];
        if (theme_changed)
            launcher->theme();
        launcher->invalidate();
    }
    if (settings) {
        if (theme_changed)
            settings->theme();
        settings->invalidate();
    }
    apply_icons();
}
void Application::notify(std::string message) {
    toast = std::move(message);
    toast_until = now() + 4;
    show_panel();
    SetTimer(controller, 7, 4000, nullptr);
    panel->invalidate();
}
void Application::error(std::string message) {
    write_log(engine.data_dir, message);
    if (panel)
        notify(message);
    else
        MessageBoxW(nullptr, wide(message).c_str(), L"DeskEdge", MB_OK | MB_ICONERROR);
}
void Application::show_panel(bool focus_search) {
    if (!panel)
        return;
    panel->focus_search = focus_search;
    panel->show(focus_search);
    panel->invalidate();
}
void Application::hide_panel() {
    if (panel)
        panel->hide();
}
void Application::show_launcher() {
    if (drawing)
        cancel_draw();
    if (launcher->visible()) {
        launcher->hide();
        return;
    }
    launcher->search.clear();
    launcher->focus_search = true;
    launcher->selected_index = 0;
    auto m = monitor("", true);
    POINT p{};
    GetCursorPos(&p);
    float w = std::min(568.f, m.width() - 16), h = std::min(388.f, m.height() - 16);
    float x = std::clamp((p.x - m.work.left) / m.scale - w / 2, 8.f, std::max(8.f, m.width() - w - 8));
    float y = std::clamp((p.y - m.work.top) / m.scale - h / 2, 8.f, std::max(8.f, m.height() - h - 8));
    launcher->place(x, y, w, h, m, true);
    launcher->show(true);
}
void Application::toggle_peek() {
    peek = !peek;
    for (auto &w : zones)
        if (peek)
            w->hide();
        else
            w->show();
    if (panel)
        panel->invalidate();
}
void Application::focus(std::string ident) {
    auto ptr = engine.item(ident);
    if (!ptr)
        return;
    focus_id = ident;
    focus_until = now() + 3;
    peek = false;
    auto target = ptr->value("zone", "");
    if (auto z = engine.zone(target))
        (*z)["collapsed"] = false;
    synchronize();
    SetTimer(controller, 8, 2800, nullptr);
}
void Application::open(std::string path) {
    shell_open(path, panel ? panel->hwnd : nullptr);
    engine.opened(path);
    engine.save();
    if (launcher)
        launcher->hide();
    for (auto &w : zones)
        w->invalidate();
    if (panel)
        panel->invalidate();
}
void Application::archive(std::vector<std::string> ids) {
    auto result = engine.archive(ids, archive_root);
    std::string message = tr("已归档 ", "Archived ") + std::to_string(result.succeeded) +
                          tr(" 项，可随时恢复。", " items. You can restore them any time.");
    if (!result.errors.empty()) {
        message += "\n" + result.errors[0];
        for (auto &e : result.errors)
            write_log(engine.data_dir, e);
    }
    notify(message);
}
void Application::restore() {
    auto result = engine.restore();
    std::string message =
        tr("已恢复 ", "Restored ") + std::to_string(result.succeeded) + tr(" 项。", " items.");
    if (!result.errors.empty()) {
        message += "\n" + result.errors[0];
        for (auto &e : result.errors)
            write_log(engine.data_dir, e);
    }
    notify(message);
}
void Application::begin_draw() {
    if (drawing)
        return;
    drawing = true;
    peek = false;
    launcher->hide();
    panel->hide();
    for (auto &z : zones)
        z->hide();
    for (auto &m : monitors()) {
        auto window = std::make_unique<Window>(*this, Kind::Draw);
        window->create();
        window->place(0, 0, m.width(), m.height(), m, true);
        window->show(true);
        draw_windows.push_back(std::move(window));
    }
}
void Application::cancel_draw() {
    drawing = false;
    draw_windows.clear();
    synchronize();
    show_panel();
}
void Application::add_files(std::string z, std::string t, bool folder) {
    modal = true;
    try {
        auto files = pick_files(launcher && launcher->visible() ? launcher->hwnd : panel->hwnd, folder);
        for (auto &path : files)
            if (!t.empty())
                engine.add_to_tab(t, path);
            else
                engine.add_file(path, z);
    } catch (...) {
        modal = false;
        throw;
    }
    modal = false;
}
void Application::drop(Window &window, const Json &payload, const std::vector<std::string> &files, POINT p) {
    if (window.kind == Kind::Zone) {
        if ((payload.is_object() ? payload.value("type", "") : "") == "desktop") {
            if (payload.contains("ids") && payload["ids"].is_array())
                for (auto &ident : payload["ids"])
                    engine.move_zone(ident.get<std::string>(), window.zone_id);
            else
                engine.move_zone(payload.value("id", ""), window.zone_id);
        } else
            for (auto &path : files)
                engine.add_file(wide(path), window.zone_id);
    } else if (window.kind == Kind::Launcher) {
        std::string target = active_tab, before;
        for (auto &spot : window.hotspots)
            if (PtInRect(&spot.rect, p)) {
                target = spot.tab;
                before = spot.before;
                break;
            }
        if ((payload.is_object() ? payload.value("type", "") : "") == "launcher" &&
            engine.launch(payload.value("id", "")))
            engine.move_launch(payload.value("id", ""), target, window.search.empty() ? before : "");
        else
            for (auto &path : files)
                engine.add_to_tab(target, wide(path));
    } else if (window.kind == Kind::Panel) {
        for (auto &path : files)
            engine.add_file(wide(path));
    }
    commit();
}
void Application::show_settings() {
    if (!settings) {
        settings = std::make_unique<Window>(*this, Kind::Settings);
        settings->create();
    }
    auto m = monitor("", true);
    settings->place((m.width() - 440) / 2, (m.height() - 500) / 2, 440, 500, m, true);
    settings->show(true);
}
void Application::tray() {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = controller;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = MSG_TRAY;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    wcscpy_s(data.szTip, L"DeskEdge · 桌沿");
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}
void Application::restore_icons() {
    if (icons_hidden && native_icons_initial) {
        if (auto icons_hwnd = desktop_icons())
            ShowWindow(icons_hwnd, SW_SHOW);
    }
    icons_hidden = false;
}
void Application::apply_icons() {
    if (no_desktop)
        return;
    auto view = desktop_icons();
    bool use = engine.state["settings"].value("replace_icons", true) && !zones.empty();
    if (use && view) {
        ShowWindow(view, SW_HIDE);
        icons_hidden = true;
    } else
        restore_icons();
}
LRESULT CALLBACK Application::mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && current_app && !current_app->quit) {
        auto a = current_app;
        auto info = reinterpret_cast<MSLLHOOKSTRUCT *>(lp);
        if (wp == WM_MOUSEMOVE) {
            if (a->panel && a->panel->visible()) {
                if (a->engine.state["settings"].value("pinned", true))
                    return CallNextHookEx(nullptr, code, wp, lp);
                RECT r{};
                GetWindowRect(a->panel->hwnd, &r);
                if (!PtInRect(&r, info->pt))
                    PostMessageW(a->controller, MSG_EDGE, 2, 0);
            } else {
                auto m = a->panel->screen;
                if (info->pt.x >= m.bounds.right - 3 && info->pt.x < m.bounds.right &&
                    info->pt.y >= m.work.top && info->pt.y < m.work.bottom)
                    PostMessageW(a->controller, MSG_EDGE, 1, 0);
            }
        } else if (wp == WM_MBUTTONUP && a->engine.state["settings"].value("middle_click", false))
            PostMessageW(a->controller, WM_HOTKEY, 2, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK Application::controller_proc(HWND handle, UINT message, WPARAM wp, LPARAM lp) {
    auto a = reinterpret_cast<Application *>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        a = static_cast<Application *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        a->controller = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
    }
    if (!a)
        return DefWindowProcW(handle, message, wp, lp);
    try {
        if (message == a->taskbar_message && a->taskbar_message) {
            SetTimer(handle, 5, 1800, nullptr);
            return 0;
        }
        if (message == a->wake_message && a->wake_message) {
            a->show_panel(true);
            return 0;
        }
        switch (message) {
        case MSG_WAKE:
            return 0;
        case MSG_ICONS:
            if (a->panel)
                a->panel->invalidate();
            if (a->launcher)
                a->launcher->invalidate();
            for (auto &z : a->zones)
                z->invalidate();
            return 0;
        case MSG_FILES:
            if (wp == 1) {
                std::unique_ptr<Json> value(reinterpret_cast<Json *>(lp));
                a->engine.rename_path(wide((*value)["from"].get<std::string>()),
                                      wide((*value)["to"].get<std::string>()));
            }
            SetTimer(handle, 4, 400, nullptr);
            return 0;
        case MSG_EDGE:
            if (wp == 1) {
                SetTimer(handle, 6, 120, nullptr);
            } else if (a->panel && a->panel->visible() &&
                       !a->engine.state["settings"].value("pinned", true) && !a->modal &&
                       !a->panel->popup_open && !a->drawing)
                a->hide_panel();
            return 0;
        case WM_HOTKEY:
            if (wp == 1)
                a->show_panel(true);
            else if (wp == 2 || wp == 3)
                a->show_launcher();
            return 0;
        case WM_TIMER:
            if (wp == 4) {
                KillTimer(handle, 4);
                a->engine.sync(a->roots, a->archive_root);
                a->commit();
            } else if (wp == 5) {
                KillTimer(handle, 5);
                a->tray();
                a->synchronize();
            } else if (wp == 6) {
                KillTimer(handle, 6);
                auto m = monitor(a->engine.state["settings"].value("monitor", ""));
                POINT p{};
                GetCursorPos(&p);
                RECT foreground{};
                auto top = GetForegroundWindow();
                GetWindowRect(top, &foreground);
                bool fullscreen = top != GetAncestor(desktop_view(), GA_ROOT) &&
                                  foreground.left <= m.bounds.left && foreground.top <= m.bounds.top &&
                                  foreground.right >= m.bounds.right && foreground.bottom >= m.bounds.bottom;
                if (p.x >= m.bounds.right - 3 && p.x < m.bounds.right && !fullscreen)
                    a->show_panel();
            } else if (wp == 7) {
                KillTimer(handle, 7);
                a->toast.clear();
                a->panel->invalidate();
            } else if (wp == 8) {
                KillTimer(handle, 8);
                a->focus_id.clear();
                for (auto &z : a->zones)
                    z->invalidate();
            } else if (wp == 98) {
                // A bounded refresh enables frame-based capture tools in the isolated QA profile.
                if (a->panel)
                    a->panel->invalidate();
                if (a->launcher)
                    a->launcher->invalidate();
                if (a->settings)
                    a->settings->invalidate();
                for (auto &window : a->draw_windows)
                    window->invalidate();
                for (auto &window : a->zones)
                    window->invalidate();
            } else if (wp == 99) {
                KillTimer(handle, 99);
                a->quit = true;
                PostQuitMessage(0);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (a->glass)
                a->glass->clear();
            if (a->icons)
                a->icons->clear();
        case WM_DISPLAYCHANGE:
            if (a->glass)
                a->glass->clear();
            a->synchronize();
            return 0;
        case MSG_TRAY: {
            auto event = LOWORD(lp);
            if (event == NIN_SELECT || event == WM_LBUTTONUP)
                a->show_panel(true);
            else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                auto menu = CreatePopupMenu();
                auto entry = [&](UINT ident, const char *zh, const char *en) {
                    AppendMenuW(menu, MF_STRING, ident, wide(a->tr(zh, en)).c_str());
                };
                entry(1, "搜索桌面", "Search desktop");
                entry(2, "启动器", "Launcher");
                entry(3, "绘制区域", "Draw zone");
                entry(4, "查看壁纸", "Peek wallpaper");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                entry(5, "设置", "Settings");
                entry(6, "打开数据目录", "Open data folder");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                entry(7, "退出 DeskEdge", "Exit DeskEdge");
                POINT p{};
                GetCursorPos(&p);
                SetForegroundWindow(handle);
                auto selected =
                    TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, handle, nullptr);
                DestroyMenu(menu);
                if (selected == 1)
                    a->show_panel(true);
                else if (selected == 2)
                    a->show_launcher();
                else if (selected == 3)
                    a->begin_draw();
                else if (selected == 4)
                    a->toggle_peek();
                else if (selected == 5)
                    a->show_settings();
                else if (selected == 6)
                    shell_open(pathstr(a->engine.data_dir));
                else if (selected == 7) {
                    a->quit = true;
                    PostQuitMessage(0);
                }
            }
            return 0;
        }
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                a->engine.save();
                a->restore_icons();
                a->quit = true;
            }
            return 0;
        case WM_CLOSE:
            a->quit = true;
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &ex) {
        a->error(ex.what());
    }
    return DefWindowProcW(handle, message, wp, lp);
}
int Application::run() {
    current_app = this;
    auto instance = GetModuleHandleW(nullptr);
    const auto hash = std::hash<std::wstring>{}(engine.data_dir.wstring());
    auto mutex_name = L"Local\\DeskEdge." + std::to_wstring(hash);
    wake_message = RegisterWindowMessageW((L"DeskEdge.Wake." + std::to_wstring(hash)).c_str());
    WNDCLASSEXW control{sizeof(control)};
    control.lpfnWndProc = controller_proc;
    control.hInstance = instance;
    control.lpszClassName = L"DeskEdge.Controller";
    RegisterClassExW(&control);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS | CS_DROPSHADOW;
    wc.lpfnWndProc = Window::procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.lpszClassName = L"DeskEdge.Window";
    RegisterClassExW(&wc);
    controller = CreateWindowExW(WS_EX_TOOLWINDOW, L"DeskEdge.Controller", L"DeskEdge background controller",
                                 WS_POPUP, -10000, -10000, 1, 1, nullptr, nullptr, instance, this);
    if (!controller)
        throw std::runtime_error(win_error());
    D3D_FEATURE_LEVEL level{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &device_context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                               &device, &level, &device_context);
    if (FAILED(hr))
        throw std::runtime_error("Cannot initialize DirectX 11");
    ComPtr<IDXGIDevice> dxgi_device;
    device.As(&dxgi_device);
    if (FAILED(DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(&composition))))
        throw std::runtime_error("Cannot initialize Windows DirectComposition");
    glass = std::make_unique<GlassMaterial>(device.Get());
    for (const auto &display : monitors())
        glass->wallpaper(display);
    icons = std::make_unique<IconCache>(device.Get(), controller);
    taskbar_message = RegisterWindowMessageW(L"TaskbarCreated");
    panel = std::make_unique<Window>(*this, Kind::Panel);
    panel->create();
    launcher = std::make_unique<Window>(*this, Kind::Launcher);
    launcher->create();
    if (!no_desktop) {
        auto icons_hwnd = desktop_icons();
        native_icons_initial = icons_hwnd && IsWindowVisible(icons_hwnd);
        if (native_icons_initial) {
            wchar_t exe[32768]{};
            GetModuleFileNameW(nullptr, exe, 32768);
            std::wstring command = L"\"" + std::wstring(exe) + L"\" --restore-desktop-pid " +
                                   std::to_wstring(GetCurrentProcessId()) + L" --restore-report \"" +
                                   (engine.data_dir / L"restoration.json").wstring() + L"\"";
            STARTUPINFOW start{sizeof(start)};
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               nullptr, &start, &process)) {
                CloseHandle(process.hProcess);
                CloseHandle(process.hThread);
            } else {
                no_desktop = true;
                write_log(engine.data_dir,
                          "Desktop restoration watcher could not start; preserving native icons.");
            }
        }
    }
    synchronize();
    show_panel();
    tray();
    mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, instance, 0);
    std::vector<std::string> conflicts;
    if (!RegisterHotKey(controller, 1, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE)) {
        conflicts.push_back("Ctrl+Space");
        if (RegisterHotKey(controller, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_SPACE))
            notify(tr("Ctrl+Space 已被占用，已启用 Ctrl+Shift+Space。",
                      "Ctrl+Space is occupied. Ctrl+Shift+Space is available."));
    }
    if (!RegisterHotKey(controller, 2, MOD_CONTROL | MOD_NOREPEAT, VK_NUMPAD0))
        conflicts.push_back("Ctrl+Num0");
    RegisterHotKey(controller, 3, MOD_CONTROL | MOD_NOREPEAT, '0');
    if (!conflicts.empty()) {
        std::string text = tr("快捷键已被其他程序占用：", "Hotkeys used by other apps: ");
        for (auto &key : conflicts)
            text += key + " ";
        write_log(engine.data_dir, text);
    }
    for (auto &root : roots)
        watches.push_back(std::make_unique<DirectoryWatch>(root, controller));
    if (quit_after > 0)
        SetTimer(controller, 99, quit_after * 1000, nullptr);
    if (demo && no_desktop)
        SetTimer(controller, 98, 500, nullptr);
    if (!engine.recovery_message.empty())
        notify(engine.recovery_message);
    write_log(engine.data_dir, "DeskEdge started, C++20/DirectX11. desktop=" + std::to_string(!no_desktop));
    for (auto &screen : monitors())
        write_log(engine.data_dir,
                  "monitor " + screen.name + " bounds=" + std::to_string(screen.bounds.left) + "," +
                      std::to_string(screen.bounds.top) + "," + std::to_string(screen.bounds.right) + "," +
                      std::to_string(screen.bounds.bottom) + " scale=" + std::to_string(screen.scale));
    if (self_test_ui)
        return run_ui_tests();
    if (render_previews) {
        modal = true;
        show_launcher();
        show_settings();
    }
    MSG message{};
    while (!quit) {
        auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0)
            break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
        int messages = 0;
        while (messages++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (quit)
            break;
        perform_actions();
        auto render = [&](Window *w) {
            if (w && w->dirty && w->visible())
                try {
                    w->render();
                } catch (const std::exception &ex) {
                    error(ex.what());
                    w->hide();
                }
        };
        render(panel.get());
        render(launcher.get());
        render(settings.get());
        for (auto &z : zones)
            render(z.get());
        for (auto &w : draw_windows)
            render(w.get());
        perform_actions();
    }
    if (render_previews) {
        auto folder = engine.data_dir / L"previews";
        panel->export_preview(folder / L"panel.png");
        launcher->export_preview(folder / L"launcher.png");
        if (settings)
            settings->export_preview(folder / L"settings.png");
        for (auto &zone : zones)
            zone->export_preview(folder / (wide(zone->zone_id) + L".png"));
    }
    for (int key = 1; key <= 3; key++)
        UnregisterHotKey(controller, key);
    engine.save();
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    Json diagnostics = {{"frames", rendered_frames.load()},
                        {"icon_cache", icons->size()},
                        {"working_set_bytes", counters.WorkingSetSize},
                        {"zones", zones.size()},
                        {"desktop_hosted", !no_desktop},
                        {"render_backend", "DirectComposition / BGRA premultiplied flip swapchain"},
                        {"animations_enabled", panel->animations_enabled()},
                        {"pid", GetCurrentProcessId()}};
    diagnostics["native_windows"] = Json::array();
    for (auto &zone : zones) {
        wchar_t parent_class[256]{};
        auto parent = GetParent(zone->hwnd);
        if (parent)
            GetClassNameW(parent, parent_class, 256);
        diagnostics["native_windows"].push_back({{"zone", zone->zone_id},
                                                 {"valid", IsWindow(zone->hwnd) != FALSE},
                                                 {"hosted", zone->desktop_child},
                                                 {"parent_class", utf8(parent_class)},
                                                 {"scale", zone->scale},
                                                 {"width", zone->width},
                                                 {"height", zone->height}});
        diagnostics["native_windows"].back()["gpu_composition"] = zone->composition_target != nullptr;
        diagnostics["native_windows"].back()["surface_allocations"] = zone->surface_allocations;
        diagnostics["native_windows"].back()["geometry_updates"] = zone->geometry_updates;
    }
    diagnostics["native_icons_initially_visible"] = native_icons_initial;
    diagnostics["native_icons_hidden_during_run"] = icons_hidden;
    std::ofstream report(engine.data_dir / L"diagnostics.json");
    report << diagnostics.dump(2);
    return 0;
}
} // namespace deskedge

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace deskedge;
    OleInitialize(nullptr);
    ImGui_ImplWin32_EnableDpiAwareness();
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    fs::path data;
    bool demo = false, no_desktop = false, previews = false, test_ui = false;
    int timeout = 0;
    try {
        for (int i = 1; i < count; i++) {
            std::wstring_view arg = args[i];
            if (arg == L"--restore-desktop-pid" && i + 1 < count) {
                DWORD pid = std::stoul(args[++i]);
                fs::path report_path;
                if (i + 2 < count && std::wstring_view(args[i + 1]) == L"--restore-report")
                    report_path = args[i + 2];
                auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
                if (process) {
                    WaitForSingleObject(process, INFINITE);
                    CloseHandle(process);
                }
                if (auto icons = desktop_icons())
                    ShowWindow(icons, SW_SHOW);
                if (!report_path.empty()) {
                    auto icons = desktop_icons();
                    std::ofstream report(report_path);
                    report << Json({{"watched_pid", pid},
                                    {"icons_restored", icons && IsWindowVisible(icons) != FALSE}})
                                  .dump(2);
                }
                LocalFree(args);
                OleUninitialize();
                return 0;
            } else if (arg == L"--data-dir" && i + 1 < count)
                data = args[++i];
            else if (arg == L"--demo")
                demo = true;
            else if (arg == L"--no-desktop-host")
                no_desktop = true;
            else if (arg == L"--render-previews") {
                previews = true;
                demo = true;
                no_desktop = true;
                timeout = 3;
            } else if (arg == L"--self-test-ui") {
                test_ui = true;
                demo = true;
                no_desktop = true;
            } else if (arg == L"--write-marker" && i + 1 < count) {
                std::ofstream marker(fs::path(args[++i]), std::ios::binary);
                marker << "DeskEdge shortcut arguments preserved";
                LocalFree(args);
                OleUninitialize();
                return marker ? 0 : 1;
            } else if (arg == L"--quit-after" && i + 1 < count)
                timeout = std::stoi(args[++i]);
        }
        LocalFree(args);
        args = nullptr;
        if (data.empty())
            data = known_folder(FOLDERID_LocalAppData) / L"DeskEdge";
        crash_folder = data;
        SetUnhandledExceptionFilter(crash_report);
        auto hash = std::hash<std::wstring>{}(fs::absolute(data).lexically_normal().wstring());
        auto mutex_name = L"Local\\DeskEdge." + std::to_wstring(hash);
        HANDLE lock = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
        DWORD lock_error = GetLastError();
        if (!lock)
            throw std::runtime_error(win_error(lock_error));
        if (lock_error == ERROR_ALREADY_EXISTS) {
            auto wake = RegisterWindowMessageW((L"DeskEdge.Wake." + std::to_wstring(hash)).c_str());
            PostMessageW(HWND_BROADCAST, wake, 0, 0);
            CloseHandle(lock);
            OleUninitialize();
            return 0;
        }
        int result = 0;
        try {
            {
                Application app(fs::absolute(data).lexically_normal(), demo, no_desktop);
                app.singleton = lock;
                lock = nullptr;
                app.quit_after = timeout;
                app.render_previews = previews;
                app.self_test_ui = test_ui;
                result = app.run();
            }
        } catch (...) {
            if (lock)
                CloseHandle(lock);
            throw;
        }
        OleUninitialize();
        return result;
    } catch (const std::exception &ex) {
        if (args)
            LocalFree(args);
        MessageBoxW(nullptr, wide(std::string("DeskEdge could not start.\n\n") + ex.what()).c_str(),
                    L"DeskEdge", MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }
}
