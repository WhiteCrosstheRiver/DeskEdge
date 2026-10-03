#pragma once
#include "core.h"
#include "imgui.h"
#include <atomic>
#include <condition_variable>
#include <d3d11.h>
#include <dcomp.h>
#include <deque>
#include <dwmapi.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <memory>
#include <mutex>
#include <oleidl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <thread>
#include <unordered_map>
#include <wrl/client.h>

namespace deskedge {
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
constexpr UINT MSG_WAKE = WM_APP + 1, MSG_ICONS = WM_APP + 2, MSG_FILES = WM_APP + 3, MSG_EDGE = WM_APP + 4,
               MSG_TRAY = WM_APP + 5;
struct Monitor {
    std::string name;
    RECT work{}, bounds{};
    float scale = 1;
    float width() const {
        return (work.right - work.left) / scale;
    }
    float height() const {
        return (work.bottom - work.top) / scale;
    }
};
std::vector<Monitor> monitors();
Monitor monitor(std::string name = "", bool at_cursor = false);
HWND desktop_view();
HWND desktop_icons();
bool system_dark();
void blur(HWND hwnd, bool dark);
void shell_open(std::string path, HWND owner = nullptr);
void reveal(std::string path);
std::vector<fs::path> pick_files(HWND owner, bool folder = false);
fs::path known_folder(REFKNOWNFOLDERID ident);
std::vector<Json> discover_apps();
void startup(bool enabled);
void write_log(const fs::path &folder, std::string message);
void native_drag(HWND owner, const Json &payload, const std::vector<std::string> &paths);
ComPtr<IDataObject> shell_data(const std::vector<std::string> &paths, const Json &payload = Json());
ComPtr<IContextMenu> shell_context(HWND owner, const std::vector<std::string> &paths,
                                   const fs::path &background = {});
ComPtr<IDropTarget> shell_drop_target(const fs::path &folder, HWND owner = nullptr);
// Returns only an application command; Windows commands are invoked inside this function.
UINT shell_menu(HWND owner, IContextMenu *context, HMENU extra, std::string *verb = nullptr);
void shell_verb(HWND owner, const std::vector<std::string> &paths, const char *verb);
void shell_folder_verb(HWND owner, const fs::path &folder, const char *verb);
void shell_rename(HWND owner, const fs::path &path, const std::string &name);
void shell_rename_batch(HWND owner, const std::vector<std::pair<fs::path, std::string>> &files);
std::vector<std::string> data_files(IDataObject *data);
Json data_payload(IDataObject *data);

enum class Kind { Panel, Launcher, Zone, Draw, Settings };
class Application;
class Window;
class GlassMaterial {
    struct Surface {
        RECT bounds{};
        ComPtr<ID3D11ShaderResourceView> blurred;
    };
    ID3D11Device *device;
    ComPtr<ID3D11ShaderResourceView> grain_texture;
    std::unordered_map<std::string, Surface> surfaces;

  public:
    explicit GlassMaterial(ID3D11Device *device) : device(device) {}
    ID3D11ShaderResourceView *wallpaper(const Monitor &screen);
    ID3D11ShaderResourceView *grain();
    void clear() {
        surfaces.clear();
    }
};
IDropTarget *create_drop_target(Window &window);
struct Hotspot {
    RECT rect{};
    std::string tab, before;
    std::string path;
};
class Window {
  public:
    Application &app;
    Kind kind;
    HWND hwnd = nullptr;
    ImGuiContext *context = nullptr;
    std::string zone_id;
    Monitor screen;
    float scale = 1;
    bool dirty = true, desktop_child = false, closing = false, focus_search = false;
    int width = 0, height = 0, settle_frames = 0;
    double last_frame = 0;
    bool popup_open = false;
    std::string search, draft, rename_id, rename_buffer;
    std::vector<std::string> selection;
    std::string selection_anchor, file_rename, file_rename_buffer, file_rename_suffix;
    bool file_rename_select = false;
    std::vector<std::string> file_rename_ids;
    bool file_rename_focus = false;
    bool selecting = false;
    ImVec2 selection_start{};
    std::vector<std::string> selection_base;
    std::vector<Hotspot> hotspots;
    POINT drop_point{};
    bool drop_hover = false, drag_in_progress = false;
    POINT drag_start{};
    POINT drag_origin{};
    float initial_x = 0, initial_y = 0, initial_w = 0, initial_h = 0;
    bool moving = false, resizing = false;
    bool geometry_capture = false, geometry_changed = false, surface_dirty = false;
    bool requested_visible = false, fading_out = false, size_animating = false;
    bool test_animations = false; // Only honored by the isolated UI test runner.
    double opacity_started = 0;
    float opacity_from = 1, opacity_to = 1, opacity_seconds = 0;
    RECT drag_bounds{}, layout_bounds{}, size_from{}, size_to{};
    std::vector<Monitor> drag_monitors;
    double size_started = 0;
    uint64_t geometry_updates = 0, surface_allocations = 0;
    bool review_open = false;
    std::function<void(ImGuiIO &)> test_input;
    std::unordered_map<std::string, ImVec4> test_controls;
    int selected_index = 0;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<IDCompositionTarget> composition_target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> opacity_effect;
    ComPtr<IDCompositionRectangleClip> clip;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    IDropTarget *drop_target = nullptr;
    explicit Window(Application &owner, Kind type, std::string ident = "");
    ~Window();
    void create(bool child = false);
    void resize(int w, int h);
    void update_surface();
    void begin_geometry(POINT point, bool resize);
    void update_geometry(POINT point);
    void end_geometry(bool cancel = false);
    bool animations_enabled() const;
    void animate_opacity(float from, float to, float seconds);
    void finish_motion();
    void place(float x, float y, float w, float h, Monitor monitor, bool activate = false);
    void show(bool activate = false);
    void hide();
    bool visible() const;
    void invalidate();
    void render();
    void export_preview(const fs::path &path);
    void theme();
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
};

class IconCache {
    struct Entry {
        std::vector<uint8_t> pixels;
        std::string display_name;
        ComPtr<ID3D11ShaderResourceView> view;
        uint64_t touched = 0;
        bool queued = false, ready = false;
    };
    std::unordered_map<std::string, Entry> entries;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> requests;
    std::thread worker;
    bool stopping = false;
    uint64_t sequence = 0;
    ID3D11Device *device;
    HWND notify;
    void run();

  public:
    IconCache(ID3D11Device *device, HWND notify);
    ~IconCache();
    ID3D11ShaderResourceView *get(const std::string &path);
    std::string display_name(const std::string &path, const std::string &fallback);
    size_t size();
    void clear();
};

class DirectoryWatch {
    HANDLE directory = INVALID_HANDLE_VALUE, stop_event = nullptr;
    std::thread worker;

  public:
    DirectoryWatch(fs::path path, HWND notify);
    ~DirectoryWatch();
};

class Application {
  public:
    Engine engine;
    std::vector<fs::path> roots;
    fs::path archive_root;
    bool demo = false, no_desktop = false, quit = false, peek = false, drawing = false, modal = false;
    int quit_after = 0;
    bool render_previews = false;
    bool self_test_ui = false;
    int run_ui_tests();
    bool native_icons_initial = false, icons_hidden = false;
    HWND controller = nullptr;
    HHOOK mouse_hook = nullptr;
    UINT taskbar_message = 0, wake_message = 0;
    HANDLE singleton = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> device_context;
    ComPtr<IDCompositionDevice> composition;
    std::unique_ptr<GlassMaterial> glass;
    std::unique_ptr<IconCache> icons;
    std::unique_ptr<Window> panel, launcher, settings;
    std::vector<std::unique_ptr<Window>> zones, draw_windows;
    std::vector<std::unique_ptr<DirectoryWatch>> watches;
    std::deque<std::function<void()>> actions;
    std::string query, focus_id, toast, active_tab = "software";
    int64_t toast_until = 0, focus_until = 0;
    bool dark = false;
    std::optional<Json> rename_event;
    std::atomic<uint64_t> rendered_frames{0};
    explicit Application(fs::path data, bool test, bool unhosted);
    ~Application();
    int run();
    void synchronize();
    void commit();
    void defer(std::function<void()> action);
    void perform_actions();
    void notify(std::string message);
    void error(std::string message);
    std::string tr(const char *zh, const char *en) const;
    void show_panel(bool focus = false);
    void hide_panel();
    void show_launcher();
    void toggle_peek();
    void begin_draw();
    void cancel_draw();
    void focus(std::string ident);
    void open(std::string path);
    void archive(std::vector<std::string> ids);
    void restore();
    void drop(Window &dest, const Json &payload, const std::vector<std::string> &files, POINT point);
    void add_files(std::string zone_id = "", std::string tab_id = "", bool folder = false);
    void show_settings();
    void tray();
    void seed();
    void restore_icons();
    void apply_icons();
    static LRESULT CALLBACK controller_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp);
};
void draw_ui(Window &window);
} // namespace deskedge
