#include "platform.h"
#include <algorithm>
#include <array>
#include <commoncontrols.h>

namespace deskedge {
namespace {
struct Pixels {
    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
};
int icon_bucket(int size) {
    for (int bucket : {48, 64, 96, 128, 256})
        if (size <= bucket)
            return bucket;
    return 256;
}
std::string cache_key(const std::string &path, int pixels) {
    return path + '\x1f' + std::to_string(icon_bucket(pixels));
}
Pixels bitmap_pixels(HBITMAP bitmap) {
    BITMAP info{};
    if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0)
        return {};
    Pixels result{{}, info.bmWidth, info.bmHeight};
    result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = result.width;
    dib.bmiHeader.biHeight = -result.height;
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    dib.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    int rows = GetDIBits(dc, bitmap, 0, result.height, result.rgba.data(), &dib, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (rows != result.height)
        return {};
    bool has_alpha = false;
    for (size_t i = 0; i < result.rgba.size(); i += 4) {
        std::swap(result.rgba[i], result.rgba[i + 2]);
        auto alpha = result.rgba[i + 3];
        has_alpha |= alpha != 0;
        if (alpha && alpha < 255)
            for (int c = 0; c < 3; ++c)
                result.rgba[i + c] = static_cast<uint8_t>(std::min(255, result.rgba[i + c] * 255 / alpha));
    }
    // Legacy icons lacking alpha are decoded using their native icon mask below.
    return has_alpha ? result : Pixels{};
}
Pixels hicon_pixels(HICON icon, int size) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = size;
    dib.bmiHeader.biHeight = -size;
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &dib, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        DestroyIcon(icon);
        return {};
    }
    auto old = SelectObject(dc, bitmap);
    size_t bytes = static_cast<size_t>(size) * size * 4;
    memset(bits, 0, bytes);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    auto black = std::vector<uint8_t>(static_cast<uint8_t *>(bits), static_cast<uint8_t *>(bits) + bytes);
    memset(bits, 255, bytes);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    auto white = static_cast<const uint8_t *>(bits);
    Pixels result{{}, size, size};
    result.rgba.resize(bytes);
    bool alpha_present = false;
    for (size_t i = 3; i < bytes; i += 4)
        alpha_present |= black[i] != 0;
    for (size_t i = 0; i < bytes; i += 4) {
        int alpha = black[i + 3];
        if (!alpha_present) {
            int difference = 0;
            for (int c = 0; c < 3; ++c)
                difference = std::max(difference, white[i + c] - black[i + c]);
            alpha = std::clamp(255 - difference, 0, 255);
        }
        result.rgba[i + 3] = static_cast<uint8_t>(alpha);
        for (int c = 0; c < 3; ++c)
            result.rgba[i + c] =
                alpha ? static_cast<uint8_t>(std::min(255, black[i + 2 - c] * 255 / alpha)) : 0;
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyIcon(icon);
    return result;
}
Pixels fallback_icon(const std::wstring &path, int size) {
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                        SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS) ||
        !info.hIcon)
        return {};
    return hicon_pixels(info.hIcon, size);
}
void apply_overlay(Pixels &base, int overlay) {
    if (overlay == 0 || base.width != base.height)
        return;
    ComPtr<IImageList> images;
    int image = 0;
    HICON icon = nullptr;
    if (FAILED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&images))) ||
        FAILED(images->GetOverlayImage(overlay, &image)) ||
        FAILED(images->GetIcon(image, ILD_TRANSPARENT, &icon)) || !icon)
        return;
    auto badge = hicon_pixels(icon, base.width);
    if (badge.rgba.size() != base.rgba.size())
        return;
    for (size_t i = 0; i < base.rgba.size(); i += 4) {
        int source_alpha = badge.rgba[i + 3], dest_alpha = base.rgba[i + 3];
        int alpha = source_alpha + dest_alpha * (255 - source_alpha) / 255;
        for (int c = 0; c < 3; ++c)
            base.rgba[i + c] =
                alpha ? static_cast<uint8_t>((badge.rgba[i + c] * source_alpha +
                                              base.rgba[i + c] * dest_alpha * (255 - source_alpha) / 255) /
                                             alpha)
                      : 0;
        base.rgba[i + 3] = static_cast<uint8_t>(alpha);
    }
}
Pixels icon_pixels(const std::string &path, int size, std::string &name) {
    auto native = wide(path);
    SHFILEINFOW info{};
    SHGetFileInfoW(native.c_str(), 0, &info, sizeof(info),
                   SHGFI_DISPLAYNAME | SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_OVERLAYINDEX |
                       SHGFI_ADDOVERLAYS);
    name = utf8(info.szDisplayName);
    int overlay = (static_cast<unsigned int>(info.iIcon) >> 24) & 0xff;
    if (info.hIcon)
        DestroyIcon(info.hIcon);
    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(SHCreateItemFromParsingName(native.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(factory->GetImage(
                {size, size}, static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK), &bitmap))) {
            auto pixels = bitmap_pixels(bitmap);
            DeleteObject(bitmap);
            if (!pixels.rgba.empty()) {
                apply_overlay(pixels, overlay);
                return pixels;
            }
        }
    }
    return fallback_icon(native, size);
}
} // namespace
void IconCache::run() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        Request request;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [this] { return stopping || !requests.empty(); });
            if (stopping)
                break;
            request = std::move(requests.front());
            requests.pop_front();
        }
        std::string name;
        auto pixels = icon_pixels(request.path, request.pixels, name);
        {
            std::lock_guard lock(mutex);
            if (auto found = entries.find(request.key);
                found != entries.end() && request.generation == generation) {
                auto &entry = found->second;
                entry.pixels = std::move(pixels.rgba);
                entry.width = pixels.width;
                entry.height = pixels.height;
                entry.display_name = name;
                entry.ready = true;
                if (!name.empty())
                    names[request.path] = name;
                if (names.size() > 2048)
                    names.erase(names.begin());
            }
        }
        PostMessageW(notify, MSG_ICONS, 0, 0);
    }
    CoUninitialize();
}
ID3D11ShaderResourceView *IconCache::get(const std::string &path, int pixels) {
    std::lock_guard lock(mutex);
    auto &entry = entries[cache_key(path, pixels)];
    entry.touched = ++sequence;
    if (!entry.queued) {
        entry.queued = true;
        entry.path = path;
        requests.push_back({cache_key(path, pixels), path, icon_bucket(pixels), generation});
        condition.notify_one();
    }
    if (entry.ready && !entry.view && !entry.pixels.empty()) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = entry.width;
        desc.Height = entry.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{entry.pixels.data(), static_cast<UINT>(entry.width * 4), 0};
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
            device->CreateShaderResourceView(texture.Get(), nullptr, &entry.view);
        entry.pixels.clear();
        entry.pixels.shrink_to_fit();
    }
    auto result = entry.view.Get();
    if (entries.size() > 512) {
        auto oldest = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->second.path != path && it->second.ready &&
                (oldest == entries.end() || it->second.touched < oldest->second.touched))
                oldest = it;
        if (oldest != entries.end())
            entries.erase(oldest);
    }
    return result;
}
std::string IconCache::display_name(const std::string &path, const std::string &fallback) {
    std::lock_guard lock(mutex);
    auto found = names.find(path);
    return found != names.end() ? found->second : fallback;
}
size_t IconCache::size() {
    std::lock_guard lock(mutex);
    return entries.size();
}
void IconCache::clear() {
    std::lock_guard lock(mutex);
    ++generation;
    entries.clear();
    names.clear();
    requests.clear();
}
} // namespace deskedge
