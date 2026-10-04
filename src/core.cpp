#include "core.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <shlwapi.h>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace deskedge {
std::wstring wide(std::string_view s) {
    if (s.empty())
        return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}
std::string utf8(std::wstring_view s) {
    if (s.empty())
        return {};
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::string lower(std::string_view s) {
    auto w = wide(s);
    if (w.empty())
        return {};
    auto r = w;
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), static_cast<int>(w.size()), r.data(),
                  static_cast<int>(r.size()), nullptr, nullptr, 0);
    return utf8(r);
}
std::string id() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g)))
        throw std::runtime_error("Cannot create identifier");
    wchar_t s[40]{};
    StringFromGUID2(g, s, 40);
    return utf8(s);
}
int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string pathstr(const fs::path &p) {
    return utf8(p.wstring());
}
std::string filename(const fs::path &p) {
    return utf8(lower(pathstr(p.extension())) == ".lnk" ? p.stem().wstring() : p.filename().wstring());
}
bool path_equal(std::string_view a, std::string_view b) {
    auto aw = wide(a), bw = wide(b);
    return CompareStringOrdinal(aw.data(), static_cast<int>(aw.size()), bw.data(),
                                static_cast<int>(bw.size()), TRUE) == CSTR_EQUAL;
}
int64_t file_time(const fs::path &p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return now();
    ULARGE_INTEGER u{};
    u.LowPart = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}
std::string win_error(DWORD code) {
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    std::string s = message ? utf8(message) : ("Windows error " + std::to_string(code));
    if (message)
        LocalFree(message);
    return s;
}
static Json read_json(const fs::path &p) {
    std::ifstream file(p, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read saved state");
    auto s = Json::parse(file);
    if (s.value("version", 0) != 1)
        throw std::runtime_error("Unsupported state version");
    auto invalid = []() { throw std::runtime_error("Saved state has an invalid schema"); };
    for (auto key : {"zones", "items", "tabs", "todos", "favorites", "archive", "moves"})
        if (s.contains(key) && !s[key].is_array())
            invalid();
    auto strings = [&](const Json &object, std::initializer_list<const char *> keys) {
        if (!object.is_object())
            invalid();
        for (auto key : keys)
            if (!object.contains(key) || !object[key].is_string())
                invalid();
    };
    if (s.contains("settings")) {
        if (!s["settings"].is_object())
            invalid();
        auto expected = Engine::defaults()["settings"];
        for (auto it = expected.begin(); it != expected.end(); ++it)
            if (s["settings"].contains(it.key())) {
                auto &value = s["settings"][it.key()];
                if (it->is_boolean() && !value.is_boolean())
                    invalid();
                if (it->is_string() && !value.is_string())
                    invalid();
                if (it->is_number_integer() && !value.is_number_integer())
                    invalid();
            }
    }
    if (s.contains("zones"))
        for (auto &z : s["zones"]) {
            strings(z, {"id", "name"});
            for (auto key : {"x", "y", "w", "h", "color"})
                if (z.contains(key) && !z[key].is_number())
                    invalid();
            for (auto key : {"collapsed", "temporary"})
                if (z.contains(key) && !z[key].is_boolean())
                    invalid();
        }
    if (s.contains("items"))
        for (auto &item : s["items"])
            strings(item, {"id", "name", "path", "zone"});
    if (s.contains("favorites"))
        for (auto &item : s["favorites"])
            strings(item, {"id", "name", "path"});
    if (s.contains("todos"))
        for (auto &todo : s["todos"]) {
            strings(todo, {"id", "text"});
            if (todo.contains("done") && !todo["done"].is_boolean())
                invalid();
        }
    if (s.contains("tabs"))
        for (auto &tab : s["tabs"]) {
            strings(tab, {"id", "name"});
            if (!tab.contains("items") || !tab["items"].is_array())
                invalid();
            for (auto &item : tab["items"])
                strings(item, {"id", "name", "path"});
        }
    if (s.contains("archive"))
        for (auto &entry : s["archive"]) {
            strings(entry, {"id", "path", "original"});
            if (!entry.contains("item"))
                invalid();
            strings(entry["item"], {"id", "name", "path", "zone"});
        }
    if (s.contains("moves"))
        for (auto &move : s["moves"]) {
            strings(move, {"id", "op", "source", "destination"});
            if (move["op"] == "archive")
                strings(move, {"item"});
            else if (move["op"] == "restore")
                strings(move, {"archive"});
            else
                invalid();
        }
    return s;
}
static bool exists(std::string_view p) {
    std::error_code ec;
    return fs::exists(wide(p), ec);
}
Json Engine::defaults() {
    return {{"version", 1},
            {"settings",
             {{"language", "zh"},
              {"theme", "light"},
              {"glass_opacity", 55},
              {"animations", true},
              {"grid_mode", true},
              {"window_grid", true},
              {"card_style", true},
              {"pinned", true},
              {"stale_days", 7},
              {"monitor", ""},
              {"replace_icons", true},
              {"startup", false},
              {"middle_click", false}}},
            {"zones", Json::array({{{"id", "projects"},
                                    {"name", "项目"},
                                    {"en", "Projects"},
                                    {"x", 24},
                                    {"y", 24},
                                    {"w", 316},
                                    {"h", 196},
                                    {"color", 0x649CDA},
                                    {"collapsed", false},
                                    {"temporary", false},
                                    {"monitor", ""}},
                                   {{"id", "papers"},
                                    {"name", "文献"},
                                    {"en", "Papers"},
                                    {"x", 24},
                                    {"y", 236},
                                    {"w", 316},
                                    {"h", 196},
                                    {"color", 0xB38CDB},
                                    {"collapsed", false},
                                    {"temporary", false},
                                    {"monitor", ""}},
                                   {{"id", "data"},
                                    {"name", "数据与结果"},
                                    {"en", "Datasets & results"},
                                    {"x", 24},
                                    {"y", 448},
                                    {"w", 316},
                                    {"h", 196},
                                    {"color", 0x65B395},
                                    {"collapsed", false},
                                    {"temporary", false},
                                    {"monitor", ""}},
                                   {{"id", "temp"},
                                    {"name", "临时"},
                                    {"en", "Temp"},
                                    {"x", 356},
                                    {"y", 24},
                                    {"w", 316},
                                    {"h", 214},
                                    {"color", 0xD7A164},
                                    {"collapsed", false},
                                    {"temporary", true},
                                    {"monitor", ""}}})},
            {"tabs", Json::array({{{"id", "software"},
                                   {"name", "实验软件"},
                                   {"en", "Lab software"},
                                   {"sort", "used"},
                                   {"items", Json::array()}},
                                  {{"id", "current"},
                                   {"name", "当前课题"},
                                   {"en", "Current project"},
                                   {"sort", "manual"},
                                   {"items", Json::array()}},
                                  {{"id", "reading"},
                                   {"name", "文献"},
                                   {"en", "Papers"},
                                   {"sort", "recent"},
                                   {"items", Json::array()}},
                                  {{"id", "tools"},
                                   {"name", "工具"},
                                   {"en", "Tools"},
                                   {"sort", "name"},
                                   {"items", Json::array()}}})},
            {"items", Json::array()},
            {"todos", Json::array()},
            {"favorites", Json::array()},
            {"archive", Json::array()},
            {"moves", Json::array()},
            {"snooze_until", 0},
            {"last_review", 0}};
}
Engine::Engine(fs::path directory) : data_dir(std::move(directory)) {
    fs::create_directories(data_dir);
    auto p = data_dir / L"state.json";
    if (fs::exists(p)) {
        try {
            state = read_json(p);
        } catch (...) {
            auto backup = data_dir / L"state.json.bak";
            if (!fs::exists(backup))
                throw;
            state = read_json(backup);
            fs::copy_file(p, data_dir / (L"state.damaged-" + std::to_wstring(now()) + L".json"),
                          fs::copy_options::overwrite_existing);
            fs::copy_file(backup, p, fs::copy_options::overwrite_existing);
            recovery_message = "Recovered the previous saved state.";
        }
    } else
        state = defaults();
    auto standard = defaults();
    for (auto it = standard.begin(); it != standard.end(); ++it)
        if (!state.contains(it.key()))
            state[it.key()] = it.value();
    for (auto it = standard["settings"].begin(); it != standard["settings"].end(); ++it)
        if (!state["settings"].contains(it.key()))
            state["settings"][it.key()] = it.value();
    state["settings"]["stale_days"] = std::clamp(state["settings"]["stale_days"].get<int>(), 1, 60);
    state["settings"]["glass_opacity"] = std::clamp(state["settings"]["glass_opacity"].get<int>(), 20, 90);
    if (state["tabs"].empty())
        state["tabs"].push_back(standard["tabs"][0]);
    for (auto &z : state["zones"]) {
        z["w"] = std::clamp(z.value("w", 316.f), 140.f, 10000.f);
        z["h"] = std::clamp(z.value("h", 196.f), 80.f, 10000.f);
    }
    recover_moves();
    ensure_zone_grid();
    ensure_grid();
}
void Engine::save() {
    ensure_zone_grid();
    ensure_grid();
    invalidate_search();
    auto text = state.dump(2);
    auto temp = data_dir / L"state.tmp", dest = data_dir / L"state.json",
         backup = data_dir / L"state.json.bak";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error(win_error());
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size() && FlushFileBuffers(h);
    auto error = GetLastError();
    CloseHandle(h);
    if (!ok)
        throw std::runtime_error(win_error(error));
    if (fs::exists(dest) && !CopyFileW(dest.c_str(), backup.c_str(), FALSE))
        throw std::runtime_error(win_error());
    if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(win_error());
}
Json *Engine::zone(std::string_view s) {
    for (auto &z : state["zones"])
        if (z["id"].get_ref<const std::string &>() == s)
            return &z;
    return nullptr;
}
Json *Engine::item(std::string_view s) {
    for (auto &i : state["items"])
        if (i["id"].get_ref<const std::string &>() == s)
            return &i;
    return nullptr;
}
Json *Engine::tab(std::string_view s) {
    for (auto &t : state["tabs"])
        if (t["id"].get_ref<const std::string &>() == s)
            return &t;
    return nullptr;
}
Json *Engine::launch(std::string_view s) {
    for (auto &t : state["tabs"])
        for (auto &i : t["items"])
            if (i["id"].get_ref<const std::string &>() == s)
                return &i;
    return nullptr;
}
std::string Engine::name(const Json &j) const {
    return state["settings"].value("language", "zh") == "en" ? j.value("en", j.value("name", ""))
                                                             : j.value("name", "");
}
std::string Engine::ensure_zone() {
    if (!state["zones"].empty())
        return state["zones"][0]["id"];
    return add_zone(24, 24, 316, 196, "");
}
std::string Engine::add_zone(float x, float y, float w, float h, std::string monitor) {
    auto s = id();
    static const int colors[] = {0x659DD6, 0xD99178, 0x8CBA8A, 0xB38BCE, 0x65AFBC};
    state["zones"].push_back({{"id", s},
                              {"name", "新区域"},
                              {"en", "New zone"},
                              {"x", x},
                              {"y", y},
                              {"w", std::max(w, 140.f)},
                              {"h", std::max(h, 80.f)},
                              {"color", colors[state["zones"].size() % 5]},
                              {"collapsed", false},
                              {"temporary", false},
                              {"monitor", monitor}});
    return s;
}
void Engine::remove_zone(std::string s) {
    invalidate_search();
    auto &zs = state["zones"];
    zs.erase(std::remove_if(zs.begin(), zs.end(), [&](auto &z) { return z["id"] == s; }), zs.end());
    std::vector<std::string> orphan;
    for (auto &i : state["items"])
        if (i["zone"] == s)
            orphan.push_back(i["id"]);
    if (!orphan.empty()) {
        auto target = ensure_zone();
        for (auto &ident : orphan)
            move_zone(ident, target);
    }
}
std::string Engine::add_file(const fs::path &original, std::string target, bool desktop) {
    invalidate_search();
    std::error_code ec;
    auto path = fs::absolute(original, ec).lexically_normal();
    if (ec || !fs::exists(path, ec) || ec)
        return "";
    auto text = pathstr(path);
    for (auto &i : state["items"])
        if (path_equal(i["path"].get<std::string>(), text)) {
            if (zone(target) && i["zone"] != target) {
                i["zone"] = target;
                i.erase("grid");
            }
            i["desktop"] = desktop || i.value("desktop", false);
            i["written"] = file_time(path);
            return i["id"];
        }
    const auto ext = lower(pathstr(path.extension()));
    bool directory = fs::is_directory(path, ec);
    std::string category = "temp";
    if (directory || ext == ".lnk" || ext == ".exe" || ext == ".url")
        category = "projects";
    else if (ext == ".pdf" || ext == ".epub" || ext == ".docx" || ext == ".doc")
        category = "papers";
    else if (ext == ".csv" || ext == ".xlsx" || ext == ".xls" || ext == ".mat" || ext == ".py" ||
             ext == ".r" || ext == ".ipynb" || ext == ".json")
        category = "data";
    if (!zone(target))
        target = zone(category) ? category : ensure_zone();
    auto ident = id();
    state["items"].push_back({{"id", ident},
                              {"path", text},
                              {"name", filename(path)},
                              {"zone", target},
                              {"folder", directory},
                              {"desktop", desktop},
                              {"written", file_time(path)},
                              {"used", 0},
                              {"kept", 0}});
    return ident;
}
void Engine::sync(const std::vector<fs::path> &roots, const fs::path &archive_root) {
    for (auto &root : roots) {
        std::error_code ec;
        fs::directory_iterator iter(root, fs::directory_options::skip_permission_denied, ec);
        if (ec)
            continue;
        for (auto &entry : iter) {
            auto p = entry.path();
            auto attrs = GetFileAttributesW(p.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES ||
                (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
                path_equal(pathstr(p), pathstr(archive_root)))
                continue;
            add_file(p, "", true);
        }
    }
    auto &items = state["items"];
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](auto &i) {
                                   return i.value("desktop", false) &&
                                          !exists(i["path"].template get<std::string>());
                               }),
                items.end());
}
void Engine::repoint(std::string old_path, std::string new_path) {
    for (auto &t : state["tabs"])
        for (auto &i : t["items"])
            if (path_equal(i["path"].get<std::string>(), old_path))
                i["path"] = new_path;
    for (auto &i : state["favorites"])
        if (path_equal(i["path"].get<std::string>(), old_path))
            i["path"] = new_path;
    for (auto &t : state["todos"])
        if (path_equal(t.value("link", ""), old_path))
            t["link"] = new_path;
}
void Engine::rename_path(const fs::path &from, const fs::path &to) {
    invalidate_search();
    auto a = pathstr(from), b = pathstr(to);
    for (auto &i : state["items"])
        if (path_equal(i["path"].get<std::string>(), a)) {
            i["path"] = b;
            i["name"] = filename(to);
        }
    repoint(a, b);
}
void Engine::move_zone(std::string i, std::string z) {
    invalidate_search();
    if (auto ptr = item(i); ptr && zone(z) && (*ptr)["zone"] != z) {
        (*ptr)["zone"] = z;
        ptr->erase("grid");
    }
}
std::string Engine::add_to_tab(std::string s, const fs::path &p, std::string label) {
    invalidate_search();
    auto t = tab(s);
    if (!t)
        return "";
    auto path = pathstr(fs::absolute(p).lexically_normal());
    for (auto &i : (*t)["items"])
        if (path_equal(i["path"].get<std::string>(), path))
            return i["id"];
    auto ident = id();
    auto kind = fs::is_directory(p) ? "folder" : lower(pathstr(p.extension()));
    (*t)["items"].push_back({{"id", ident},
                             {"path", path},
                             {"name", label.empty() ? filename(p) : label},
                             {"kind", kind},
                             {"uses", 0},
                             {"used", 0}});
    return ident;
}
std::vector<std::string> Engine::sorted(std::string s) const {
    const Json *t = nullptr;
    for (auto &candidate : state["tabs"])
        if (candidate["id"] == s)
            t = &candidate;
    if (!t)
        return {};
    std::vector<const Json *> list;
    for (auto &i : (*t)["items"])
        list.push_back(&i);
    auto sort = t->value("sort", "manual");
    if (sort != "manual")
        std::stable_sort(list.begin(), list.end(), [&](auto a, auto b) {
            if (sort == "used" && (*a)["uses"] != (*b)["uses"])
                return (*a)["uses"].template get<int>() > (*b)["uses"].template get<int>();
            if (sort == "recent" && (*a)["used"] != (*b)["used"])
                return (*a)["used"].template get<int64_t>() > (*b)["used"].template get<int64_t>();
            if (sort == "type" && (*a)["kind"] != (*b)["kind"])
                return a->value("kind", "") < b->value("kind", "");
            return lower(a->value("name", "")) < lower(b->value("name", ""));
        });
    std::vector<std::string> result;
    for (auto p : list)
        result.push_back((*p)["id"]);
    return result;
}
void Engine::move_launch(std::string ident, std::string target, std::string before) {
    invalidate_search();
    if (ident == before)
        return;
    auto dest = tab(target);
    auto ptr = launch(ident);
    if (!dest || !ptr)
        return;
    auto moved = *ptr;
    if (dest->value("sort", "manual") != "manual") {
        auto ordered = Json::array();
        for (auto &s : sorted(target))
            ordered.push_back(*launch(s));
        (*dest)["items"] = ordered;
        (*dest)["sort"] = "manual";
    }
    for (auto &t : state["tabs"]) {
        auto &a = t["items"];
        a.erase(std::remove_if(a.begin(), a.end(), [&](auto &i) { return i["id"] == ident; }), a.end());
    }
    auto &a = (*tab(target))["items"];
    a.erase(std::remove_if(a.begin(), a.end(),
                           [&](auto &i) {
                               return path_equal(i["path"].template get<std::string>(),
                                                 moved["path"].get<std::string>());
                           }),
            a.end());
    auto pos = std::find_if(a.begin(), a.end(), [&](auto &i) { return i["id"] == before; });
    a.insert(pos, moved);
}
std::vector<Match> Engine::search(std::string query, bool launcher, std::string active) const {
    auto normalized = lower(query);
    std::istringstream input(normalized);
    std::vector<std::string> tokens;
    for (std::string token; input >> token;)
        tokens.push_back(token);
    std::vector<Match> results;
    if (launcher && tokens.empty()) {
        for (auto &ident : sorted(active))
            results.push_back({ident, active, 0});
        return results;
    }
    size_t count = 0;
    for (auto &tab : state["tabs"])
        count += tab["items"].size();
    auto language = state["settings"].value("language", "zh");
    if (search_dirty || indexed_items != state["items"].size() || indexed_launchers != count ||
        indexed_language != language) {
        desktop_index.clear();
        launcher_index.clear();
        desktop_index.reserve(state["items"].size());
        launcher_index.reserve(count);
        for (auto &item : state["items"]) {
            std::string group;
            for (auto &z : state["zones"])
                if (z["id"] == item["zone"]) {
                    group = lower(name(z));
                    break;
                }
            desktop_index.push_back(
                {item["id"], "", lower(item.value("name", "")), lower(item.value("path", "")), group});
        }
        for (auto &tab : state["tabs"]) {
            auto group = lower(name(tab));
            for (auto &item : tab["items"])
                launcher_index.push_back({item["id"], tab["id"], lower(item.value("name", "")),
                                          lower(item.value("path", "")), group});
        }
        search_dirty = false;
        indexed_items = state["items"].size();
        indexed_launchers = count;
        indexed_language = language;
    }
    auto score = [&](const SearchEntry &entry) {
        int total = 0;
        for (auto &token : tokens) {
            if (entry.name.starts_with(token))
                total += 100;
            else if (entry.name.find(token) != std::string::npos)
                total += 70;
            else if (entry.group.find(token) != std::string::npos)
                total += 35;
            else if (entry.path.find(token) != std::string::npos)
                total += 20;
            else
                return -1;
        }
        return total;
    };
    for (auto &entry : (launcher ? launcher_index : desktop_index)) {
        int value = score(entry);
        if (value >= 0)
            results.push_back({entry.id, entry.tab, value});
    }
    std::stable_sort(results.begin(), results.end(), [](auto &a, auto &b) { return a.score > b.score; });
    return results;
}
int Engine::age(const Json &i) const {
    auto last =
        std::max({i.value("written", int64_t(0)), i.value("used", int64_t(0)), i.value("kept", int64_t(0))});
    return std::max(0, static_cast<int>((clock() - last) / 86400));
}
std::vector<std::string> Engine::stale() const {
    std::vector<std::string> result;
    for (auto &i : state["items"]) {
        bool temporary = false;
        for (auto &z : state["zones"])
            if (z["id"] == i["zone"])
                temporary = z.value("temporary", false);
        if (temporary && !i.value("folder", false) && age(i) >= state["settings"].value("stale_days", 7) &&
            exists(i["path"].get<std::string>()))
            result.push_back(i["id"]);
    }
    return result;
}
void Engine::keep(std::string ident) {
    if (auto ptr = item(ident))
        (*ptr)["kept"] = clock();
}
void Engine::opened(std::string path) {
    for (auto &i : state["items"])
        if (path_equal(i["path"].get<std::string>(), path))
            i["used"] = clock();
    for (auto &t : state["tabs"])
        for (auto &i : t["items"])
            if (path_equal(i["path"].get<std::string>(), path)) {
                i["used"] = clock();
                i["uses"] = i.value("uses", 0) + 1;
            }
    for (auto &i : state["favorites"])
        if (path_equal(i["path"].get<std::string>(), path)) {
            i["used"] = clock();
            i["uses"] = i.value("uses", 0) + 1;
        }
}
fs::path Engine::unique_path(const fs::path &p) {
    if (!fs::exists(p))
        return p;
    for (int n = 2;; ++n) {
        auto q = p.parent_path() /
                 (p.stem().wstring() + L" (" + std::to_wstring(n) + L")" + p.extension().wstring());
        if (!fs::exists(q))
            return q;
    }
}
void Engine::finish_move(const Json &move) {
    invalidate_search();
    auto from = move["source"].get<std::string>(), to = move["destination"].get<std::string>();
    if (move["op"] == "archive") {
        auto ptr = item(move["item"].get<std::string>());
        if (!ptr)
            return;
        auto copy = *ptr;
        state["archive"].push_back(
            {{"id", move["id"]}, {"item", copy}, {"original", from}, {"path", to}, {"time", clock()}});
        auto &a = state["items"];
        a.erase(std::remove_if(a.begin(), a.end(), [&](auto &i) { return i["id"] == copy["id"]; }), a.end());
    } else {
        auto &a = state["archive"];
        auto it = std::find_if(a.begin(), a.end(), [&](auto &e) { return e["id"] == move["archive"]; });
        if (it == a.end())
            return;
        auto copy = (*it)["item"];
        copy["path"] = to;
        copy["name"] = filename(wide(to));
        copy["kept"] = clock();
        if (!zone(copy["zone"].get<std::string>())) {
            copy["zone"] = ensure_zone();
            copy.erase("grid");
        }
        state["items"].push_back(copy);
        a.erase(it);
    }
    repoint(from, to);
}
void Engine::recover_moves() {
    for (auto it = state["moves"].begin(); it != state["moves"].end();) {
        auto copy = *it;
        bool source = exists(copy["source"].get<std::string>()),
             dest = exists(copy["destination"].get<std::string>());
        if (!source && dest) {
            finish_move(copy);
            it = state["moves"].erase(it);
        } else if (source && !dest)
            it = state["moves"].erase(it);
        else
            ++it;
    }
    if (!state["moves"].empty())
        recovery_message = "Some interrupted file moves require review. The journal has been preserved.";
}
Result Engine::archive(const std::vector<std::string> &ids, const fs::path &root) {
    Result r;
    for (auto &ident : ids) {
        auto ptr = item(ident);
        if (!ptr)
            continue;
        auto copy = *ptr;
        std::string move_id;
        try {
            for (auto &pending : state["moves"])
                if (pending.value("item", "") == ident ||
                    path_equal(pending.value("source", ""), copy["path"].get<std::string>()))
                    throw std::runtime_error("An interrupted move for this file requires review");
            if (copy.value("folder", false))
                throw std::runtime_error("Folders are not archived automatically");
            auto source = fs::path(wide(copy["path"].get<std::string>()));
            if (!fs::exists(source))
                throw std::runtime_error("File is missing");
            time_t time = clock();
            tm date{};
            localtime_s(&date, &time);
            char buffer[32]{};
            strftime(buffer, sizeof(buffer), "%Y-%m-%d", &date);
            auto folder = root / wide(buffer);
            fs::create_directories(folder);
            auto dest = unique_path(folder / source.filename());
            move_id = id();
            Json move = {{"id", move_id},
                         {"op", "archive"},
                         {"item", ident},
                         {"source", pathstr(source)},
                         {"destination", pathstr(dest)}};
            state["moves"].push_back(move);
            save();
            if (!MoveFileExW(source.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error(win_error());
            finish_move(move);
            auto &a = state["moves"];
            a.erase(std::remove_if(a.begin(), a.end(), [&](auto &m) { return m["id"] == move_id; }), a.end());
            save();
            ++r.succeeded;
        } catch (const std::exception &ex) {
            r.errors.push_back(copy.value("name", "") + ": " + ex.what());
            recover_moves();
            save();
        }
    }
    state["last_review"] = clock();
    save();
    return r;
}
Result Engine::restore(const std::vector<std::string> &ids) {
    Result r;
    auto snapshot = state["archive"];
    for (auto &e : snapshot) {
        if (!ids.empty() && std::find(ids.begin(), ids.end(), e["id"].get<std::string>()) == ids.end())
            continue;
        try {
            for (auto &pending : state["moves"])
                if (pending.value("archive", "") == e["id"].get<std::string>())
                    throw std::runtime_error("An interrupted restore for this file requires review");
            auto from = fs::path(wide(e["path"].get<std::string>()));
            if (!fs::exists(from))
                throw std::runtime_error("Archived file is missing");
            auto dest = unique_path(wide(e["original"].get<std::string>()));
            fs::create_directories(dest.parent_path());
            Json move = {{"id", id()},
                         {"op", "restore"},
                         {"archive", e["id"]},
                         {"source", pathstr(from)},
                         {"destination", pathstr(dest)}};
            state["moves"].push_back(move);
            save();
            if (!MoveFileExW(from.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error(win_error());
            finish_move(move);
            auto &a = state["moves"];
            a.erase(std::remove_if(a.begin(), a.end(), [&](auto &m) { return m["id"] == move["id"]; }),
                    a.end());
            save();
            ++r.succeeded;
        } catch (const std::exception &ex) {
            r.errors.push_back(e["item"].value("name", "") + ": " + ex.what());
            recover_moves();
            save();
        }
    }
    return r;
}
} // namespace deskedge
