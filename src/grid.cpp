#include "core.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace deskedge {
float snap_coordinate(float value) {
    return std::round(value / WINDOW_GRID) * WINDOW_GRID;
}
RegionRect snap_region(RegionRect rect, float area_width, float area_height, bool resize) {
    if (resize) {
        rect.w = std::clamp(
            snap_coordinate(rect.w), std::min(144.f, area_width),
            std::max(std::min(144.f, area_width), std::floor(area_width / WINDOW_GRID) * WINDOW_GRID));
        rect.h = std::clamp(
            snap_coordinate(rect.h), std::min(96.f, area_height),
            std::max(std::min(96.f, area_height), std::floor(area_height / WINDOW_GRID) * WINDOW_GRID));
    }
    auto maximum_x = std::max(0.f, std::floor((area_width - rect.w) / WINDOW_GRID) * WINDOW_GRID);
    auto maximum_y = std::max(0.f, std::floor((area_height - rect.h) / WINDOW_GRID) * WINDOW_GRID);
    rect.x = std::clamp(snap_coordinate(rect.x), 0.f, maximum_x);
    rect.y = std::clamp(snap_coordinate(rect.y), 0.f, maximum_y);
    return rect;
}
bool region_conflict(RegionRect a, RegionRect b, float gap) {
    return a.x < b.x + b.w + gap && b.x < a.x + a.w + gap && a.y < b.y + b.h + gap && b.y < a.y + a.h + gap;
}
void Engine::ensure_zone_grid() {
    if (!state["settings"].value("window_grid", true))
        return;
    bool repair = !state["settings"].value("window_grid_migrated", false);
    std::unordered_map<std::string, std::vector<RegionRect>> placed;
    for (auto &zone : state["zones"]) {
        RegionRect rect{zone.value("x", 24.f), zone.value("y", 24.f), zone.value("w", 316.f),
                        zone.value("h", 196.f)};
        // Upgrade without shrinking an existing region or removing its icon columns.
        rect.w = std::ceil((rect.w - .01f) / WINDOW_GRID) * WINDOW_GRID;
        rect.h = std::ceil((rect.h - .01f) / WINDOW_GRID) * WINDOW_GRID;
        rect = snap_region(rect, 10032, 10032);
        auto &occupied = placed[zone.value("monitor", "")];
        if (repair)
            for (auto previous : occupied) {
                if (std::abs(rect.x - previous.x) <= WINDOW_GRID)
                    rect.x = previous.x;
                if (std::abs(rect.y - previous.y) <= WINDOW_GRID)
                    rect.y = previous.y;
            }
        auto available = [&](RegionRect candidate) {
            return std::none_of(occupied.begin(), occupied.end(),
                                [&](auto other) { return region_conflict(candidate, other); });
        };
        if (repair && !available(rect)) {
            std::vector<float> xs{rect.x, 0}, ys{rect.y, 0};
            for (auto previous : occupied) {
                xs.push_back(previous.x + previous.w + WINDOW_GRID);
                xs.push_back(previous.x - rect.w - WINDOW_GRID);
                ys.push_back(previous.y + previous.h + WINDOW_GRID);
                ys.push_back(previous.y - rect.h - WINDOW_GRID);
            }
            float best = std::numeric_limits<float>::max();
            auto destination = rect;
            for (float x : xs)
                for (float y : ys) {
                    auto candidate = RegionRect{x, y, rect.w, rect.h};
                    float distance = (x - rect.x) * (x - rect.x) + (y - rect.y) * (y - rect.y);
                    if (x >= 0 && y >= 0 && distance < best && available(candidate)) {
                        best = distance;
                        destination = candidate;
                    }
                }
            rect = destination;
        }
        zone["x"] = rect.x;
        zone["y"] = rect.y;
        zone["w"] = rect.w;
        zone["h"] = rect.h;
        occupied.push_back(rect);
    }
    state["settings"]["window_grid_migrated"] = true;
}
int grid_columns(float width) {
    // Reserve the scrollbar width so its appearance does not change the grid's column count.
    return std::max(1, static_cast<int>((width - 27.f) / GRID_CELL_WIDTH));
}
namespace {
int64_t key(GridCell cell, int columns) {
    return static_cast<int64_t>(cell.row) * columns + cell.column;
}
GridCell cell_at(int64_t index, int columns) {
    return {static_cast<int>(index % columns), static_cast<int>(index / columns)};
}
bool saved_cell(const Json &item, GridCell &cell) {
    auto grid = item.find("grid");
    if (grid == item.end() || !grid->is_object() || !grid->contains("column") || !grid->contains("row") ||
        !(*grid)["column"].is_number_integer() || !(*grid)["row"].is_number_integer())
        return false;
    auto column = (*grid)["column"].get<int64_t>(), row = (*grid)["row"].get<int64_t>();
    if (column < 0 || column > 10000 || row < 0 || row > 100000)
        return false;
    cell = {static_cast<int>(column), static_cast<int>(row)};
    return true;
}
void store(Json &item, GridCell cell) {
    item["grid"] = {{"column", cell.column}, {"row", cell.row}};
}
} // namespace
std::vector<GridItem> Engine::zone_grid(std::string_view zone_id, int columns) const {
    columns = std::max(1, columns);
    std::vector<GridItem> result;
    std::vector<size_t> unresolved;
    std::unordered_set<int64_t> occupied;
    for (auto &item : state["items"]) {
        if (item["zone"].get_ref<const std::string &>() != zone_id)
            continue;
        GridCell cell{};
        bool valid = saved_cell(item, cell);
        cell.column = std::min(cell.column, columns - 1);
        result.push_back({item["id"], cell});
        if (!valid || !occupied.insert(key(cell, columns)).second)
            unresolved.push_back(result.size() - 1);
    }
    // Assign missing/duplicate positions after reserving all existing positions, preserving holes.
    int64_t next_free = 0;
    for (auto index : unresolved) {
        while (occupied.contains(next_free))
            ++next_free;
        result[index].cell = cell_at(next_free, columns);
        occupied.insert(next_free++);
    }
    return result;
}
void Engine::ensure_grid() {
    if (!state["settings"].value("grid_mode", true))
        return;
    std::unordered_map<std::string, Json *> by_id;
    for (auto &entry : state["items"])
        by_id.emplace(entry["id"], &entry);
    for (auto &zone : state["zones"])
        for (auto &placed : zone_grid(zone["id"].get<std::string>(), grid_columns(zone.value("w", 316.f))))
            store(*by_id.at(placed.id), placed.cell);
}
void Engine::arrange_grid(std::string zone_id) {
    for (auto &entry : state["items"])
        if (entry["zone"] == zone_id)
            entry.erase("grid");
    ensure_grid();
}
void Engine::place_grid(const std::vector<std::string> &ids, std::string zone_id, GridCell target,
                        int columns, std::string anchor) {
    if (!zone(zone_id) || ids.empty())
        return;
    columns = std::max(1, columns);
    target.column = std::clamp(target.column, 0, columns - 1);
    target.row = std::clamp(target.row, 0, 100000);
    ensure_grid();
    std::unordered_map<std::string, Json *> by_id;
    for (auto &entry : state["items"])
        by_id.emplace(entry["id"], &entry);
    std::vector<Json *> moving;
    std::unordered_set<std::string> moving_ids;
    for (auto &ident : ids)
        if (auto entry = by_id.find(ident); entry != by_id.end() && moving_ids.insert(ident).second)
            moving.push_back(entry->second);
    if (moving.empty())
        return;
    auto anchor_found = by_id.find(anchor);
    auto anchor_item = anchor_found == by_id.end() ? nullptr : anchor_found->second;
    if (!anchor_item || !moving_ids.contains(anchor))
        anchor_item = moving.front();
    auto anchor_position = std::find(moving.begin(), moving.end(), anchor_item);
    if (anchor_position != moving.end())
        std::iter_swap(moving.begin(), anchor_position);
    GridCell anchor_cell{};
    saved_cell(*anchor_item, anchor_cell);
    std::string origin_zone = (*anchor_item)["zone"];
    std::unordered_map<int64_t, std::string> occupied;
    std::vector<GridCell> vacant;
    for (auto &placed : zone_grid(zone_id, columns))
        if (moving_ids.contains(placed.id))
            vacant.push_back(placed.cell);
        else
            occupied.emplace(key(placed.cell, columns), placed.id);
    std::vector<GridCell> desired;
    std::unordered_set<int64_t> reserved;
    for (auto entry : moving) {
        GridCell original{}, destination = target;
        if ((*entry)["zone"] == origin_zone && saved_cell(*entry, original)) {
            destination.column += original.column - anchor_cell.column;
            destination.row += original.row - anchor_cell.row;
        } else {
            destination = cell_at(key(target, columns) + static_cast<int64_t>(desired.size()), columns);
        }
        auto index = std::max<int64_t>(0, key(destination, columns));
        while (reserved.contains(index))
            ++index;
        reserved.insert(index);
        desired.push_back(cell_at(index, columns));
    }
    std::vector<std::string> displaced;
    for (auto destination : desired) {
        auto index = key(destination, columns);
        if (auto found = occupied.find(index); found != occupied.end()) {
            displaced.push_back(found->second);
            occupied.erase(found);
        }
    }
    for (size_t i = 0; i < moving.size(); ++i) {
        (*moving[i])["zone"] = zone_id;
        store(*moving[i], desired[i]);
        occupied.emplace(key(desired[i], columns), (*moving[i])["id"]);
    }
    // Occupied-cell drops use the vacated source cells first, giving single-item drags a true swap.
    size_t vacancy = 0;
    int64_t next_free = 0;
    for (auto &ident : displaced) {
        while (vacancy < vacant.size() && occupied.contains(key(vacant[vacancy], columns)))
            ++vacancy;
        GridCell destination{};
        if (vacancy < vacant.size())
            destination = vacant[vacancy++];
        else {
            while (occupied.contains(next_free))
                ++next_free;
            destination = cell_at(next_free++, columns);
        }
        store(*by_id.at(ident), destination);
        occupied.emplace(key(destination, columns), ident);
    }
    invalidate_search();
}
} // namespace deskedge
