#include "platform.h"
#include <algorithm>
#include <cmath>

namespace deskedge {
bool Application::region_available(const Window *source, RegionRect bounds, const Monitor &screen) {
    for (auto &other : zones) {
        if (other.get() == source || other->screen.name != screen.name)
            continue;
        if (auto zone = engine.zone(other->zone_id)) {
            RegionRect occupied{zone->value("x", 0.f), zone->value("y", 0.f), zone->value("w", 316.f),
                                zone->value("h", 196.f)};
            if (region_conflict(bounds, occupied))
                return false;
        }
    }
    return true;
}
void Application::show_geometry_guide(Window &source, RegionRect bounds) {
    if (geometry_guide && !IsWindow(geometry_guide->hwnd))
        geometry_guide.reset();
    if (!geometry_guide) {
        geometry_guide = std::make_unique<Window>(*this, Kind::Guides);
        geometry_guide->create(!no_desktop);
    }
    auto &guide = *geometry_guide;
    // Keep the compositor surface near the region instead of allocating a whole-monitor overlay.
    float left = std::max(0.f, bounds.x - WINDOW_GRID * 2);
    float top = std::max(0.f, bounds.y - WINDOW_GRID * 2);
    float width = std::min(source.screen.width() - left, bounds.x + bounds.w + WINDOW_GRID * 2 - left);
    float height = std::min(source.screen.height() - top, bounds.y + bounds.h + WINDOW_GRID * 2 - top);
    guide.guide_bounds = {bounds.x - left, bounds.y - top, bounds.w, bounds.h};
    guide.placement_blocked = source.placement_blocked;
    guide.place(left, top, width, height, source.screen);
    if (!guide.visible()) {
        guide.show();
        SetWindowPos(guide.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    guide.invalidate();
}
std::optional<GridCell> Window::grid_at(POINT point) const {
    if (kind != Kind::Zone || !app.engine.state["settings"].value("grid_mode", true) ||
        !PtInRect(&grid_rect, point))
        return std::nullopt;
    return GridCell{
        std::clamp(static_cast<int>((point.x - grid_rect.left) / (GRID_CELL_WIDTH * scale)), 0,
                   grid_columns - 1),
        std::max(0, static_cast<int>((point.y - grid_rect.top + grid_scroll) / (GRID_CELL_HEIGHT * scale)))};
}
bool Window::animations_enabled() const {
    if (app.self_test_ui && test_animations)
        return true;
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    return enabled && !(contrast.dwFlags & HCF_HIGHCONTRASTON) &&
           app.engine.state["settings"].value("animations", true) && !app.self_test_ui &&
           !app.render_previews && kind != Kind::Draw && kind != Kind::Guides;
}
void Window::animate_opacity(float from, float to, float seconds) {
    double clock = static_cast<double>(GetTickCount64());
    if (opacity_seconds > 0 && IsWindowVisible(hwnd)) {
        float t =
            std::clamp(static_cast<float>((clock - opacity_started) / (opacity_seconds * 1000.)), 0.f, 1.f);
        from = opacity_from + (opacity_to - opacity_from) * (1 - (1 - t) * (1 - t) * (1 - t));
    }
    opacity_started = clock;
    opacity_from = from;
    opacity_to = to;
    opacity_seconds = seconds;
    ComPtr<IDCompositionAnimation> animation;
    if (FAILED(app.composition->CreateAnimation(&animation)))
        return;
    // Ease-out cubic runs in the compositor, with no application frame timer.
    float delta = to - from;
    animation->AddCubic(0., from, 3 * delta / seconds, -3 * delta / (seconds * seconds),
                        delta / (seconds * seconds * seconds));
    animation->End(seconds, to);
    opacity_effect->SetOpacity(animation.Get());
    if (kind == Kind::Panel || kind == Kind::Launcher || kind == Kind::Settings) {
        ComPtr<IDCompositionAnimation> slide;
        if (SUCCEEDED(app.composition->CreateAnimation(&slide))) {
            float distance = (kind == Kind::Panel ? 16.f : 8.f) * scale;
            float start = (1 - from) * distance, end = (1 - to) * distance, shift = end - start;
            slide->AddCubic(0., start, 3 * shift / seconds, -3 * shift / (seconds * seconds),
                            shift / (seconds * seconds * seconds));
            slide->End(seconds, end);
            if (kind == Kind::Panel)
                visual->SetOffsetX(slide.Get());
            else
                visual->SetOffsetY(slide.Get());
        }
    }
    app.composition->Commit();
}
void Window::finish_motion() {
    if (fading_out) {
        fading_out = false;
        KillTimer(hwnd, 4);
        if (!requested_visible)
            ShowWindow(hwnd, SW_HIDE);
    }
}
void Window::begin_geometry(POINT point, bool resize_gesture) {
    if (geometry_capture || kind == Kind::Panel || kind == Kind::Draw)
        return;
    KillTimer(hwnd, 5);
    size_animating = false;
    geometry_capture = true;
    geometry_changed = false;
    placement_blocked = false;
    drag_scale = scale;
    moving = !resize_gesture;
    resizing = resize_gesture;
    drag_start = point;
    GetWindowRect(hwnd, &drag_bounds);
    drag_monitors = monitors();
    SetFocus(hwnd);
    SetCapture(hwnd);
}
void Window::update_geometry(POINT point) {
    if (!geometry_capture)
        return;
    LONG dx = point.x - drag_start.x, dy = point.y - drag_start.y;
    if (!geometry_changed && std::abs(dx) < GetSystemMetrics(SM_CXDRAG) &&
        std::abs(dy) < GetSystemMetrics(SM_CYDRAG))
        return;
    RECT bounds = drag_bounds;
    if (resizing) {
        bounds.right =
            bounds.left +
            std::clamp<LONG>(drag_bounds.right - drag_bounds.left + dx, static_cast<LONG>(140 * scale),
                             std::max<LONG>(static_cast<LONG>(140 * scale), screen.work.right - bounds.left));
        bounds.bottom =
            bounds.top +
            std::clamp<LONG>(drag_bounds.bottom - drag_bounds.top + dy, static_cast<LONG>(80 * scale),
                             std::max<LONG>(static_cast<LONG>(80 * scale), screen.work.bottom - bounds.top));
    } else {
        for (auto &m : drag_monitors)
            if (PtInRect(&m.bounds, point)) {
                screen = m;
                break;
            }
        LONG drag_width = static_cast<LONG>(
                 std::round((drag_bounds.right - drag_bounds.left) * screen.scale / drag_scale)),
             drag_height = static_cast<LONG>(
                 std::round((drag_bounds.bottom - drag_bounds.top) * screen.scale / drag_scale));
        if (scale != screen.scale) {
            scale = screen.scale;
            theme();
        }
        bounds.left = std::clamp(drag_bounds.left + dx, screen.work.left,
                                 std::max(screen.work.left, screen.work.right - drag_width));
        bounds.top =
            std::clamp(drag_bounds.top + dy, screen.work.top,
                       std::max(screen.work.top, screen.work.bottom - static_cast<LONG>(34 * scale)));
        bounds.right = bounds.left + drag_width;
        bounds.bottom = bounds.top + drag_height;
    }
    if (kind == Kind::Zone && app.engine.state["settings"].value("window_grid", true)) {
        auto zone = app.engine.zone(zone_id);
        bool collapsed = zone && zone->value("collapsed", false);
        RegionRect logical{(bounds.left - screen.work.left) / scale, (bounds.top - screen.work.top) / scale,
                           (bounds.right - bounds.left) / scale,
                           collapsed ? zone->value("h", 196.f) : (bounds.bottom - bounds.top) / scale};
        logical = snap_region(logical, screen.width(), screen.height(), resizing);
        placement_blocked = !app.region_available(this, logical, screen);
        bounds.left = screen.work.left + static_cast<LONG>(std::round(logical.x * scale));
        bounds.top = screen.work.top + static_cast<LONG>(std::round(logical.y * scale));
        bounds.right = bounds.left + static_cast<LONG>(std::round(logical.w * scale));
        bounds.bottom = bounds.top + static_cast<LONG>(std::round((collapsed ? 34.f : logical.h) * scale));
        app.show_geometry_guide(*this, logical);
    }
    RECT current{};
    GetWindowRect(hwnd, &current);
    if (EqualRect(&current, &bounds))
        return;
    POINT origin{bounds.left, bounds.top};
    if (desktop_child)
        ScreenToClient(GetParent(hwnd), &origin);
    SetWindowPos(hwnd, nullptr, origin.x, origin.y, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    geometry_changed = true;
    ++geometry_updates;
    invalidate();
}
void Window::end_geometry(bool cancel) {
    if (!geometry_capture)
        return;
    cancel |= placement_blocked;
    geometry_capture = false;
    moving = resizing = false;
    placement_blocked = false;
    if (app.geometry_guide)
        app.geometry_guide->hide();
    if (GetCapture() == hwnd)
        ReleaseCapture();
    if (cancel) {
        POINT origin{drag_bounds.left, drag_bounds.top};
        if (desktop_child)
            ScreenToClient(GetParent(hwnd), &origin);
        SetWindowPos(hwnd, nullptr, origin.x, origin.y, drag_bounds.right - drag_bounds.left,
                     drag_bounds.bottom - drag_bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
        app.defer([] {});
    } else if (geometry_changed && kind == Kind::Zone) {
        RECT bounds{};
        GetWindowRect(hwnd, &bounds);
        auto destination = screen;
        auto ident = zone_id;
        app.defer([owner = &app, ident, bounds, destination] {
            if (auto zone = owner->engine.zone(ident)) {
                (*zone)["x"] = (bounds.left - destination.work.left) / destination.scale;
                (*zone)["y"] = (bounds.top - destination.work.top) / destination.scale;
                (*zone)["w"] = (bounds.right - bounds.left) / destination.scale;
                if (!zone->value("collapsed", false))
                    (*zone)["h"] = (bounds.bottom - bounds.top) / destination.scale;
                (*zone)["monitor"] = destination.name;
            }
        });
    }
    geometry_changed = false;
    invalidate();
}
} // namespace deskedge
