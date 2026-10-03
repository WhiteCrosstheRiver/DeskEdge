#include "platform.h"
#include <algorithm>
#include <cmath>

namespace deskedge {
bool Window::animations_enabled() const {
    if (app.self_test_ui && test_animations)
        return true;
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    return enabled && !(contrast.dwFlags & HCF_HIGHCONTRASTON) &&
           app.engine.state["settings"].value("animations", true) && !app.self_test_ui &&
           !app.render_previews && kind != Kind::Draw;
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
        LONG drag_width = drag_bounds.right - drag_bounds.left,
             drag_height = drag_bounds.bottom - drag_bounds.top;
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
    geometry_capture = false;
    moving = resizing = false;
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
