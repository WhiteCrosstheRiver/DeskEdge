#include "platform.h"
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace deskedge {
// White-box integration tests use the app's own ImGui input queue, never OS input injection.
// They run only on an isolated demo profile and export the app's own render targets.
int Application::run_ui_tests() {
    Json checks = Json::array();
    modal = true;
    auto require = [&](bool value, std::string name) {
        write_log(engine.data_dir, (value ? "PASS " : "FAIL ") + name);
        checks.push_back({{"name", name}, {"pass", value}});
        if (!value)
            throw std::runtime_error("UI test: " + name);
    };
    auto frame = [&](Window &w, std::function<void(ImGuiIO &)> input = {}) {
        if (!w.visible()) {
            ImGui::SetCurrentContext(w.context);
            if (input)
                input(ImGui::GetIO());
            return;
        }
        w.test_input = [input](ImGuiIO &io) {
            io.ConfigInputTrickleEventQueue = false;
            io.AddFocusEvent(true);
            io.AddMousePosEvent(-10000, -10000);
            if (input)
                input(io);
        };
        w.render();
        w.test_input = nullptr;
    };
    auto render_all = [&] {
        frame(*panel);
        for (auto &w : zones)
            frame(*w);
    };
    auto click = [&](Window &w, std::string key) {
        frame(w);
        auto it = w.test_controls.find(key);
        if (it == w.test_controls.end())
            throw std::runtime_error("Missing UI control: " + key);
        auto rect = it->second;
        float x = (rect.x + rect.z) / 2, y = (rect.y + rect.w) / 2;
        frame(w, [=](ImGuiIO &io) { io.AddMousePosEvent(x, y); });
        frame(w, [=](ImGuiIO &io) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
        });
        frame(w, [=](ImGuiIO &io) {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, false);
        });
        perform_actions();
    };
    auto key = [&](Window &w, ImGuiKey key) {
        frame(w, [=](ImGuiIO &io) { io.AddKeyEvent(key, true); });
        frame(w, [=](ImGuiIO &io) { io.AddKeyEvent(key, false); });
        perform_actions();
    };
    auto text = [&](Window &w, std::string value) {
        frame(w, [value](ImGuiIO &io) { io.AddInputCharactersUTF8(value.c_str()); });
    };
    auto pump = [&](int milliseconds) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
            MSG m{};
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            perform_actions();
        }
    };
    int result = 0;
    try {
        engine.state["settings"]["language"] = "zh";
        engine.state["settings"]["theme"] = "light";
        engine.state["snooze_until"] = 0;
        commit();
        render_all();
        require(zones.size() == 4 && panel->width == static_cast<int>(340 * panel->scale),
                "Default zones and scaled panel geometry");
        {
            auto &zone = *zones.front();
            engine.state["settings"]["window_grid"] = false;
            auto original = *engine.zone(zone.zone_id);
            RECT start{}, moved{}, restored{};
            GetWindowRect(zone.hwnd, &start);
            auto other_updates = zones[1]->geometry_updates;
            auto panel_updates = panel->geometry_updates;
            auto allocations = zone.surface_allocations;
            require(zone.composition_target && zone.visual &&
                        !(GetWindowLongPtrW(zone.hwnd, GWL_EXSTYLE) & WS_EX_LAYERED),
                    "Desktop regions use GPU DirectComposition without layered-window CPU copies");
            require(glass->wallpaper(zone.screen) != nullptr,
                    "Wallpaper glass material is uploaded and cached");
            POINT pointer{start.left + 40, start.top + 15};
            zone.begin_geometry(pointer, false);
            zone.update_geometry({pointer.x + 1, pointer.y + 1});
            GetWindowRect(zone.hwnd, &moved);
            require(EqualRect(&start, &moved), "System drag threshold prevents accidental title movement");
            auto begin = std::chrono::steady_clock::now();
            for (int i = 1; i <= 120; ++i)
                zone.update_geometry({pointer.x + i, pointer.y + i / 2});
            auto elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            GetWindowRect(zone.hwnd, &moved);
            require(moved.left == start.left + 120 && moved.top == start.top + 60,
                    "Captured drag follows the full initial cursor delta without a threshold jump");
            require(*engine.zone(zone.zone_id) == original && actions.empty(),
                    "Dragging leaves the persisted model untouched until mouse release");
            require(zones[1]->geometry_updates == other_updates && panel->geometry_updates == panel_updates &&
                        zone.surface_allocations == allocations,
                    "Dragging neither repositions other windows nor reallocates render surfaces");
            checks.push_back({{"metric", "120 native geometry updates, milliseconds"}, {"value", elapsed}});
            zone.end_geometry(true);
            perform_actions();
            GetWindowRect(zone.hwnd, &restored);
            require(EqualRect(&start, &restored) && *engine.zone(zone.zone_id) == original,
                    "Cancelling a captured drag restores geometry and preserves layout");
            zone.begin_geometry(pointer, false);
            zone.update_geometry({pointer.x + 30, pointer.y + 20});
            zone.end_geometry();
            require(actions.size() == 1, "Mouse release queues exactly one layout commit");
            perform_actions();
            require(engine.zone(zone.zone_id)->value("x", 0.f) == original.value("x", 0.f) + 30 / zone.scale,
                    "Released drag persists screen coordinates in DPI-independent units");
            GetWindowRect(zone.hwnd, &moved);
            zone.begin_geometry({moved.right - 4, moved.bottom - 4}, true);
            allocations = zone.surface_allocations;
            for (int i = 1; i <= 30; ++i)
                zone.update_geometry({moved.right - 4 + i, moved.bottom - 4 + i});
            require(zone.surface_allocations == allocations && zone.surface_dirty,
                    "Resize input coalesces GPU allocation until the next rendered frame");
            frame(zone);
            require(zone.surface_allocations == allocations + 1,
                    "A resized frame allocates one surface for the latest size");
            zone.end_geometry(true);
            perform_actions();
            *engine.zone(zone.zone_id) = original;
            engine.state["settings"]["window_grid"] = true;
            commit();
            frame(zone);
            zone.test_animations = true;
            if (zone.animations_enabled()) {
                (*engine.zone(zone.zone_id))["collapsed"] = true;
                commit();
                require(zone.size_animating, "Collapse starts a bounded height transition");
                pump(260);
                frame(zone);
                require(!zone.size_animating && zone.height == static_cast<int>(34 * zone.scale),
                        "Collapse settles to the exact target and stops its animation timer");
                (*engine.zone(zone.zone_id))["collapsed"] = false;
                commit();
                pump(260);
                frame(zone);
                zone.hide();
                require(!zone.visible() && IsWindowVisible(zone.hwnd),
                        "Hide updates logical visibility while the compositor finishes fading");
                zone.show();
                pump(220);
                require(zone.visible() && IsWindowVisible(zone.hwnd) && !zone.fading_out,
                        "Reopening during fade cancels the pending native hide");
                zone.hide();
                pump(220);
                require(!IsWindowVisible(zone.hwnd) && !zone.fading_out,
                        "Fade completion hides the HWND and stops its timer");
                zone.show();
            }
            zone.test_animations = false;
            frame(zone);
        }
        {
            auto &zone = *zones.front();
            auto original = *engine.zone(zone.zone_id);
            RECT start{}, moved{}, returned{};
            GetWindowRect(zone.hwnd, &start);
            POINT pointer{start.left + 40, start.top + 15};
            zone.begin_geometry(pointer, false);
            zone.update_geometry(
                {pointer.x + static_cast<LONG>((1121 - original.value("x", 0.f)) * zone.scale),
                 pointer.y + static_cast<LONG>((533 - original.value("y", 0.f)) * zone.scale)});
            GetWindowRect(zone.hwnd, &moved);
            require(moved.left == zone.screen.work.left + static_cast<LONG>(1128 * zone.scale) &&
                        moved.top == zone.screen.work.top + static_cast<LONG>(528 * zone.scale),
                    "125-percent DPI title drag snaps to the desktop lattice");
            require(geometry_guide && geometry_guide->visible() && GetCapture() == zone.hwnd &&
                        GetFocus() == zone.hwnd && geometry_guide->drop_target == nullptr,
                    "Grid guide preserves the dragged window's capture and focus without a drop target");
            require(SendMessageW(geometry_guide->hwnd, WM_NCHITTEST, 0, 0) == HTTRANSPARENT &&
                        SendMessageW(geometry_guide->hwnd, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE,
                    "Guide window declines mouse targeting and activation");
            frame(*geometry_guide);
            {
                D3D11_TEXTURE2D_DESC desc{};
                geometry_guide->texture->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;
                ComPtr<ID3D11Texture2D> readback;
                require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)),
                        "Guide alpha can be inspected in its own GPU render target");
                device_context->CopyResource(readback.Get(), geometry_guide->texture.Get());
                D3D11_MAPPED_SUBRESOURCE pixels{};
                if (FAILED(device_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &pixels)))
                    throw std::runtime_error("Cannot inspect guide alpha");
                int x =
                    int((geometry_guide->guide_bounds.x + geometry_guide->guide_bounds.w / 2) * zone.scale);
                int y =
                    int((geometry_guide->guide_bounds.y + geometry_guide->guide_bounds.h / 2) * zone.scale);
                auto bytes = static_cast<const uint8_t *>(pixels.pData);
                auto background_alpha = bytes[3 * pixels.RowPitch + 3 * 4 + 3],
                     center_alpha = bytes[y * pixels.RowPitch + x * 4 + 3];
                device_context->Unmap(readback.Get(), 0);
                require(background_alpha == 0 && center_alpha > 0 && center_alpha < 64,
                        "Guide background is fully transparent and its placement fill remains translucent");
                require(desc.Width < DWORD(zone.screen.work.right - zone.screen.work.left) &&
                            desc.Height < DWORD(zone.screen.work.bottom - zone.screen.work.top),
                        "Region guides allocate a bounded surface instead of a whole-monitor framebuffer");
            }
            if (!no_desktop)
                require(geometry_guide->desktop_child &&
                            GetParent(geometry_guide->hwnd) == GetParent(zone.hwnd),
                        "Native guide shares the region's desktop host");
            geometry_guide->export_preview(engine.data_dir / L"previews" / L"window-grid.png");
            require(*engine.zone(zone.zone_id) == original && actions.empty(),
                    "Snapped drag preview leaves persisted region geometry untouched");
            zone.end_geometry();
            require(actions.size() == 1 && !geometry_guide->visible(),
                    "Snapped release hides its guide and queues one commit");
            perform_actions();
            require(engine.zone(zone.zone_id)->at("x") == 1128 && engine.zone(zone.zone_id)->at("y") == 528,
                    "Region snap coordinates persist in logical units");
            Engine reloaded(engine.data_dir);
            require(reloaded.zone(zone.zone_id)->at("x") == 1128,
                    "Region placement survives an independent profile reload");
            POINT corner{moved.right - 4, moved.bottom - 4};
            zone.begin_geometry(corner, true);
            zone.update_geometry({corner.x + 34, corner.y + 28});
            GetWindowRect(zone.hwnd, &returned);
            require((returned.right - returned.left) == static_cast<LONG>(360 * zone.scale) &&
                        (returned.bottom - returned.top) == static_cast<LONG>(240 * zone.scale),
                    "Region resizing snaps both dimensions in physical DPI coordinates");
            zone.end_geometry(true);
            perform_actions();
            GetWindowRect(zone.hwnd, &returned);
            require(EqualRect(&moved, &returned) && !geometry_guide->visible(),
                    "Cancelling snapped resize restores the original native bounds and removes guides");
            pointer = {moved.left + 40, moved.top + 15};
            zone.begin_geometry(pointer, false);
            zone.update_geometry({pointer.x + 10000, pointer.y + 10000});
            GetWindowRect(zone.hwnd, &returned);
            require(returned.right <= zone.screen.work.right && returned.bottom <= zone.screen.work.bottom,
                    "Region snapping at a monitor edge keeps the whole card inside the work area");
            zone.end_geometry(true);
            perform_actions();
            auto occupied = engine.zone(zones[1]->zone_id);
            zone.begin_geometry(pointer, false);
            zone.update_geometry(
                {pointer.x + static_cast<LONG>((occupied->value("x", 0.f) - 1128) * zone.scale),
                 pointer.y + static_cast<LONG>((occupied->value("y", 0.f) - 528) * zone.scale)});
            require(zone.placement_blocked && geometry_guide->placement_blocked,
                    "Occupied region footprints are marked as unavailable");
            frame(*geometry_guide);
            geometry_guide->export_preview(engine.data_dir / L"previews" / L"window-grid-blocked.png");
            zone.end_geometry();
            perform_actions();
            GetWindowRect(zone.hwnd, &returned);
            require(EqualRect(&moved, &returned) && engine.zone(zone.zone_id)->at("x") == 1128,
                    "Release over an occupied footprint restores the previous region placement");
            *engine.zone(zone.zone_id) = original;
            commit();
            frame(zone);
        }
        panel->export_preview(engine.data_dir / L"previews" / L"panel.png");
        for (auto &w : zones)
            w->export_preview(engine.data_dir / L"previews" / (wide(w->zone_id) + L".png"));
        auto stale_count = engine.stale().size();
        require(stale_count > 1, "Demo stale files are isolated fixtures");
        click(*panel, "button:检查");
        require(panel->review_open, "Review button opens file list");
        click(*panel, "button:保留");
        require(engine.stale().size() == stale_count - 1, "Keep button resets a file's age");
        auto item_count = engine.state["items"].size();
        click(*panel, "button:全部归档");
        require(engine.stale().empty() && engine.state["archive"].size() == stale_count - 1,
                "Archive button performs actual file moves");
        click(*panel, "button:恢复已归档文件");
        require(engine.state["archive"].empty() && engine.state["items"].size() == item_count,
                "Restore button restores files and references");
        panel->review_open = false;
        click(*panel, "new-task");
        text(*panel, "中文待办 · integration");
        key(*panel, ImGuiKey_Enter);
        require(engine.state["todos"].back()["text"] == "中文待办 · integration",
                "Task field handles Chinese text and Enter");
        click(*panel, "desktop-search");
        text(*panel, "XRD");
        require(query == "XRD" && engine.search(query).size() == 1,
                "Desktop search input matches a fixture (query=" + query +
                    ", matches=" + std::to_string(engine.search(query).size()) + ")");
        key(*panel, ImGuiKey_Escape);
        require(query.empty(), "Escape clears desktop search");
        auto project = zones[0].get();
        click(*project, "button:−");
        require(engine.zone(project->zone_id)->value("collapsed", false) &&
                    project->height == static_cast<int>(34 * project->scale),
                "Collapse changes native window height");
        click(*project, "button:+");
        require(!engine.zone(project->zone_id)->value("collapsed", false), "Expand restores zone contents");
        show_launcher();
        launcher->focus_search = false;
        frame(*launcher);
        require(launcher->width == static_cast<int>(568 * launcher->scale),
                "Launcher is scaled to the cursor monitor");
        auto tab_before = active_tab;
        key(*launcher, ImGuiKey_Tab);
        require(active_tab != tab_before, "Tab switches launcher groups");
        auto path = engine.state["items"][0]["path"].get<std::string>();
        drop(*launcher, Json(), {path}, {launcher->width - 5, launcher->height - 5});
        require(!engine.tab(active_tab)->at("items").empty(),
                "External drop without private payload is accepted");
        click(*launcher, "button:名称");
        require(engine.tab(active_tab)->value("sort", "") == "name", "Sort button updates active tab");
        auto moved = engine.tab(active_tab)->at("items")[0]["id"].get<std::string>();
        auto target = engine.state["tabs"][0]["id"].get<std::string>();
        frame(*launcher);
        Hotspot tab_spot{};
        for (auto &spot : launcher->hotspots)
            if (spot.tab == target && spot.before.empty()) {
                tab_spot = spot;
                break;
            }
        drop(*launcher, {{"type", "launcher"}, {"id", moved}}, {path},
             {tab_spot.rect.left + 2, tab_spot.rect.top + 2});
        require(engine.launch(moved) && engine.tab(target)->at("items").size() > 0,
                "Tab drop hit regions move launcher references");
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        auto marker = engine.data_dir / L"shortcut-marker.txt", shortcut = roots[0] / L"DeskEdge marker.lnk";
        ComPtr<IShellLinkW> link;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        require(SUCCEEDED(hr), "Create private Windows shortcut fixture");
        link->SetPath(exe);
        auto args = L"--write-marker \"" + marker.wstring() + L"\"";
        link->SetArguments(args.c_str());
        ComPtr<IPersistFile> persist;
        link.As(&persist);
        require(SUCCEEDED(persist->Save(shortcut.c_str(), TRUE)), "Shortcut stores launch arguments");
        engine.add_to_tab("tools", shortcut);
        active_tab = "tools";
        launcher->show(true);
        launcher->search.clear();
        launcher->focus_search = false;
        click(*launcher, "launcher-search");
        text(*launcher, "DeskEdge marker");
        require(engine.search(launcher->search, true, active_tab).size() == 1,
                "Launcher input searches across tabs");
        key(*launcher, ImGuiKey_Enter);
        pump(600);
        require(fs::exists(marker), "Enter launches a real .lnk and preserves its arguments");
        require(!launcher->visible(), "Launcher closes after launch");
        show_panel();
        click(*panel, "button:中文");
        require(engine.state["settings"]["language"] == "en", "Language button switches to English");
        click(*panel, "button:◒ Light");
        require(dark, "Theme button applies dark palette");
        panel->export_preview(engine.data_dir / L"previews" / L"panel-dark-en.png");
        click(*panel, "button:Settings");
        require(settings && settings->visible(), "Settings opens native window");
        frame(*settings);
        settings->export_preview(engine.data_dir / L"previews" / L"settings.png");
        key(*settings, ImGuiKey_Escape);
        require(!settings->visible(), "Escape closes settings");
        auto original_zones = zones.size();
        begin_draw();
        require(drawing && !draw_windows.empty(), "Draw command creates monitor overlays");
        auto &canvas = *draw_windows[0];
        frame(canvas);
        float sx = 760 * canvas.scale, sy = 300 * canvas.scale, ex = sx + 180 * canvas.scale,
              ey = sy + 120 * canvas.scale;
        frame(canvas, [=](ImGuiIO &io) {
            io.AddMousePosEvent(sx, sy);
            io.AddMouseButtonEvent(0, true);
        });
        frame(canvas, [=](ImGuiIO &io) { io.AddMousePosEvent(ex, ey); });
        frame(canvas, [=](ImGuiIO &io) {
            io.AddMousePosEvent(ex, ey);
            io.AddMouseButtonEvent(0, false);
        });
        perform_actions();
        require(!drawing && zones.size() == original_zones + 1,
                "Drawing creates a region and removes overlays");
        auto drawn = engine.zone(zones.back()->zone_id);
        require(int(drawn->value("x", 0.f)) % 24 == 0 && int(drawn->value("y", 0.f)) % 24 == 0 &&
                    int(drawn->value("w", 0.f)) % 24 == 0 && int(drawn->value("h", 0.f)) % 24 == 0,
                "Newly drawn regions use the same position and size lattice");
        require(!zones.back()->rename_id.empty(), "New region enters title editing");
        auto new_region = zones.back().get();
        frame(*new_region);
        frame(*new_region);
        text(*new_region, " · 实验区域");
        key(*new_region, ImGuiKey_Enter);
        require(engine.zone(new_region->zone_id)->value("name", "").find("实验区域") != std::string::npos,
                "New region title accepts Chinese text and saves");
        toggle_peek();
        require(peek && !zones[0]->visible(), "Peek hides zones");
        toggle_peek();
        require(!peek && zones[0]->visible(), "Peek restores zones");
        auto watched = roots[0] / L"watcher-created.txt";
        {
            std::ofstream f(watched);
            f << "watch fixture";
        }
        pump(700);
        std::string watch_id;
        for (auto &item : engine.state["items"])
            if (item["path"] == pathstr(watched))
                watch_id = item["id"];
        require(!watch_id.empty(), "Native directory event imports a newly created file");
        auto renamed = roots[0] / L"watcher-renamed.txt";
        fs::rename(watched, renamed);
        pump(700);
        require(engine.item(watch_id) && engine.item(watch_id)->at("path") == pathstr(renamed),
                "Native rename event preserves file identity");
        auto shell_source = engine.data_dir / L"native-source";
        fs::create_directories(shell_source);
        auto make_file = [&](const wchar_t *name) {
            auto path = shell_source / name;
            std::ofstream stream(path);
            stream << "DeskEdge native Shell fixture";
            return path;
        };
        auto copy_source = make_file(L"复制测试.txt");
        write_log(engine.data_dir, "Binding native file context menu");
        auto menu = shell_context(panel->hwnd, {pathstr(copy_source)});
        write_log(engine.data_dir, "Querying native file context menu");
        auto popup = CreatePopupMenu();
        auto menu_result = menu->QueryContextMenu(popup, 0, 1, 0x6FFF, CMF_NORMAL | CMF_CANRENAME);
        require(SUCCEEDED(menu_result) && GetMenuItemCount(popup) > 5,
                "Windows provides the actual file context menu");
        std::vector<std::string> verbs;
        for (UINT n = 0; n < static_cast<UINT>(HRESULT_CODE(menu_result)); ++n) {
            char verb[256]{};
            if (SUCCEEDED(menu->GetCommandString(n, GCS_VERBA, nullptr, verb, sizeof(verb))))
                verbs.emplace_back(verb);
        }
        DestroyMenu(popup);
        for (auto verb : {"copy", "cut", "delete", "properties", "rename"})
            require(std::find(verbs.begin(), verbs.end(), verb) != verbs.end(),
                    std::string("Real Shell menu exposes ") + verb);
        write_log(engine.data_dir, "Releasing file context menu");
        menu.Reset();
        write_log(engine.data_dir, "File context menu released");
        auto background = shell_context(panel->hwnd, {}, roots[0]);
        popup = CreatePopupMenu();
        require(SUCCEEDED(background->QueryContextMenu(popup, 0, 1, 0x6FFF, CMF_NORMAL)) &&
                    GetMenuItemCount(popup) > 0,
                "Windows provides the folder background menu");
        DestroyMenu(popup);
        write_log(engine.data_dir, "Releasing background context menu");
        background.Reset();
        write_log(engine.data_dir, "Background context menu released");
        auto data = shell_data({pathstr(copy_source)}, {{"type", "test"}});
        FORMATETC shell_format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr,
                               DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        require(SUCCEEDED(data->QueryGetData(&shell_format)), "Outgoing drag retains Shell ID lists");
        require(data_files(data.Get()) == std::vector<std::string>{pathstr(copy_source)} &&
                    data_payload(data.Get()).value("type", "") == "test",
                "Native Shell data retains Unicode paths and DeskEdge grouping metadata");
        auto &drop_window = *zones[0];
        auto native_drop = [&](const fs::path &source, DWORD keys, DWORD expected) {
            auto object = shell_data({pathstr(source)});
            POINT point{2, 40};
            ClientToScreen(drop_window.hwnd, &point);
            POINTL where{point.x, point.y};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            require(SUCCEEDED(drop_window.drop_target->DragEnter(object.Get(), keys | MK_LBUTTON, where,
                                                                 &effect)) &&
                        effect == expected,
                    "Native drop cursor effect " + std::to_string(expected));
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            require(SUCCEEDED(drop_window.drop_target->Drop(object.Get(), keys, where, &effect)),
                    "Native Shell drop completes");
            pump(700);
        };
        native_drop(copy_source, MK_CONTROL, DROPEFFECT_COPY);
        require(fs::exists(copy_source) && fs::exists(roots[0] / copy_source.filename()),
                "Ctrl drag copies the real file");
        auto move_source = make_file(L"移动测试.txt");
        native_drop(move_source, 0, DROPEFFECT_MOVE);
        require(!fs::exists(move_source) && fs::exists(roots[0] / move_source.filename()),
                "Same-volume drag moves the real file");
        auto link_source = make_file(L"快捷方式测试.txt");
        native_drop(link_source, MK_CONTROL | MK_SHIFT, DROPEFFECT_LINK);
        bool linked = false;
        for (auto &entry : fs::directory_iterator(roots[0]))
            if (entry.path().extension() == L".lnk" &&
                entry.path().filename().wstring().find(L"快捷方式测试") != std::wstring::npos)
                linked = true;
        require(fs::exists(link_source) && linked, "Ctrl+Shift drag creates a genuine Windows shortcut");
        auto file = roots[0] / move_source.filename(), renamed_file = roots[0] / L"原生重命名.txt";
        shell_rename(panel->hwnd, file, pathstr(renamed_file.filename()));
        pump(700);
        require(!fs::exists(file) && fs::exists(renamed_file),
                "IFileOperation renames the real Unicode file");
        auto nested = roots[0] / L"Native drop folder";
        fs::create_directories(nested);
        auto nested_source = make_file(L"文件夹拖入.txt");
        auto nested_data = shell_data({pathstr(nested_source)});
        auto folder_target = shell_drop_target(nested);
        POINTL location{1, 1};
        DWORD folder_effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        require(SUCCEEDED(folder_target->DragEnter(nested_data.Get(), MK_LBUTTON | MK_CONTROL, location,
                                                   &folder_effect)) &&
                    folder_effect == DROPEFFECT_COPY,
                "Windows folder accepts the native file data object");
        folder_effect = DROPEFFECT_COPY;
        folder_target->Drop(nested_data.Get(), MK_LBUTTON | MK_CONTROL, location, &folder_effect);
        pump(700);
        require(fs::exists(nested_source) && fs::exists(nested / nested_source.filename()),
                "Dropping onto a folder writes inside that folder");
        folder_target.Reset();
        nested_data.Reset();
        auto selected_copy = engine.add_file(roots[0] / copy_source.filename(), drop_window.zone_id);
        auto selected_move = engine.add_file(renamed_file, drop_window.zone_id);
        frame(drop_window);
        frame(drop_window, [](ImGuiIO &io) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_A, true);
        });
        frame(drop_window, [](ImGuiIO &io) {
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        });
        require(drop_window.selection.size() >= 2, "Ctrl+A selects multiple files in the region");
        auto group_data =
            shell_data({pathstr(roots[0] / copy_source.filename()), pathstr(renamed_file)},
                       {{"type", "desktop"}, {"id", selected_copy}, {"ids", {selected_copy, selected_move}}});
        auto &other_zone = *zones[1];
        POINT group_point{2, 40};
        ClientToScreen(other_zone.hwnd, &group_point);
        POINTL group_location{group_point.x, group_point.y};
        DWORD group_effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        other_zone.drop_target->DragEnter(group_data.Get(), MK_LBUTTON, group_location, &group_effect);
        require(group_effect == DROPEFFECT_MOVE, "Internal multi-file drag advertises regrouping");
        group_effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        other_zone.drop_target->Drop(group_data.Get(), MK_LBUTTON, group_location, &group_effect);
        require(engine.item(selected_copy)->at("zone") == other_zone.zone_id &&
                    engine.item(selected_move)->at("zone") == other_zone.zone_id && fs::exists(renamed_file),
                "Internal multi-file drop regroups without moving desktop files");
        other_zone.selection = {selected_copy};
        key(other_zone, ImGuiKey_F2);
        frame(other_zone);
        frame(other_zone);
        text(other_zone, "文件键盘重命名");
        key(other_zone, ImGuiKey_Enter);
        pump(500);
        require(fs::exists(roots[0] / L"文件键盘重命名.txt") &&
                    engine.item(selected_copy)->at("path") == pathstr(roots[0] / L"文件键盘重命名.txt"),
                "F2 and Enter rename in place while preserving the extension and file identity");
        {
            struct RestoreClipboard {
                ComPtr<IDataObject> original;
                RestoreClipboard() {
                    OleGetClipboard(&original);
                }
                ~RestoreClipboard() {
                    OleSetClipboard(original.Get());
                    OleFlushClipboard();
                }
            } restore_clipboard;
            auto clipboard_source = make_file(L"剪贴板复制.txt");
            shell_verb(panel->hwnd, {pathstr(clipboard_source)}, "copy");
            ComPtr<IDataObject> clipboard;
            require(SUCCEEDED(OleGetClipboard(&clipboard)) &&
                        data_files(clipboard.Get()) == std::vector<std::string>{pathstr(clipboard_source)},
                    "Shell Copy places native file data on the clipboard");
            frame(other_zone, [](ImGuiIO &io) {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_V, true);
            });
            frame(other_zone, [](ImGuiIO &io) {
                io.AddKeyEvent(ImGuiKey_V, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
            });
            perform_actions();
            pump(700);
            require(fs::exists(clipboard_source) && fs::exists(roots[0] / clipboard_source.filename()),
                    "Ctrl+V pastes an actual copied file into the region");
            clipboard.Reset();
            auto cut_source = make_file(L"剪贴板剪切.txt");
            shell_verb(panel->hwnd, {pathstr(cut_source)}, "cut");
            frame(other_zone, [](ImGuiIO &io) {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_V, true);
            });
            frame(other_zone, [](ImGuiIO &io) {
                io.AddKeyEvent(ImGuiKey_V, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
            });
            perform_actions();
            pump(700);
            require(!fs::exists(cut_source) && fs::exists(roots[0] / cut_source.filename()),
                    "Ctrl+V honors the Shell Cut move effect");
        }
        auto batch_csv = roots[0] / L"batch-fixture.csv";
        {
            std::ofstream stream(batch_csv);
            stream << "x,y\n1,2\n";
        }
        auto batch_csv_id = engine.add_file(batch_csv, other_zone.zone_id);
        other_zone.selection = {selected_copy, batch_csv_id};
        frame(other_zone);
        key(other_zone, ImGuiKey_F2);
        frame(other_zone);
        frame(other_zone);
        text(other_zone, "批量文件");
        key(other_zone, ImGuiKey_Enter);
        pump(500);
        require(fs::exists(roots[0] / L"批量文件 (1).txt") && fs::exists(roots[0] / L"批量文件 (2).csv"),
                "Multiple-file F2 uses numbered names and preserves each file type");
        {
            engine.state["settings"]["language"] = "zh";
            engine.state["settings"]["theme"] = "light";
            auto grid_id = engine.add_zone(700, 450, 380, 310, monitor().name);
            auto source_a = make_file(L"格点 A.csv"), source_b = make_file(L"格点 B.txt");
            auto icon_folder = roots[0] / L"清晰文件夹";
            fs::create_directories(icon_folder);
            auto file_a = engine.add_file(source_a, grid_id), file_b = engine.add_file(source_b, grid_id);
            engine.add_file(icon_folder, grid_id);
            commit();
            Window *grid = nullptr;
            for (auto &zone : zones)
                if (zone->zone_id == grid_id)
                    grid = zone.get();
            require(grid != nullptr, "Grid test region uses a real native window");
            frame(*grid);
            auto cell_point = [&](GridCell cell) {
                POINT point{grid->grid_rect.left +
                                static_cast<LONG>((cell.column + .5f) * GRID_CELL_WIDTH * grid->scale),
                            grid->grid_rect.top +
                                static_cast<LONG>((cell.row + .5f) * GRID_CELL_HEIGHT * grid->scale -
                                                  grid->grid_scroll)};
                require(grid->grid_at(point) == cell, "Pointer maps to the requested physical grid cell");
                ClientToScreen(grid->hwnd, &point);
                return POINTL{point.x, point.y};
            };
            auto original_b = engine.item(file_b)->at("grid");
            auto grid_data =
                shell_data({pathstr(source_a)}, {{"type", "desktop"}, {"id", file_a}, {"ids", {file_a}}});
            auto grid_location = cell_point({3, 1});
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            grid->drop_target->DragEnter(grid_data.Get(), MK_LBUTTON, grid_location, &effect);
            require(effect == DROPEFFECT_MOVE && grid->drop_hover,
                    "Internal grid drag advertises a move and landing preview");
            frame(*grid);
            grid->export_preview(engine.data_dir / L"previews" / L"grid-landing.png");
            require(engine.item(file_b)->at("grid") == original_b &&
                        engine.item(file_a)->at("grid")["row"] == 0,
                    "Landing preview leaves saved positions unchanged until drop");
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            grid->drop_target->Drop(grid_data.Get(), MK_LBUTTON, grid_location, &effect);
            frame(*grid);
            require(engine.item(file_a)->at("grid") == Json({{"column", 3}, {"row", 1}}) &&
                        engine.item(file_b)->at("grid") == original_b && fs::exists(source_a),
                    "Native grid drop keeps the chosen empty cell and preserves other icons and files");
            auto landing_rect = grid->test_controls.at("file:" + file_a);
            require(std::abs(landing_rect.x - (grid->grid_rect.left + 3 * GRID_CELL_WIDTH * grid->scale)) < 2,
                    "Rendered icon occupies the persisted destination cell");
            auto occupied_cell = GridCell{original_b["column"], original_b["row"]};
            grid_location = cell_point(occupied_cell);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            grid->drop_target->DragEnter(grid_data.Get(), MK_LBUTTON, grid_location, &effect);
            require(effect == DROPEFFECT_MOVE, "Occupied non-folder cells remain valid grid targets");
            grid->drop_target->Drop(grid_data.Get(), MK_LBUTTON, grid_location, &effect);
            require(engine.item(file_b)->at("grid") == Json({{"column", 3}, {"row", 1}}),
                    "Occupied-cell drop swaps the resident icon into the vacated cell");
            auto group = shell_data({pathstr(source_a), pathstr(source_b)},
                                    {{"type", "desktop"}, {"id", file_b}, {"ids", {file_a, file_b}}});
            grid_location = cell_point({2, 1});
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            grid->drop_target->DragEnter(group.Get(), MK_LBUTTON, grid_location, &effect);
            grid->drop_target->Drop(group.Get(), MK_LBUTTON, grid_location, &effect);
            require(engine.item(file_a)->at("grid") == Json({{"column", 0}, {"row", 0}}) &&
                        engine.item(file_b)->at("grid") == Json({{"column", 2}, {"row", 1}}),
                    "Multi-icon native drop preserves the group's relative arrangement");
            frame(*grid);
            auto imported_source = make_file(L"外部落位.csv");
            auto external_data = shell_data({pathstr(imported_source)}, {{"type", "external"}});
            grid_location = cell_point({3, 2});
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            grid->drop_target->DragEnter(external_data.Get(), MK_LBUTTON | MK_CONTROL, grid_location,
                                         &effect);
            grid->drop_target->Drop(external_data.Get(), MK_LBUTTON | MK_CONTROL, grid_location, &effect);
            std::string imported_id;
            for (auto &item : engine.state["items"])
                if (item["path"] == pathstr(roots[0] / imported_source.filename()))
                    imported_id = item["id"];
            require(!imported_id.empty() && fs::exists(imported_source) &&
                        engine.item(imported_id)->at("grid") == Json({{"column", 3}, {"row", 2}}),
                    "Native external copy lands its new icon in the indicated cell");
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, 32768);
            auto executable_path = utf8(executable);
            icons->get(executable_path, 64);
            icons->get(executable_path, 96);
            ComPtr<ID3D11ShaderResourceView> at_64, at_96;
            for (int attempt = 0; attempt < 30 && (!at_64 || !at_96); ++attempt) {
                pump(100);
                at_64 = icons->get(executable_path, 64);
                at_96 = icons->get(executable_path, 96);
            }
            require(at_64 && at_96 && at_64 != at_96,
                    "Shell icon cache retains independent physical-size variants");
            ComPtr<ID3D11Resource> resource;
            at_64->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            resource.As(&texture);
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            require(description.Width >= 64 && description.Height >= 64,
                    "125-percent DPI icon rendering uses a high-resolution Shell texture instead of a "
                    "32-pixel stretch");
            frame(*grid);
            grid->export_preview(engine.data_dir / L"previews" / L"grid-occupied.png");
            engine.save();
            Engine reopened(engine.data_dir);
            require(reopened.item(file_b)->at("grid") == engine.item(file_b)->at("grid"),
                    "Native drop positions survive a fresh Engine load");
            auto shortcut_id = engine.add_file(shortcut, grid_id);
            engine.ensure_grid();
            frame(*grid);
            auto shortcut_cell = engine.item(shortcut_id)->at("grid");
            auto shortcut_point = cell_point({shortcut_cell["column"], shortcut_cell["row"]});
            auto shortcut_handler = shell_drop_target(shortcut, grid->hwnd);
            DWORD native_effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            require(SUCCEEDED(shortcut_handler->DragEnter(grid_data.Get(), MK_LBUTTON, shortcut_point,
                                                          &native_effect)),
                    "Program shortcut exposes its native Shell drop handler");
            shortcut_handler->DragLeave();
            auto unchanged = engine.item(file_a)->at("grid");
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            grid->drop_target->DragEnter(grid_data.Get(), MK_LBUTTON, shortcut_point, &effect);
            require(effect == native_effect && engine.item(file_a)->at("grid") == unchanged,
                    "Internal drag over a program shortcut preserves the native Shell effect");
            grid->drop_target->DragLeave();
        }
        engine.save();
    } catch (const std::exception &ex) {
        panel->export_preview(engine.data_dir / L"previews" / L"failure.png");
        checks.push_back({{"error", ex.what()}});
        result = 1;
        write_log(engine.data_dir, ex.what());
    }
    Json report = {{"mode", "Own ImGui input queue + native HWND/D3D/Shell/file operations"},
                   {"desktop_hosted", !no_desktop},
                   {"passed", result == 0},
                   {"checks", checks},
                   {"frames", rendered_frames.load()}};
    std::ofstream output(engine.data_dir / L"ui-test.json");
    output << report.dump(2);
    return result;
}
} // namespace deskedge
