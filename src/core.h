#pragma once
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace deskedge {
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;
std::wstring wide(std::string_view value);
std::string utf8(std::wstring_view value);
std::string lower(std::string_view value);
std::string id();
int64_t now();
std::string filename(const fs::path &path);
std::string pathstr(const fs::path &path);
bool path_equal(std::string_view a, std::string_view b);
int64_t file_time(const fs::path &path);
std::string win_error(DWORD code = GetLastError());
struct Result {
    int succeeded = 0;
    std::vector<std::string> errors;
};
struct Match {
    std::string id, tab;
    int score = 0;
};

class Engine {
  public:
    Json state;
    fs::path data_dir;
    std::function<int64_t()> clock = now;
    std::string recovery_message;
    explicit Engine(fs::path directory);
    static Json defaults();
    void save();
    void invalidate_search() {
        search_dirty = true;
    }
    Json *zone(std::string_view zone_id);
    Json *item(std::string_view item_id);
    Json *tab(std::string_view tab_id);
    Json *launch(std::string_view item_id);
    std::string name(const Json &object) const;
    std::string add_zone(float x, float y, float w, float h, std::string monitor);
    void remove_zone(std::string zone_id);
    std::string add_file(const fs::path &path, std::string zone_id = "", bool desktop = false);
    void sync(const std::vector<fs::path> &roots, const fs::path &archive_root);
    void rename_path(const fs::path &old_path, const fs::path &new_path);
    void move_zone(std::string item_id, std::string zone_id);
    std::string add_to_tab(std::string tab_id, const fs::path &path, std::string label = "");
    void move_launch(std::string item_id, std::string tab_id, std::string before_id = "");
    std::vector<std::string> sorted(std::string tab_id) const;
    std::vector<Match> search(std::string query, bool launcher = false, std::string active_tab = "") const;
    std::vector<std::string> stale() const;
    int age(const Json &object) const;
    void keep(std::string item_id);
    void opened(std::string path);
    Result archive(const std::vector<std::string> &ids, const fs::path &root);
    Result restore(const std::vector<std::string> &archive_ids = {});
    void recover_moves();
    static fs::path unique_path(const fs::path &path);

  private:
    struct SearchEntry {
        std::string id, tab, name, path, group;
    };
    mutable std::vector<SearchEntry> desktop_index, launcher_index;
    mutable bool search_dirty = true;
    mutable size_t indexed_items = 0, indexed_launchers = 0;
    mutable std::string indexed_language;
    void repoint(std::string old_path, std::string new_path);
    void finish_move(const Json &move);
    std::string ensure_zone();
};
} // namespace deskedge
