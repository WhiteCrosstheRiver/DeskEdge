#include "imgui.h"
#include "imgui_stdlib.h"
#include "platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace deskedge {
namespace {
float u(Window &w, float value) {
    return value * w.scale;
}
ImU32 color(Window &, int rgb, int alpha = 255) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha);
}
ImU32 text_color(Window &w, bool muted = false) {
    return color(w, w.app.dark ? (muted ? 0xA9ABB1 : 0xF1F1F3) : (muted ? 0x72757C : 0x24262B));
}
void font(Window &, float size) {
    ImGui::PushFont(nullptr, size);
}
void label(Window &w, std::string text, bool muted = false, float size = 12) {
    font(w, size);
    if (muted)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text.c_str());
    if (muted)
        ImGui::PopStyleColor();
    ImGui::PopFont();
}
void record(Window &w, const std::string &ident) {
    if (!w.app.self_test_ui)
        return;
    auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    w.test_controls[ident] = {a.x, a.y, b.x, b.y};
}
bool button(Window &w, std::string text, float width = 0, bool primary = false) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.08f, .42f, .72f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.10f, .49f, .80f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    }
    bool clicked = ImGui::Button(text.c_str(), {u(w, width), 0});
    record(w, "button:" + text);
    if (w.app.self_test_ui && clicked)
        record(w, "clicked:" + text);
    if (primary)
        ImGui::PopStyleColor(3);
    return clicked;
}
void hint(std::string text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text.c_str());
        ImGui::EndTooltip();
    }
}
void section(Window &w, const char *zh, const char *en) {
    ImGui::Dummy({0, u(w, 5)});
    label(w, w.app.tr(zh, en), true, 11);
    ImGui::Dummy({0, u(w, 1)});
}
void logo(Window &w, ImVec2 p, float size) {
    auto draw = ImGui::GetWindowDrawList();
    float s = u(w, size);
    draw->AddRectFilled(p, {p.x + s, p.y + s}, color(w, 0x146AB5), u(w, 5));
    draw->AddRectFilled({p.x + s * .21f, p.y + s * .21f}, {p.x + s * .69f, p.y + s * .34f}, IM_COL32_WHITE,
                        u(w, 1));
    draw->AddRectFilled({p.x + s * .21f, p.y + s * .44f}, {p.x + s * .69f, p.y + s * .57f}, IM_COL32_WHITE,
                        u(w, 1));
    draw->AddRectFilled({p.x + s * .76f, p.y + s * .20f}, {p.x + s * .84f, p.y + s * .80f}, IM_COL32_WHITE,
                        u(w, 1));
}
void icon(Window &w, std::string path, ImVec2 p, float size) {
    auto draw = ImGui::GetWindowDrawList();
    float s = std::round(u(w, size));
    p = {std::round(p.x), std::round(p.y)};
    auto tex = w.app.icons->get(path, static_cast<int>(s));
    if (tex)
        draw->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(tex)), p, {p.x + s, p.y + s});
    else {
        draw->AddRectFilled({p.x + s * .15f, p.y}, {p.x + s * .85f, p.y + s}, color(w, 0x82ACD7), u(w, 3));
        draw->AddLine({p.x + s * .3f, p.y + s * .4f}, {p.x + s * .65f, p.y + s * .4f}, IM_COL32_WHITE,
                      u(w, 1));
    }
}
// Split labels only at UTF-8 character boundaries, then ellipsize the second line.
void tile_name(Window &w, const std::string &text, ImVec2 p, float width, bool muted = false,
               bool compact = false) {
    font(w, compact ? 10.f : 11.f);
    std::vector<std::string> lines(1);
    for (size_t n = 0; n < text.size();) {
        size_t count = 1;
        unsigned char c = static_cast<unsigned char>(text[n]);
        if (c >= 0xF0)
            count = 4;
        else if (c >= 0xE0)
            count = 3;
        else if (c >= 0xC0)
            count = 2;
        auto next = text.substr(n, count);
        if (ImGui::CalcTextSize((lines.back() + next).c_str()).x > width && !lines.back().empty()) {
            if (lines.size() == (compact ? 1 : 2)) {
                while (!lines.back().empty() && ImGui::CalcTextSize((lines.back() + "…").c_str()).x > width) {
                    size_t end = lines.back().size() - 1;
                    while (end > 0 && (static_cast<unsigned char>(lines.back()[end]) & 0xC0) == 0x80)
                        --end;
                    lines.back().resize(end);
                }
                lines.back() += "…";
                break;
            }
            lines.push_back("");
        }
        lines.back() += next;
        n += count;
    }
    auto d = ImGui::GetWindowDrawList();
    for (size_t n = 0; n < lines.size(); n++) {
        auto size = ImGui::CalcTextSize(lines[n].c_str());
        d->AddText({p.x + (width - size.x) / 2, p.y + u(w, 13) * n}, text_color(w, muted), lines[n].c_str());
    }
    ImGui::PopFont();
}
void hotspot(Window &w, ImVec2 p, ImVec2 size, std::string tab, std::string before = "") {
    w.hotspots.push_back({{static_cast<LONG>(p.x), static_cast<LONG>(p.y), static_cast<LONG>(p.x + size.x),
                           static_cast<LONG>(p.y + size.y)},
                          std::move(tab),
                          std::move(before)});
}
void drag_item(Window &w, const Json &item, bool launcher) {
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, u(w, 5)) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right, u(w, 5))) &&
        !w.drag_in_progress) {
        w.drag_in_progress = true;
        auto ident = item["id"].get<std::string>(), path = item["path"].get<std::string>();
        std::vector<std::string> paths{path}, ids{ident};
        if (!launcher && std::find(w.selection.begin(), w.selection.end(), ident) != w.selection.end()) {
            paths.clear();
            ids.clear();
            for (auto &selected : w.selection)
                if (auto p = w.app.engine.item(selected)) {
                    paths.push_back((*p)["path"]);
                    ids.push_back(selected);
                }
        }
        w.app.defer([&w, ident, paths, ids, launcher] {
            try {
                native_drag(w.hwnd,
                            {{"type", launcher ? "launcher" : "desktop"}, {"id", ident}, {"ids", ids}},
                            paths);
            } catch (...) {
                w.drag_in_progress = false;
                w.invalidate();
                throw;
            }
            w.drag_in_progress = false;
            ImGui::SetCurrentContext(w.context);
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            ImGui::GetIO().AddMouseButtonEvent(1, false);
            w.invalidate();
        });
    }
}
void begin_file_rename(Window &w, const std::string &ident) {
    auto item = w.app.engine.item(ident);
    if (!item)
        return;
    auto path = fs::path(wide((*item)["path"].get<std::string>()));
    SHFILEINFOW info{};
    SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME);
    w.file_rename = ident;
    w.file_rename_ids = std::find(w.selection.begin(), w.selection.end(), ident) != w.selection.end()
                            ? w.selection
                            : std::vector<std::string>{ident};
    w.file_rename_buffer = *info.szDisplayName ? utf8(info.szDisplayName) : pathstr(path.filename());
    w.file_rename_suffix =
        path_equal(w.file_rename_buffer, pathstr(path.stem())) ? pathstr(path.extension()) : "";
    w.file_rename_focus = true;
    w.file_rename_select = true;
    w.show(true);
}
void native_item_menu(Window &w, Json item, bool launch) {
    auto &a = w.app;
    std::vector<std::string> paths{item["path"]};
    if (!launch && std::find(w.selection.begin(), w.selection.end(), item["id"].get<std::string>()) !=
                       w.selection.end()) {
        paths.clear();
        for (auto &ident : w.selection)
            if (auto p = a.engine.item(ident))
                paths.push_back((*p)["path"]);
    }
    auto context = shell_context(w.hwnd, paths);
    auto menu = CreatePopupMenu();
    auto entry = [&](int id, const char *zh, const char *en) {
        AppendMenuW(menu, MF_STRING, id + 0x8000, wide(a.tr(zh, en)).c_str());
    };
    entry(1, "打开", "Open");
    entry(2, "在资源管理器中显示", "Show in Explorer");
    entry(3, "加入启动器", "Add to launcher");
    entry(4, "固定到常用应用", "Pin to favorites");
    if (!launch)
        entry(5, "保留，重新计时", "Keep and reset age");
    auto group = CreatePopupMenu();
    std::vector<std::string> targets;
    int base = 100;
    for (auto &t : a.engine.state[launch ? "tabs" : "zones"]) {
        AppendMenuW(group, MF_STRING, base++ + 0x8000, wide(a.engine.name(t)).c_str());
        targets.push_back(t["id"]);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(group),
                wide(a.tr("移动到分组", "Move to group")).c_str());
    entry(6, "从列表移除", "Remove from list");
    a.modal = true;
    std::string verb;
    int selected;
    try {
        selected = static_cast<int>(shell_menu(w.hwnd, context.Get(), menu, &verb)) - 0x8000;
    } catch (...) {
        a.modal = false;
        throw;
    }
    a.modal = false;
    auto path = item["path"].get<std::string>(), ident = item["id"].get<std::string>();
    if (verb == "open" || verb == "openas" || verb == "runas")
        for (auto &target : paths)
            a.engine.opened(target);
    if (verb == "rename" && !launch) {
        begin_file_rename(w, ident);
    }
    if (selected == 1)
        a.open(path);
    else if (selected == 2)
        reveal(path);
    else if (selected == 3)
        a.engine.add_to_tab(a.active_tab, wide(path), item.value("name", ""));
    else if (selected == 4) {
        bool duplicate = false;
        for (auto &f : a.engine.state["favorites"])
            if (path_equal(f["path"].get<std::string>(), path))
                duplicate = true;
        if (!duplicate)
            a.engine.state["favorites"].push_back(
                {{"id", id()}, {"name", item["name"]}, {"path", path}, {"uses", 0}, {"used", 0}});
    } else if (selected == 5)
        a.engine.keep(ident);
    else if (selected == 6) {
        if (launch) {
            auto &favorites = a.engine.state["favorites"];
            favorites.erase(
                std::remove_if(favorites.begin(), favorites.end(), [&](auto &i) { return i["id"] == ident; }),
                favorites.end());
            for (auto &t : a.engine.state["tabs"]) {
                auto &items = t["items"];
                items.erase(
                    std::remove_if(items.begin(), items.end(), [&](auto &i) { return i["id"] == ident; }),
                    items.end());
            }
        } else {
            auto &items = a.engine.state["items"];
            items.erase(std::remove_if(items.begin(), items.end(), [&](auto &i) { return i["id"] == ident; }),
                        items.end());
        }
    } else if (selected >= 100 && selected < 100 + static_cast<int>(targets.size())) {
        if (launch) {
            if (a.engine.launch(ident))
                a.engine.move_launch(ident, targets[selected - 100]);
            else
                a.engine.add_to_tab(targets[selected - 100], wide(path), item.value("name", ""));
        } else
            a.engine.move_zone(ident, targets[selected - 100]);
    }
}
void paste_files(Window &w, bool shortcut = false) {
    ComPtr<IDataObject> data;
    if (FAILED(OleGetClipboard(&data)))
        return;
    DWORD preferred = DROPEFFECT_COPY;
    FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)), nullptr,
                     DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&format, &medium))) {
        auto effect = static_cast<const DWORD *>(GlobalLock(medium.hGlobal));
        if (effect && GlobalSize(medium.hGlobal) >= sizeof(DWORD))
            preferred = *effect;
        if (effect)
            GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
    }
    DWORD keys = shortcut ? MK_CONTROL | MK_SHIFT : preferred & DROPEFFECT_MOVE ? MK_SHIFT : MK_CONTROL;
    POINT point{2, 40};
    ClientToScreen(w.hwnd, &point);
    POINTL where{point.x, point.y};
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    if (SUCCEEDED(w.drop_target->DragEnter(data.Get(), keys | MK_LBUTTON, where, &effect)) && effect) {
        effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        w.drop_target->Drop(data.Get(), keys, where, &effect);
    } else
        w.drop_target->DragLeave();
}
void zone_background(Window &w) {
    auto &a = w.app;
    auto context = shell_context(w.hwnd, {}, a.roots.at(0));
    auto extra = CreatePopupMenu();
    for (auto entry :
         std::vector<std::pair<UINT, std::string>>{{0x8001, a.tr("添加文件引用", "Add file references")},
                                                   {0x8002, a.tr("重命名区域", "Rename zone")},
                                                   {0x8003, a.tr("切换临时区域", "Toggle temporary zone")},
                                                   {0x8004, a.tr("移除区域", "Remove zone")},
                                                   {0x8005, a.tr("重新整理图标", "Arrange icons")}})
        AppendMenuW(extra, MF_STRING, entry.first, wide(entry.second).c_str());
    std::unordered_set<std::string> before;
    for (auto &item : a.engine.state["items"])
        before.insert(lower(item["path"].get<std::string>()));
    a.modal = true;
    UINT command;
    try {
        command = shell_menu(w.hwnd, context.Get(), extra);
    } catch (...) {
        a.modal = false;
        throw;
    }
    a.modal = false;
    auto ident = w.zone_id;
    if (command == 0x8001)
        a.add_files(ident);
    else if (command == 0x8002) {
        w.rename_id = ident;
        w.rename_buffer = a.engine.name(*a.engine.zone(ident));
        w.focus_search = true;
        w.show(true);
    } else if (command == 0x8003) {
        auto zone = a.engine.zone(ident);
        (*zone)["temporary"] = !zone->value("temporary", false);
    } else if (command == 0x8004)
        a.engine.remove_zone(ident);
    else if (command == 0x8005)
        a.engine.arrange_grid(ident);
    a.engine.sync(a.roots, a.archive_root);
    if (a.engine.zone(ident))
        for (auto &item : a.engine.state["items"])
            if (!before.contains(lower(item["path"].get<std::string>())))
                item["zone"] = ident;
}
bool tile(Window &w, const Json &item, bool launch, float width = 72, float height = 83, bool dim = false,
          bool selected = false, bool compact = false) {
    auto p = ImGui::GetCursorScreenPos();
    ImVec2 size{u(w, width), u(w, height)};
    auto ident = item["id"].get<std::string>();
    ImGui::PushID(ident.c_str());
    bool clicked = ImGui::InvisibleButton(
        "tile", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    clicked = clicked && ImGui::IsMouseReleased(0);
    record(w, "file:" + ident);
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (!launch) {
        auto contains = std::find(w.selection.begin(), w.selection.end(), ident) != w.selection.end();
        if (ImGui::IsItemClicked(0)) {
            auto &io = ImGui::GetIO();
            if (io.KeyShift && !w.selection_anchor.empty()) {
                std::vector<std::string> order = w.grid_order;
                auto begin = std::find(order.begin(), order.end(), w.selection_anchor),
                     end = std::find(order.begin(), order.end(), ident);
                if (begin != order.end() && end != order.end()) {
                    if (begin > end)
                        std::swap(begin, end);
                    if (!io.KeyCtrl)
                        w.selection.clear();
                    for (; begin <= end; ++begin)
                        if (std::find(w.selection.begin(), w.selection.end(), *begin) == w.selection.end())
                            w.selection.push_back(*begin);
                }
            } else if (io.KeyCtrl) {
                if (contains)
                    std::erase(w.selection, ident);
                else
                    w.selection.push_back(ident);
                w.selection_anchor = ident;
            } else {
                if (!contains)
                    w.selection = {ident};
                w.selection_anchor = ident;
            }
        }
        if (hovered && ImGui::IsMouseReleased(1) && !w.drag_in_progress && !contains) {
            w.selection = {ident};
            w.selection_anchor = ident;
        }
        selected = std::find(w.selection.begin(), w.selection.end(), ident) != w.selection.end() || selected;
        if (w.kind == Kind::Zone)
            w.hotspots.push_back({{static_cast<LONG>(p.x), static_cast<LONG>(p.y),
                                   static_cast<LONG>(p.x + size.x), static_cast<LONG>(p.y + size.y)},
                                  "",
                                  "",
                                  item["path"],
                                  item.value("folder", false)});
    }
    auto draw = ImGui::GetWindowDrawList();
    if (selected || hovered || active) {
        ImVec2 start{p.x + u(w, 3), p.y + u(w, 2)}, end{p.x + size.x - u(w, 3), p.y + size.y - u(w, 4)};
        draw->AddRectFilled(start, end, color(w, selected ? 0x619CE1 : 0x64748B, selected ? 30 : 15),
                            u(w, 7));
        if (selected)
            draw->AddRect(start, end, color(w, 0x488BDD, 100), u(w, 7));
    }
    if (dim)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, .34f);
    float icon_size = compact ? 32.f : launch ? 36.f : 40.f;
    icon(w, item["path"], {p.x + (size.x - u(w, icon_size)) / 2, p.y + u(w, compact ? 4.f : 8.f)}, icon_size);
    if (w.file_rename == ident) {
        ImGui::SetCursorScreenPos({p.x + u(w, 2), p.y + u(w, 52)});
        ImGui::SetNextItemWidth(size.x - u(w, 4));
        if (w.file_rename_focus) {
            ImGui::SetKeyboardFocusHere();
            w.file_rename_focus = false;
            w.settle_frames = 2;
        }
        if (ImGui::InputText(
                "##file-rename", &w.file_rename_buffer,
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                    ImGuiInputTextFlags_CallbackAlways,
                [](ImGuiInputTextCallbackData *data) -> int {
                    auto view = static_cast<Window *>(data->UserData);
                    if (view->file_rename_select) {
                        int end = data->BufTextLen;
                        auto item = view->app.engine.item(view->file_rename);
                        if (item && !item->value("folder", false) && view->file_rename_suffix.empty()) {
                            auto dot = strrchr(data->Buf, '.');
                            if (dot && dot != data->Buf)
                                end = static_cast<int>(dot - data->Buf);
                        }
                        data->SelectionStart = 0;
                        data->SelectionEnd = data->CursorPos = end;
                        view->file_rename_select = false;
                    }
                    return 0;
                },
                &w)) {
            auto name = w.file_rename_buffer + w.file_rename_suffix;
            std::vector<std::pair<fs::path, std::string>> changes;
            if (!w.file_rename_buffer.empty()) {
                auto base = item.value("folder", false) ? wide(name) : fs::path(wide(name)).stem().wstring();
                size_t number = 0;
                for (auto &file_id : w.file_rename_ids)
                    if (auto file = w.app.engine.item(file_id)) {
                        auto original = fs::path(wide((*file)["path"].get<std::string>()));
                        auto target =
                            w.file_rename_ids.size() == 1
                                ? name
                                : utf8(base + L" (" + std::to_wstring(++number) + L")" +
                                       (file->value("folder", false) ? std::wstring()
                                                                     : original.extension().wstring()));
                        changes.emplace_back(original, target);
                    }
            }
            w.file_rename.clear();
            if (!changes.empty())
                w.app.defer([&w, changes] {
                    shell_rename_batch(w.hwnd, changes);
                    for (auto &[original, target] : changes) {
                        auto renamed = original.parent_path() / wide(target);
                        if (fs::exists(renamed) && !fs::exists(original))
                            w.app.engine.rename_path(original, renamed);
                    }
                    w.app.engine.sync(w.app.roots, w.app.archive_root);
                });
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            w.file_rename.clear();
        ImGui::SetCursorScreenPos({p.x, p.y + size.y});
    } else
        tile_name(w,
                  launch ? item.value("name", "")
                         : w.app.icons->display_name(item["path"], item.value("name", "")),
                  {p.x + u(w, 3), p.y + u(w, compact  ? 40.f
                                             : launch ? 48.f
                                                      : 52.f)},
                  size.x - u(w, 6), dim, compact);
    if (launch && !compact) {
        std::string meta = item.value("uses", 0) > 0
                               ? std::to_string(item.value("uses", 0)) + w.app.tr(" 次", " uses")
                               : w.app.tr("未使用", "Not used");
        font(w, 10);
        auto tw = ImGui::CalcTextSize(meta.c_str()).x;
        draw->AddText({p.x + (size.x - tw) / 2, p.y + u(w, 74)}, text_color(w, true), meta.c_str());
        ImGui::PopFont();
    } else if (w.app.engine.age(item) >= w.app.engine.state["settings"].value("stale_days", 7)) {
        auto z = w.app.engine.zone(item.value("zone", ""));
        if (z && z->value("temporary", false) && !item.value("folder", false))
            draw->AddCircleFilled({p.x + size.x / 2 + u(w, 17), p.y + u(w, 7)}, u(w, 3), color(w, 0xCA853B));
    }
    if (dim)
        ImGui::PopStyleVar();
    hint(item.value("name", "") + "\n" + item.value("path", ""));
    drag_item(w, item, launch);
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !w.drag_in_progress &&
        w.file_rename.empty()) {
        auto copy = item;
        w.app.defer([&w, copy, launch] { native_item_menu(w, copy, launch); });
    }
    bool doubleclick = hovered && ImGui::IsMouseDoubleClicked(0);
    ImGui::PopID();
    if (doubleclick && !launch && !w.drag_in_progress) {
        auto path = item["path"].get<std::string>();
        w.app.defer([&w, path] { w.app.open(path); });
    }
    return clicked && !w.drag_in_progress;
}
void zone_ui(Window &w) {
    auto &a = w.app;
    auto z = a.engine.zone(w.zone_id);
    w.grid_rect = {};
    if (!z)
        return;
    auto ident = w.zone_id;
    std::erase_if(w.selection, [&](auto &selected) {
        auto p = a.engine.item(selected);
        return !p || (*p)["zone"] != ident;
    });
    if (!ImGui::GetIO().WantTextInput && !w.popup_open && w.file_rename.empty() && w.rename_id.empty()) {
        auto &io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            w.selection.clear();
            for (auto &item : a.engine.state["items"])
                if (item["zone"] == ident)
                    w.selection.push_back(item["id"]);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))
            a.defer([&w, link = io.KeyShift] { paste_files(w, link); });
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
            a.defer([&w, redo = io.KeyShift] {
                shell_folder_verb(w.hwnd, w.app.roots.at(0), redo ? "redo" : "undo");
                w.app.engine.sync(w.app.roots, w.app.archive_root);
            });
        std::vector<std::string> paths;
        for (auto &selected : w.selection)
            if (auto p = a.engine.item(selected))
                paths.push_back((*p)["path"]);
        const char *verb = nullptr;
        if (!paths.empty()) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
                verb = "copy";
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X))
                verb = "cut";
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                verb = "delete";
            if (verb)
                a.defer([&w, paths, verb] {
                    shell_verb(w.hwnd, paths, verb);
                    w.app.engine.sync(w.app.roots, w.app.archive_root);
                });
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                begin_file_rename(w, w.selection[0]);
            }
            if ((io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10)) || ImGui::IsKeyPressed(ImGuiKey_Menu)) {
                auto item = *a.engine.item(w.selection[0]);
                a.defer([&w, item] { native_item_menu(w, item, false); });
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter))
                a.defer([&a, paths] {
                    for (auto &path : paths)
                        a.open(path);
                });
        }
    }
    auto draw = ImGui::GetWindowDrawList();
    float ww = static_cast<float>(w.width), hh = static_cast<float>(w.height);
    draw->AddRectFilled({0, 0}, {ww, u(w, 32)}, color(w, a.dark ? 0xFFFFFF : 0xE7EAF0, a.dark ? 9 : 55),
                        u(w, 12), ImDrawFlags_RoundCornersTop);
    draw->AddLine({u(w, 12), u(w, 32)}, {ww - u(w, 12), u(w, 32)},
                  color(w, a.dark ? 0xFFFFFF : 0x596579, 18));
    draw->AddRectFilled({u(w, 12), u(w, 13)}, {u(w, 18), u(w, 19)}, color(w, z->value("color", 0x649CDA)),
                        u(w, 2));
    size_t count = 0;
    std::vector<std::string> items;
    for (auto &i : a.engine.state["items"])
        if (i["zone"] == ident) {
            ++count;
            items.push_back(i["id"]);
        }
    std::unordered_set<std::string> matches;
    if (!a.query.empty())
        for (auto &m : a.engine.search(a.query))
            matches.insert(m.id);
    ImGui::SetCursorPos({u(w, 26), u(w, 5)});
    if (w.rename_id == ident) {
        ImGui::SetNextItemWidth(ww - u(w, 91));
        if (w.focus_search) {
            ImGui::SetKeyboardFocusHere();
            w.focus_search = false;
        }
        if (ImGui::InputText("##rename", &w.rename_buffer,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            auto name = w.rename_buffer;
            w.rename_id.clear();
            a.defer([&a, ident, name] {
                if (auto p = a.engine.zone(ident)) {
                    (*p)["name"] = name.empty() ? a.tr("新区域", "New zone") : name;
                    (*p)["en"] = (*p)["name"];
                }
            });
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            w.rename_id.clear();
    } else {
        font(w, 12);
        ImGui::TextUnformatted(a.engine.name(*z).c_str());
        ImGui::PopFont();
        ImGui::SetCursorPos({u(w, 22), 0});
        ImGui::InvisibleButton("header", {ww - u(w, 80), u(w, 32)});
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            w.rename_id = ident;
            w.rename_buffer = a.engine.name(*z);
            w.focus_search = true;
            w.invalidate();
        }
    }
    ImGui::SetCursorPos({ww - u(w, 55), u(w, 8)});
    label(w, std::to_string(count), true, 10);
    ImGui::SetCursorPos({ww - u(w, 30), u(w, 3)});
    if (button(w, z->value("collapsed", false) ? "+" : "−", 24))
        a.defer([&a, ident] {
            if (auto p = a.engine.zone(ident))
                (*p)["collapsed"] = !p->value("collapsed", false);
        });
    hint(a.tr("折叠 / 展开", "Collapse / expand"));
    if (z->value("collapsed", false))
        return;
    ImGui::SetCursorPos({u(w, 12), u(w, 40)});
    ImGui::BeginChild("files", {ww - u(w, 24), hh - u(w, 50)}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    int columns = deskedge::grid_columns(ww / w.scale);
    w.grid_columns = columns;
    w.grid_scroll = ImGui::GetScrollY();
    auto viewport = ImGui::GetWindowPos();
    auto extent = ImGui::GetWindowSize();
    float grid_inset = std::max(0.f, (extent.x - columns * u(w, GRID_CELL_WIDTH)) / 2);
    ImGui::SetCursorPosX(grid_inset);
    viewport.x += grid_inset;
    w.grid_rect = {static_cast<LONG>(viewport.x), static_cast<LONG>(viewport.y),
                   static_cast<LONG>(viewport.x + columns * u(w, GRID_CELL_WIDTH)),
                   static_cast<LONG>(viewport.y + extent.y)};
    bool aligned = a.engine.state["settings"].value("grid_mode", true);
    auto layout = a.engine.zone_grid(ident, columns);
    if (!aligned)
        for (size_t i = 0; i < layout.size(); ++i)
            layout[i].cell = {static_cast<int>(i) % columns, static_cast<int>(i) / columns};
    std::sort(layout.begin(), layout.end(), [=](auto &left, auto &right) {
        return left.cell.row * columns + left.cell.column < right.cell.row * columns + right.cell.column;
    });
    w.grid_order.clear();
    std::unordered_map<int, std::string> occupied;
    int rows = std::max(1, static_cast<int>(extent.y / u(w, GRID_CELL_HEIGHT)));
    for (auto &placed : layout) {
        w.grid_order.push_back(placed.id);
        occupied[placed.cell.row * columns + placed.cell.column] = placed.id;
        rows = std::max(rows, placed.cell.row + 1);
    }
    auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGuiListClipper clipper;
    clipper.Begin(rows, u(w, GRID_CELL_HEIGHT));
    if (!w.file_rename.empty())
        for (auto &placed : layout)
            if (placed.id == w.file_rename)
                clipper.IncludeItemByIndex(placed.cell.row);
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            for (int col = 0; col < columns; ++col) {
                ImVec2 position{origin.x + col * u(w, GRID_CELL_WIDTH),
                                origin.y + row * u(w, GRID_CELL_HEIGHT)};
                ImGui::SetCursorScreenPos(position);
                if (w.drop_hover && aligned)
                    ImGui::GetWindowDrawList()->AddCircleFilled({position.x + 1, position.y + 1}, 1.f,
                                                                color(w, 0x5995D3, 70));
                auto found = occupied.find(row * columns + col);
                auto item = found == occupied.end() ? nullptr : a.engine.item(found->second);
                if (!item) {
                    ImGui::Dummy({u(w, GRID_CELL_WIDTH), u(w, GRID_CELL_HEIGHT)});
                    continue;
                }
                if (w.file_rename == found->second && w.file_rename_focus)
                    ImGui::SetScrollHereY(.5f);
                if (tile(w, *item, false, GRID_CELL_WIDTH, GRID_CELL_HEIGHT,
                         !a.query.empty() && !matches.contains(found->second), a.focus_id == found->second)) {
                    auto key = found->second;
                    a.defer([&a, key] { a.focus(key); });
                }
            }
    ImGui::PopStyleVar();
    if (w.drop_hover && aligned)
        if (auto target = w.grid_at(w.drop_point)) {
            ImVec2 position{origin.x + target->column * u(w, GRID_CELL_WIDTH),
                            origin.y + target->row * u(w, GRID_CELL_HEIGHT)};
            auto preview = ImGui::GetWindowDrawList();
            preview->AddRectFilled(
                {position.x + 2, position.y + 2},
                {position.x + u(w, GRID_CELL_WIDTH) - 2, position.y + u(w, GRID_CELL_HEIGHT) - 2},
                color(w, 0x488BDD, 18), u(w, 7));
            preview->AddRect(
                {position.x + 2, position.y + 2},
                {position.x + u(w, GRID_CELL_WIDTH) - 2, position.y + u(w, GRID_CELL_HEIGHT) - 2},
                color(w, 0x488BDD, 160), u(w, 7), 0, u(w, 1));
        }
    if (items.empty()) {
        ImGui::SetCursorScreenPos({origin.x + u(w, 6), origin.y + u(w, 8)});
        label(w, a.tr("拖入文件、文件夹或快捷方式", "Drop files, folders or shortcuts"), true, 11);
    }
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(0)) {
        w.selecting = true;
        w.selection_start = ImGui::GetMousePos();
        w.selection_base = ImGui::GetIO().KeyCtrl ? w.selection : std::vector<std::string>{};
        w.selection = w.selection_base;
        a.focus_id.clear();
    }
    if (w.selecting) {
        auto mouse = ImGui::GetMousePos();
        ImVec2 start{std::min(w.selection_start.x, mouse.x), std::min(w.selection_start.y, mouse.y)},
            end{std::max(w.selection_start.x, mouse.x), std::max(w.selection_start.y, mouse.y)};
        w.selection = w.selection_base;
        for (auto &spot : w.hotspots)
            if (!spot.path.empty() && spot.rect.left < end.x && spot.rect.right > start.x &&
                spot.rect.top < end.y && spot.rect.bottom > start.y)
                for (auto &item : a.engine.state["items"])
                    if (path_equal(item["path"].get<std::string>(), spot.path) &&
                        std::find(w.selection.begin(), w.selection.end(), item["id"].get<std::string>()) ==
                            w.selection.end())
                        w.selection.push_back(item["id"]);
        ImGui::GetWindowDrawList()->AddRectFilled(start, end, color(w, 0x5995D3, 40));
        ImGui::GetWindowDrawList()->AddRect(start, end, color(w, 0x5995D3, 180));
        if (!ImGui::IsMouseDown(0))
            w.selecting = false;
    }
    ImGui::EndChild();
    ImGui::SetCursorPos({ww - u(w, 15), hh - u(w, 15)});
    ImGui::InvisibleButton("resize", {u(w, 14), u(w, 14)});
    draw->AddLine({ww - u(w, 11), hh - u(w, 4)}, {ww - u(w, 4), hh - u(w, 11)}, text_color(w, true));
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseReleased(1) &&
        !ImGui::IsAnyItemHovered())
        a.defer([&w] { zone_background(w); });
}
void tidy_card(Window &w) {
    auto &a = w.app;
    auto stale = a.engine.stale();
    if ((stale.empty() || a.engine.state.value("snooze_until", int64_t(0)) > now()) && !w.review_open) {
        if (!a.engine.state["archive"].empty() && button(w, a.tr("恢复已归档文件", "Restore archived files")))
            a.defer([&a] { a.restore(); });
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          a.dark ? ImVec4(.25f, .20f, .14f, .6f) : ImVec4(1, .96f, .88f, .90f));
    ImGui::BeginChild("tidy", {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    label(w, a.tr("每周整理", "Weekly tidy"));
    label(w,
          std::to_string(stale.size()) + a.tr(" 个临时文件已超过 ", " temporary files older than ") +
              std::to_string(a.engine.state["settings"].value("stale_days", 7)) + a.tr(" 天", " days"),
          true, 11);
    if (!stale.empty()) {
        if (button(w, a.tr("全部归档", "Archive all"), 0, true))
            a.defer([&a, stale] { a.archive(stale); });
        ImGui::SameLine();
        if (button(w, a.tr("检查", "Review")))
            w.review_open = !w.review_open;
        ImGui::SameLine();
        if (button(w, a.tr("稍后", "Later")))
            a.defer([&a] { a.engine.state["snooze_until"] = now() + 7 * 86400; });
    }
    if (w.review_open)
        for (auto &ident : stale) {
            auto item = a.engine.item(ident);
            if (!item)
                continue;
            ImGui::PushID(ident.c_str());
            ImGui::Separator();
            font(w, 11);
            ImGui::TextWrapped("%s", item->value("name", "").c_str());
            ImGui::PopFont();
            label(w, std::to_string(a.engine.age(*item)) + a.tr(" 天未使用", " days unused"), true, 10);
            if (button(w, a.tr("保留", "Keep")))
                a.defer([&a, ident] { a.engine.keep(ident); });
            ImGui::SameLine();
            if (button(w, a.tr("归档", "Archive")))
                a.defer([&a, ident] { a.archive({ident}); });
            ImGui::PopID();
        }
    if (!a.engine.state["archive"].empty()) {
        ImGui::Separator();
        if (button(w, a.tr("恢复已归档文件", "Restore archived files")))
            a.defer([&a] { a.restore(); });
        hint(a.tr("原位置同名文件会保留，恢复文件自动添加编号。",
                  "Existing files are preserved. Restored files receive a unique name."));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
void todos_ui(Window &w) {
    auto &a = w.app;
    section(w, "待办", "To do");
    for (auto &todo : a.engine.state["todos"]) {
        auto ident = todo["id"].get<std::string>();
        ImGui::PushID(ident.c_str());
        bool done = todo.value("done", false);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(u(w, 2), u(w, 2)));
        if (ImGui::Checkbox("##done", &done))
            a.defer([&a, ident, done] {
                for (auto &t : a.engine.state["todos"])
                    if (t["id"] == ident)
                        t["done"] = done;
            });
        ImGui::PopStyleVar();
        ImGui::SameLine();
        float available = ImGui::GetContentRegionAvail().x - u(w, 28);
        auto p = ImGui::GetCursorScreenPos();
        font(w, 12);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + available);
        if (done)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(todo.value("text", "").c_str());
        if (done) {
            ImGui::GetWindowDrawList()->AddLine(
                {p.x, p.y + u(w, 7)}, {p.x + std::min(available, ImGui::GetItemRectSize().x), p.y + u(w, 7)},
                text_color(w, true));
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(u(w, 3), 0));
        if (button(w, "···", 24)) {
            w.rename_id = ident;
            w.rename_buffer = todo.value("text", "");
            w.search = todo.value("due", "");
            ImGui::OpenPopup("edit-task");
        }
        ImGui::PopStyleVar();
        if (!todo.value("due", "").empty()) {
            ImGui::Indent(u(w, 23));
            label(w, todo["due"], true, 10);
            ImGui::Unindent(u(w, 23));
        }
        if (!todo.value("link", "").empty()) {
            bool has_due = !todo.value("due", "").empty();
            if (has_due)
                ImGui::SameLine();
            else
                ImGui::Indent(u(w, 23));
            std::string path = todo["link"];
            font(w, 10);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(u(w, 3), 0));
            if (button(w, "↗ " + filename(wide(path))))
                a.defer([&a, path] {
                    for (auto &item : a.engine.state["items"])
                        if (path_equal(item["path"].get<std::string>(), path)) {
                            a.focus(item["id"]);
                            return;
                        }
                    a.open(path);
                });
            ImGui::PopStyleVar();
            ImGui::PopFont();
            if (!has_due)
                ImGui::Unindent(u(w, 23));
        }
        if (ImGui::BeginPopup("edit-task")) {
            ImGui::SetNextItemWidth(u(w, 230));
            ImGui::InputText(a.tr("内容", "Task").c_str(), &w.rename_buffer);
            ImGui::SetNextItemWidth(u(w, 160));
            ImGui::InputText("YYYY-MM-DD", &w.search);
            if (button(w, a.tr("保存", "Save"), 0, true)) {
                auto text = w.rename_buffer, due = w.search;
                a.defer([&a, ident, text, due] {
                    if (!due.empty()) {
                        std::tm date{};
                        std::istringstream input(due);
                        input >> std::get_time(&date, "%Y-%m-%d");
                        auto day = std::chrono::year_month_day{std::chrono::year(date.tm_year + 1900),
                                                               std::chrono::month(date.tm_mon + 1),
                                                               std::chrono::day(date.tm_mday)};
                        if (input.fail() || due.size() != 10 || !day.ok())
                            throw std::runtime_error(
                                a.tr("日期格式应为 YYYY-MM-DD", "Use YYYY-MM-DD for dates"));
                    }
                    for (auto &t : a.engine.state["todos"])
                        if (t["id"] == ident) {
                            t["text"] = text;
                            t["due"] = due;
                        }
                });
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::BeginMenu(a.tr("关联桌面文件", "Link desktop file").c_str())) {
                for (auto &i : a.engine.state["items"])
                    if (ImGui::MenuItem(i.value("name", "").c_str())) {
                        auto path = i["path"].get<std::string>();
                        a.defer([&a, ident, path] {
                            for (auto &t : a.engine.state["todos"])
                                if (t["id"] == ident)
                                    t["link"] = path;
                        });
                    }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(a.tr("取消文件关联", "Unlink file").c_str()))
                a.defer([&a, ident] {
                    for (auto &t : a.engine.state["todos"])
                        if (t["id"] == ident)
                            t["link"] = "";
                });
            if (ImGui::MenuItem(a.tr("删除待办", "Delete task").c_str()))
                a.defer([&a, ident] {
                    auto &tasks = a.engine.state["todos"];
                    tasks.erase(
                        std::remove_if(tasks.begin(), tasks.end(), [&](auto &t) { return t["id"] == ident; }),
                        tasks.end());
                });
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##new-task",
                                 a.tr("+ 新建待办，按 Enter 添加", "+ New task, Enter to add").c_str(),
                                 &w.draft, ImGuiInputTextFlags_EnterReturnsTrue) &&
        !w.draft.empty()) {
        auto text = w.draft;
        w.draft.clear();
        a.defer([&a, text] {
            a.engine.state["todos"].push_back(
                {{"id", id()}, {"text", text}, {"due", ""}, {"link", ""}, {"done", false}});
        });
    }
    record(w, "new-task");
}
void panel_ui(Window &w) {
    auto &a = w.app;
    auto p = ImGui::GetCursorScreenPos();
    logo(w, p, 26);
    ImGui::Dummy({u(w, 30), u(w, 27)});
    ImGui::SameLine();
    label(w, "DeskEdge", false, 15);
    ImGui::SameLine(ImGui::GetWindowWidth() - u(w, 82));
    bool pinned = a.engine.state["settings"].value("pinned", true);
    if (button(w, pinned ? "●" : "○", 26))
        a.defer([&a, pinned] { a.engine.state["settings"]["pinned"] = !pinned; });
    hint(a.tr("固定侧栏 / 贴边隐藏", "Pin panel / hide at edge"));
    ImGui::SameLine();
    if (button(w, "×", 25))
        w.hide();
    ImGui::Separator();
    if (w.focus_search) {
        ImGui::SetKeyboardFocusHere();
        w.focus_search = false;
    }
    ImGui::SetNextItemWidth(-1);
    const bool had_query = !a.query.empty();
    if (ImGui::InputTextWithHint("##desktop-search",
                                 a.tr("搜索桌面文件   Ctrl+Space", "Search desktop   Ctrl+Space").c_str(),
                                 &a.query)) {
        for (auto &z : a.zones)
            z->invalidate();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (had_query) {
            a.query.clear();
            for (auto &z : a.zones)
                z->invalidate();
        } else
            w.hide();
    }
    record(w, "desktop-search");
    float footer = u(w, 45);
    ImGui::BeginChild("panel-content", {0, -footer}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (!a.query.empty()) {
        auto matches = a.engine.search(a.query);
        section(w, "搜索结果", "Results");
        if (matches.empty())
            label(w, a.tr("没有找到匹配文件", "No matching files"), true);
        for (auto &m : matches) {
            auto ptr = a.engine.item(m.id);
            if (!ptr)
                continue;
            ImGui::PushID(m.id.c_str());
            if (ImGui::Selectable(ptr->value("name", "").c_str())) {
                auto key = m.id;
                a.defer([&a, key] { a.focus(key); });
            }
            if (auto z = a.engine.zone(ptr->value("zone", "")))
                label(w, a.engine.name(*z), true, 10);
            ImGui::PopID();
        }
        ImGui::Separator();
    }
    if (a.query.empty()) {
        tidy_card(w);
        if (button(w, a.tr("打开启动器", "Open launcher"), 0, true))
            a.defer([&a] { a.show_launcher(); });
        ImGui::SameLine();
        label(w, "Ctrl+Num0", true, 10);
        section(w, "常用应用", "Apps");
        int col = 0;
        for (auto &item : a.engine.state["favorites"]) {
            if (col % 6)
                ImGui::SameLine(0, u(w, 2));
            if (tile(w, item, true, 49, 72, false, false, true)) {
                auto path = item["path"].get<std::string>();
                a.defer([&a, path] { a.open(path); });
            }
            ++col;
        }
        section(w, "桌面区域", "Zones");
        for (auto &z : a.engine.state["zones"]) {
            auto ident = z["id"].get<std::string>();
            ImGui::PushID(ident.c_str());
            bool collapsed = z.value("collapsed", false);
            if (button(w, collapsed ? "›" : "⌄", 22))
                a.defer([&a, ident, collapsed] { (*a.engine.zone(ident))["collapsed"] = !collapsed; });
            ImGui::SameLine();
            auto pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled({pos.x + u(w, 4), pos.y + u(w, 11)}, u(w, 3),
                                                        color(w, z.value("color", 0x649CDA)));
            ImGui::Dummy({u(w, 9), u(w, 21)});
            ImGui::SameLine();
            if (w.rename_id == ident) {
                ImGui::SetNextItemWidth(u(w, 145));
                if (ImGui::InputText("##name", &w.rename_buffer, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    auto name = w.rename_buffer;
                    w.rename_id.clear();
                    a.defer([&a, ident, name] {
                        auto ptr = a.engine.zone(ident);
                        (*ptr)["name"] = name;
                        (*ptr)["en"] = name;
                    });
                }
            } else {
                if (button(w, a.engine.name(z))) {
                    for (auto &window : a.zones)
                        if (window->zone_id == ident) {
                            window->show();
                            window->invalidate();
                        }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    w.rename_id = ident;
                    w.rename_buffer = a.engine.name(z);
                }
            }
            size_t count = 0;
            for (auto &i : a.engine.state["items"])
                if (i["zone"] == ident)
                    ++count;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - u(w, 60));
            label(w, std::to_string(count), true, 11);
            ImGui::SameLine();
            if (button(w, "×", 22))
                a.defer([&a, ident] { a.engine.remove_zone(ident); });
            hint(a.tr("移除区域，文件会归入其他区域", "Remove zone; files are regrouped"));
            ImGui::PopID();
        }
        if (button(w, a.tr("+ 绘制新区域", "+ Draw new zone")))
            a.defer([&a] { a.begin_draw(); });
        ImGui::SameLine();
        if (button(w, a.peek ? a.tr("显示区域", "Show zones") : a.tr("查看壁纸", "Peek wallpaper")))
            a.defer([&a] { a.toggle_peek(); });
        todos_ui(w);
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (button(w, a.tr("中文", "EN")))
        a.defer([&a] {
            auto &l = a.engine.state["settings"]["language"];
            l = l == "zh" ? "en" : "zh";
        });
    ImGui::SameLine();
    auto theme = a.engine.state["settings"].value("theme", "auto");
    if (button(w, theme == "auto"   ? a.tr("◐ 自动", "◐ Auto")
                  : theme == "dark" ? a.tr("◑ 深色", "◑ Dark")
                                    : a.tr("◒ 浅色", "◒ Light")))
        a.defer([&a, theme] {
            a.engine.state["settings"]["theme"] = theme == "auto"    ? "light"
                                                  : theme == "light" ? "dark"
                                                                     : "auto";
        });
    ImGui::SameLine(ImGui::GetWindowWidth() - u(w, 53));
    if (button(w, a.tr("设置", "Settings")))
        a.defer([&a] { a.show_settings(); });
    if (!a.toast.empty()) {
        auto draw = ImGui::GetForegroundDrawList();
        font(w, 12);
        auto size = ImGui::CalcTextSize(a.toast.c_str(), nullptr, false, w.width - u(w, 44));
        ImVec2 p2{u(w, 12), w.height - u(w, 66) - size.y};
        draw->AddRectFilled(p2, {w.width - u(w, 12), p2.y + size.y + u(w, 20)},
                            color(w, a.dark ? 0x3B4652 : 0xE8F2FD), u(w, 7));
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {p2.x + u(w, 10), p2.y + u(w, 10)},
                      text_color(w), a.toast.c_str(), nullptr, w.width - u(w, 44));
        ImGui::PopFont();
    }
}
void launcher_ui(Window &w) {
    auto &a = w.app;
    float left = u(w, 130);
    ImGui::BeginChild("tabs", {left, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    auto p = ImGui::GetCursorScreenPos();
    logo(w, p, 23);
    ImGui::Dummy({u(w, 25), u(w, 25)});
    ImGui::SameLine();
    label(w, a.tr("启动器", "Launcher"), false, 13);
    ImGui::Separator();
    for (auto &tab : a.engine.state["tabs"]) {
        auto ident = tab["id"].get<std::string>();
        ImGui::PushID(ident.c_str());
        auto pos = ImGui::GetCursorScreenPos();
        hotspot(w, pos, {left - u(w, 4), u(w, 33)}, ident);
        if (w.rename_id == ident) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##rename-tab", &w.rename_buffer, ImGuiInputTextFlags_EnterReturnsTrue)) {
                auto name = w.rename_buffer;
                w.rename_id.clear();
                a.defer([&a, ident, name] {
                    auto ptr = a.engine.tab(ident);
                    (*ptr)["name"] = name;
                    (*ptr)["en"] = name;
                });
            }
        } else {
            font(w, 12);
            if (ImGui::Selectable((a.engine.name(tab) + "   " + std::to_string(tab["items"].size())).c_str(),
                                  a.active_tab == ident, 0, {0, u(w, 28)})) {
                a.active_tab = ident;
                w.search.clear();
                w.selected_index = 0;
            }
            ImGui::PopFont();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                w.rename_id = ident;
                w.rename_buffer = a.engine.name(tab);
            }
            if (ImGui::BeginPopupContextItem("tab-menu")) {
                if (ImGui::MenuItem(a.tr("重命名", "Rename").c_str())) {
                    w.rename_id = ident;
                    w.rename_buffer = a.engine.name(tab);
                }
                if (a.engine.state["tabs"].size() > 1 &&
                    ImGui::MenuItem(a.tr("移除标签页", "Remove tab").c_str()))
                    a.defer([&a, ident] {
                        auto &tabs = a.engine.state["tabs"];
                        tabs.erase(std::remove_if(tabs.begin(), tabs.end(),
                                                  [&](auto &t) { return t["id"] == ident; }),
                                   tabs.end());
                    });
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    if (button(w, a.tr("+ 新建标签页", "+ New tab")))
        a.defer([&a, &w] {
            auto ident = id();
            a.engine.state["tabs"].push_back({{"id", ident},
                                              {"name", a.tr("新标签页", "New tab")},
                                              {"en", a.tr("新标签页", "New tab")},
                                              {"sort", "manual"},
                                              {"items", Json::array()}});
            a.active_tab = ident;
            w.rename_id = ident;
            w.rename_buffer = a.tr("新标签页", "New tab");
        });
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - u(w, 61)));
    label(w, "Ctrl+Num0", true, 10);
    label(w, "Tab · Enter · Esc", true, 10);
    ImGui::EndChild();
    ImGui::SameLine(0, u(w, 11));
    ImGui::BeginChild("launcher-main", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (w.focus_search) {
        ImGui::SetKeyboardFocusHere();
        w.focus_search = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - u(w, 30));
    if (ImGui::InputTextWithHint("##launch-search", a.tr("搜索全部标签页…", "Search all tabs…").c_str(),
                                 &w.search))
        w.selected_index = 0;
    record(w, "launcher-search");
    ImGui::SameLine();
    if (button(w, "×", 24))
        w.hide();
    auto t = a.engine.tab(a.active_tab);
    if (!t) {
        ImGui::EndChild();
        return;
    }
    font(w, 10);
    std::vector<std::tuple<std::string, const char *, const char *>> sorts = {{"manual", "手动", "Manual"},
                                                                              {"name", "名称", "Name"},
                                                                              {"used", "常用", "Used"},
                                                                              {"recent", "最近", "Recent"},
                                                                              {"type", "类型", "Type"}};
    int n = 0;
    for (auto &[key, zh, en] : sorts) {
        if (n++)
            ImGui::SameLine(0, u(w, 1));
        bool selected = t->value("sort", "manual") == key;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.2f, .5f, .8f, .15f));
        if (button(w, a.tr(zh, en)))
            a.defer([&a, key] { (*a.engine.tab(a.active_tab))["sort"] = key; });
        if (selected)
            ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    ImGui::Separator();
    auto matches = a.engine.search(w.search, true, a.active_tab);
    int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / u(w, 78)));
    if (!matches.empty())
        w.selected_index = std::clamp(w.selected_index, 0, static_cast<int>(matches.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        w.selected_index = std::min(static_cast<int>(matches.size()) - 1, w.selected_index + columns);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        w.selected_index = std::max(0, w.selected_index - columns);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !matches.empty()) {
        auto ptr = a.engine.launch(matches[w.selected_index].id);
        if (ptr) {
            auto path = ptr->value("path", "");
            a.defer([&a, path] { a.open(path); });
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        w.hide();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !w.popup_open) {
        int index = 0;
        for (size_t i = 0; i < a.engine.state["tabs"].size(); i++)
            if (a.engine.state["tabs"][i]["id"] == a.active_tab)
                index = static_cast<int>(i);
        int count = static_cast<int>(a.engine.state["tabs"].size());
        index = (index + (ImGui::GetIO().KeyShift ? -1 : 1) + count) % count;
        a.active_tab = a.engine.state["tabs"][index]["id"];
        w.search.clear();
        w.selected_index = 0;
        w.invalidate();
    }
    ImGui::BeginChild("launch-grid", {0, -u(w, 35)}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    int rows = (static_cast<int>(matches.size()) + columns - 1) / columns;
    ImGuiListClipper clipper;
    clipper.Begin(rows, u(w, 94));
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            for (int col = 0; col < columns; col++) {
                int index = row * columns + col;
                if (index >= static_cast<int>(matches.size()))
                    break;
                if (col)
                    ImGui::SameLine(0, 0);
                auto m = matches[index];
                auto ptr = a.engine.launch(m.id);
                if (!ptr)
                    continue;
                hotspot(w, ImGui::GetCursorScreenPos(), {u(w, 78), u(w, 94)}, m.tab, m.id);
                if (tile(w, *ptr, true, 78, 94, false, index == w.selected_index)) {
                    auto path = ptr->value("path", "");
                    a.defer([&a, path] { a.open(path); });
                }
            }
    if (matches.empty())
        label(w,
              w.search.empty()
                  ? a.tr("拖入应用或文件，建立你的启动页。", "Drop apps or files to build your launch page.")
                  : a.tr("没有找到匹配项", "No matches"),
              true, 11);
    ImGui::EndChild();
    if (button(w, a.tr("+ 添加", "+ Add")))
        a.defer([&a] { a.add_files("", a.active_tab); });
    ImGui::SameLine();
    if (button(w, a.tr("文件夹", "Folder")))
        a.defer([&a] { a.add_files("", a.active_tab, true); });
    ImGui::SameLine();
    label(w, std::to_string(matches.size()) + a.tr(" 项", " items"), true, 10);
    ImGui::EndChild();
}
void settings_ui(Window &w) {
    auto &a = w.app;
    label(w, a.tr("DeskEdge 设置", "DeskEdge settings"), false, 17);
    ImGui::SameLine(ImGui::GetWindowWidth() - u(w, 42));
    if (button(w, "×", 24))
        w.hide();
    ImGui::Separator();
    auto &s = a.engine.state["settings"];
    int opacity = s.value("glass_opacity", 55);
    bool card_surface = s.value("card_style", true);
    ImGui::BeginDisabled(card_surface);
    ImGui::SetNextItemWidth(u(w, 165));
    if (ImGui::SliderInt(a.tr("玻璃浓度", "Glass tint").c_str(), &opacity, 20, 90, "%d%%")) {
        s["glass_opacity"] = opacity;
        for (auto &zone : a.zones)
            zone->invalidate();
        if (a.panel)
            a.panel->invalidate();
        if (a.launcher)
            a.launcher->invalidate();
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        a.defer([] {});
    ImGui::EndDisabled();
    label(w,
          card_surface
              ? a.tr("关闭卡片材质后，可调整玻璃浓度。", "Turn off card surfaces to adjust glass tint.")
              : a.tr("降低浓度可透出更多背景，文字和图标保持清晰。",
                     "Lower tint reveals more background; text stays opaque."),
          true, 11);
    int days = s.value("stale_days", 7);
    ImGui::SetNextItemWidth(u(w, 130));
    if (ImGui::SliderInt(a.tr("整理提醒天数", "Tidy age in days").c_str(), &days, 1, 60))
        a.defer([&a, days] { a.engine.state["settings"]["stale_days"] = days; });
    label(
        w,
        a.tr("只整理临时区域的文件，归档后可以恢复。", "Temporary zone files can be archived and restored."),
        true, 11);
    for (auto [key, zh, en] : std::vector<std::tuple<std::string, const char *, const char *>>{
             {"pinned", "固定右侧边栏", "Keep panel pinned"},
             {"grid_mode", "图标对齐网格，保留空位", "Align icons to grid; keep empty cells"},
             {"window_grid", "区域窗口吸附网格", "Snap regions to the desktop grid"},
             {"card_style", "简洁卡片材质", "Clean card surfaces"},
             {"animations", "柔和的开合动画", "Animate opening and closing"},
             {"replace_icons", "用桌面区域显示原有桌面文件", "Display desktop files in zones"},
             {"middle_click", "鼠标中键呼出启动器", "Open launcher with middle mouse button"},
             {"startup", "登录 Windows 时启动", "Start when signing in to Windows"}}) {
        bool value = s.value(key, false);
        if (ImGui::Checkbox(a.tr(zh, en).c_str(), &value))
            a.defer([&a, key, value] {
                if (key == "startup")
                    startup(value);
                a.engine.state["settings"][key] = value;
                if (key == "window_grid" && value)
                    a.engine.state["settings"]["window_grid_migrated"] = false;
            });
    }
    auto list = monitors();
    std::string current = s.value("monitor", "");
    std::string display = current.empty() ? a.tr("主显示器", "Primary monitor") : current;
    if (ImGui::BeginCombo(a.tr("侧栏显示器", "Panel monitor").c_str(), display.c_str())) {
        if (ImGui::Selectable(a.tr("主显示器", "Primary monitor").c_str(), current.empty()))
            a.defer([&a] { a.engine.state["settings"]["monitor"] = ""; });
        for (auto &m : list)
            if (ImGui::Selectable(m.name.c_str(), m.name == current)) {
                auto name = m.name;
                a.defer([&a, name] { a.engine.state["settings"]["monitor"] = name; });
            }
        ImGui::EndCombo();
    }
    if (button(w, a.tr("打开数据目录", "Open data folder")))
        a.defer([&a] { shell_open(pathstr(a.engine.data_dir)); });
    ImGui::SameLine();
    if (button(w, a.tr("打开归档目录", "Open archive folder")))
        a.defer([&a] {
            fs::create_directories(a.archive_root);
            shell_open(pathstr(a.archive_root));
        });
    if (button(w, a.tr("刷新文件与图标", "Refresh files and icons")))
        a.defer([&a] {
            a.engine.sync(a.roots, a.archive_root);
            a.icons->clear();
        });
    ImGui::Separator();
    label(w, "DeskEdge · 桌沿  1.3.0", false, 13);
    label(w, a.tr("为专注研究的桌面留出秩序。", "A calmer desktop for focused work."), true, 11);
    label(w, "C++20 · Win32 · DirectX 11", true, 10);
    label(w,
          a.tr("双击区域标题重命名，拖动标题移动区域。",
               "Double-click a zone title to rename; drag it to move."),
          true, 11);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        w.hide();
}
void region_preview(Window &w, RegionRect rect, bool blocked) {
    auto draw = ImGui::GetWindowDrawList();
    int accent = blocked ? 0xC95252 : 0x488BDD;
    float step = u(w, WINDOW_GRID);
    ImVec2 start{u(w, rect.x), u(w, rect.y)}, end{u(w, rect.x + rect.w), u(w, rect.y + rect.h)};
    float left = std::max(0.f, start.x - step * 2), top = std::max(0.f, start.y - step * 2);
    for (float y = std::floor(top / step) * step; y <= std::min(float(w.height), end.y + step * 2); y += step)
        for (float x = std::floor(left / step) * step; x <= std::min(float(w.width), end.x + step * 2);
             x += step)
            draw->AddCircleFilled({x, y}, 1.f, color(w, accent, 55), 4);
    for (float x : {start.x, end.x})
        draw->AddLine({x, 0}, {x, float(w.height)}, color(w, accent, 38));
    for (float y : {start.y, end.y})
        draw->AddLine({0, y}, {float(w.width), y}, color(w, accent, 38));
    draw->AddRectFilled(start, end, color(w, accent, 12), u(w, 12));
    draw->AddRect(start, end, color(w, accent, 185), u(w, 12), 0, u(w, 1.5f));
    font(w, 11);
    std::string caption = blocked
                              ? w.app.tr("位置已占用 · 请选择空位", "Space occupied · choose an empty spot")
                              : std::to_string(int(rect.w)) + " × " + std::to_string(int(rect.h)) + "  ·  " +
                                    std::to_string(int(rect.w / WINDOW_GRID)) + " × " +
                                    std::to_string(int(rect.h / WINDOW_GRID)) + w.app.tr(" 格", " cells");
    auto size = ImGui::CalcTextSize(caption.c_str());
    ImVec2 label_at{std::clamp(start.x, u(w, 12), std::max(u(w, 12), w.width - size.x - u(w, 28))),
                    std::min(end.y + u(w, 12), w.height - size.y - u(w, 24))};
    draw->AddRectFilled({label_at.x - u(w, 10), label_at.y - u(w, 6)},
                        {label_at.x + size.x + u(w, 10), label_at.y + size.y + u(w, 6)},
                        color(w, w.app.dark ? 0x252A32 : 0xF9FAFC, 250), u(w, 7));
    draw->AddText(label_at, color(w, accent), caption.c_str());
    ImGui::PopFont();
}
void draw_zone(Window &w) {
    auto &a = w.app;
    auto d = ImGui::GetWindowDrawList();
    d->AddRectFilled({0, 0}, {static_cast<float>(w.width), static_cast<float>(w.height)},
                     color(w, 0x20364F, 70));
    font(w, 16);
    auto text = a.tr("拖动鼠标绘制新区域 · Esc 取消", "Drag to draw a new zone · Esc to cancel");
    auto size = ImGui::CalcTextSize(text.c_str());
    d->AddRectFilled({w.width / 2 - size.x / 2 - u(w, 20), u(w, 25)},
                     {w.width / 2 + size.x / 2 + u(w, 20), u(w, 71)}, color(w, 0xF3F7FC, 245), u(w, 8));
    d->AddText({w.width / 2 - size.x / 2, u(w, 38)}, color(w, 0x24262B), text.c_str());
    ImGui::PopFont();
    auto &io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(0)) {
        w.moving = true;
        w.initial_x = std::clamp(io.MousePos.x, 0.f, static_cast<float>(w.width));
        w.initial_y = std::clamp(io.MousePos.y, 0.f, static_cast<float>(w.height));
    }
    if (w.moving) {
        float x = std::min(w.initial_x, io.MousePos.x), y = std::min(w.initial_y, io.MousePos.y),
              width = std::abs(io.MousePos.x - w.initial_x), height = std::abs(io.MousePos.y - w.initial_y);
        bool aligned = a.engine.state["settings"].value("window_grid", true);
        RegionRect draft{x / w.scale, y / w.scale, width / w.scale, height / w.scale};
        if (aligned)
            draft = snap_region(draft, w.screen.width(), w.screen.height());
        bool blocked = aligned && !a.region_available(nullptr, draft, w.screen);
        region_preview(w, draft, blocked);
        if (ImGui::IsMouseReleased(0)) {
            w.moving = false;
            if (width >= u(w, 140) && height >= u(w, 80) && !blocked) {
                auto screen = w.screen;
                a.defer([&a, draft, screen] {
                    auto ident = a.engine.add_zone(draft.x, draft.y, draft.w, draft.h, screen.name);
                    a.cancel_draw();
                    for (auto &zone : a.zones)
                        if (zone->zone_id == ident) {
                            zone->rename_id = ident;
                            zone->rename_buffer = a.tr("新区域", "New zone");
                            zone->focus_search = true;
                            zone->show(true);
                        }
                });
            }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        a.defer([&a] { a.cancel_draw(); });
}
} // namespace
void draw_ui(Window &w) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({static_cast<float>(w.width), static_cast<float>(w.height)});
    bool transparent = w.kind == Kind::Zone || w.kind == Kind::Draw || w.kind == Kind::Guides;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, transparent ? ImVec2(0, 0) : ImVec2(u(w, 12), u(w, 10)));
    ImGui::Begin("DeskEdge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    if (w.kind != Kind::Draw && w.kind != Kind::Guides) {
        auto draw = ImGui::GetWindowDrawList();
        ImVec2 end{static_cast<float>(w.width), static_cast<float>(w.height)};
        float radius = u(w, 12);
        bool card = w.app.engine.state["settings"].value("card_style", true);
        if (!card && w.kind == Kind::Zone && w.app.glass) {
            if (auto texture = w.app.glass->wallpaper(w.screen)) {
                RECT bounds{};
                GetWindowRect(w.hwnd, &bounds);
                float sw = static_cast<float>(w.screen.bounds.right - w.screen.bounds.left);
                float sh = static_cast<float>(w.screen.bounds.bottom - w.screen.bounds.top);
                ImVec2 uv0{(bounds.left - w.screen.bounds.left) / sw,
                           (bounds.top - w.screen.bounds.top) / sh};
                ImVec2 uv1{(bounds.right - w.screen.bounds.left) / sw,
                           (bounds.bottom - w.screen.bounds.top) / sh};
                draw->AddImageRounded((ImTextureID)(intptr_t)texture, {0, 0}, end, uv0, uv1, IM_COL32_WHITE,
                                      radius);
            }
        }
        int tint = std::clamp(w.app.engine.state["settings"].value("glass_opacity", 55), 20, 90);
        // Only the material receives opacity; glyphs, icons and focus indicators remain fully opaque.
        int alpha =
            card ? 247 : static_cast<int>(255 * (w.app.dark ? .18f + tint * .008f : .12f + tint * .008f));
        draw->AddRectFilled({0, 0}, end, color(w, w.app.dark ? 0x20242C : 0xF7F9FC, alpha), radius);
        if (!card && w.app.glass) {
            if (auto grain = w.app.glass->grain()) {
                draw->PushClipRect({radius, radius}, {end.x - radius, end.y - radius}, true);
                for (float y = 0; y < end.y; y += 128)
                    for (float x = 0; x < end.x; x += 128)
                        draw->AddImage((ImTextureID)(intptr_t)grain, {x, y}, {x + 128, y + 128});
                draw->PopClipRect();
            }
        }
        draw->AddRect({.5f, .5f}, {end.x - .5f, end.y - .5f},
                      color(w, w.app.dark ? 0xBAC8DA : 0x697586, card ? 52 : 70), radius);
    }
    switch (w.kind) {
    case Kind::Panel:
        panel_ui(w);
        break;
    case Kind::Launcher:
        launcher_ui(w);
        break;
    case Kind::Zone:
        zone_ui(w);
        break;
    case Kind::Settings:
        settings_ui(w);
        break;
    case Kind::Draw:
        draw_zone(w);
        break;
    case Kind::Guides:
        region_preview(w, w.guide_bounds, w.placement_blocked);
        break;
    }
    w.popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (w.drop_hover) {
        auto d = ImGui::GetForegroundDrawList();
        d->AddRect({u(w, 2), u(w, 2)}, {w.width - u(w, 2), w.height - u(w, 2)}, color(w, 0x488BDD, 130),
                   u(w, 11), 0, u(w, 1));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
} // namespace deskedge
