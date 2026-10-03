#include "core.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>
using namespace deskedge;
static int checks = 0;
static void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
static fs::path file(const fs::path &folder, const wchar_t *name) {
    auto p = folder / name;
    std::ofstream out(p, std::ios::binary);
    out << "owned test fixture\n";
    return p;
}
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto root = fs::temp_directory_path() / (L"DeskEdge-tests-" + wide(id()));
    fs::create_directories(root / L"desktop");
    try {
        Engine e(root / L"state");
        auto desktop = root / L"desktop", archives = desktop / L"DeskEdge Archive";
        e.clock = []() { return int64_t(2000000000); };
        auto a = file(desktop, L"临时计算.txt"), b = file(desktop, L"Alpha results.csv"),
             c = file(desktop, L"Alpha notes.pdf");
        auto ia = e.add_file(a, "temp", true), ib = e.add_file(b, "data", true),
             ic = e.add_file(c, "papers", true);
        require(!ia.empty() && e.state["items"].size() == 3, "Import Unicode files");
        require(e.add_file(a, "temp") == ia && e.state["items"].size() == 3,
                "Duplicate import is idempotent");
        e.move_zone(ib, "projects");
        require(e.item(ib)->at("zone") == "projects" && fs::exists(b), "Grouping preserves files");
        e.item(ia)->at("written") = e.clock() - 20 * 86400;
        e.item(ib)->at("written") = e.clock() - 20 * 86400;
        require(e.stale() == std::vector<std::string>{ia}, "Only temporary files become stale");
        e.keep(ia);
        require(e.stale().empty(), "Keep resets age");
        e.item(ia)->at("kept") = 0;
        auto result = e.search("alpha notes");
        require(result.size() == 1 && result[0].id == ic, "Search uses AND tokens");
        require(e.search("临时").size() == 1, "Search matches Unicode");
        require(e.search("ALPHA").size() == 2, "Search is case insensitive");
        auto la = e.add_to_tab("software", a), lb = e.add_to_tab("software", b);
        e.add_to_tab("reading", c);
        require(e.add_to_tab("software", a) == la, "Launcher deduplication");
        e.tab("software")->at("sort") = "name";
        e.move_launch(lb, "software", la);
        require(e.tab("software")->at("sort") == "manual" && e.sorted("software")[0] == lb,
                "Dragging freezes sorted order");
        e.move_launch(la, "tools");
        require(e.tab("tools")->at("items").size() == 1 && e.tab("software")->at("items").size() == 1,
                "Cross tab move is unique");
        require(e.search("alpha", true, "tools").size() == 2, "Launcher search spans tabs");
        e.state["todos"].push_back(
            {{"id", "todo"}, {"text", "Read"}, {"link", pathstr(a)}, {"done", false}, {"due", ""}});
        auto ar = e.archive({ia}, archives);
        require(ar.succeeded == 1 && ar.errors.empty() && !fs::exists(a), "Archive moves actual file");
        require(e.state["archive"].size() == 1 && e.state["moves"].empty(), "Archive journal completes");
        auto archived = e.state["archive"][0]["path"].get<std::string>();
        require(e.launch(la)->at("path") == archived && e.state["todos"][0]["link"] == archived,
                "Archive repoints references");
        file(desktop, L"临时计算.txt");
        auto restored = e.restore();
        require(restored.succeeded == 1 && fs::exists(a) && e.state["archive"].empty(),
                "Restore preserves an existing same name file");
        auto renamed = e.item(ia)->at("path").get<std::string>();
        require(renamed != pathstr(a) && fs::exists(wide(renamed)), "Restore resolves collisions");
        require(e.launch(la)->at("path") == renamed && e.state["todos"][0]["link"] == renamed,
                "Restore repoints references");
        require(e.stale().empty(), "Restored item starts fresh");
        auto locked = file(desktop, L"locked.tmp");
        auto il = e.add_file(locked, "temp");
        HANDLE handle =
            CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Acquire test file lock");
        auto failed = e.archive({il}, archives);
        CloseHandle(handle);
        require(failed.succeeded == 0 && failed.errors.size() == 1 && fs::exists(locked) && e.item(il),
                "Archive failures preserve the source and record");
        auto dest = root / L"interrupted.tmp";
        Json move = {{"id", id()},
                     {"op", "archive"},
                     {"item", il},
                     {"source", pathstr(locked)},
                     {"destination", pathstr(dest)}};
        e.state["moves"].push_back(move);
        e.save();
        fs::rename(locked, dest);
        Engine recovered(root / L"state");
        require(!recovered.item(il) && recovered.state["moves"].empty() &&
                    recovered.state["archive"].size() == 1,
                "Interrupted move recovery");
        recovered.clock = e.clock;
        require(recovered.restore().succeeded == 1 && fs::exists(locked),
                "Recovered archive remains restorable");
        auto renamed_b = desktop / L"renamed results.csv";
        fs::rename(b, renamed_b);
        recovered.rename_path(b, renamed_b);
        require(recovered.item(ib)->at("path") == pathstr(renamed_b) &&
                    recovered.launch(lb)->at("path") == pathstr(renamed_b),
                "Filesystem renames update references");
        std::vector<std::string> zone_ids;
        for (auto &z : recovered.state["zones"])
            zone_ids.push_back(z["id"]);
        for (auto &z : zone_ids)
            recovered.remove_zone(z);
        require(!recovered.state["zones"].empty(), "Last zone removal creates a fallback");
        for (auto &i : recovered.state["items"])
            require(recovered.zone(i["zone"].get<std::string>()) != nullptr, "No orphaned items");
        recovered.save();
        recovered.state["settings"]["language"] = "en";
        recovered.save();
        {
            std::ofstream broken(root / L"state" / L"state.json");
            broken << "{broken";
        }
        Engine backup(root / L"state");
        require(!backup.recovery_message.empty() &&
                    backup.state["items"].size() == recovered.state["items"].size(),
                "Corrupt primary recovers from backup");
        {
            auto malformed = backup.state;
            malformed["zones"] = "invalid";
            std::ofstream out(root / L"state" / L"state.json");
            out << malformed.dump();
        }
        Engine schema_backup(root / L"state");
        require(!schema_backup.recovery_message.empty(),
                "Valid JSON with an invalid schema recovers from backup");
        Engine ambiguous(root / L"ambiguous-state");
        auto src = file(desktop, L"ambiguous.txt"), dst = root / L"partial-copy.txt";
        auto aid = ambiguous.add_file(src, "temp");
        ambiguous.state["moves"].push_back({{"id", id()},
                                            {"op", "archive"},
                                            {"item", aid},
                                            {"source", pathstr(src)},
                                            {"destination", pathstr(dst)}});
        ambiguous.save();
        fs::copy_file(src, dst);
        Engine partial(root / L"ambiguous-state");
        require(partial.state["moves"].size() == 1 && partial.item(aid) && fs::exists(src) && fs::exists(dst),
                "Ambiguous copies preserve both files and the move journal");
        require(partial.archive({aid}, archives).succeeded == 0 && partial.state["moves"].size() == 1,
                "Ambiguous moves cannot be retried silently");
        Engine bench(root / L"benchmark");
        auto start = std::chrono::steady_clock::now();
        for (int n = 0; n < 10000; n++)
            bench.state["items"].push_back({{"id", std::to_string(n)},
                                            {"name", "Dataset experiment " + std::to_string(n) + ".csv"},
                                            {"path", "C:\\Research\\dataset" + std::to_string(n) + ".csv"},
                                            {"zone", "data"}});
        auto build_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        start = std::chrono::steady_clock::now();
        size_t found = 0;
        for (int n = 0; n < 20; n++)
            found = bench.search("experiment 999").size();
        auto search_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20;
        require(found == 19, "10k item benchmark results");
        std::cout << "PASS " << checks << " checks\n10,000 items: create " << build_ms << " ms, search mean "
                  << search_ms << " ms\n";
        fs::remove_all(root);
        CoUninitialize();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << "\nFixtures retained at " << pathstr(root) << "\n";
        CoUninitialize();
        return 1;
    }
}
