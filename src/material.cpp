#include "platform.h"
#include <algorithm>
#include <cmath>
#include <wincodec.h>

namespace deskedge {
namespace {
// Sliding box filters approximate a Gaussian in linear time. Work is done once per wallpaper,
// at quarter resolution, never in the drag/render loop. No desktop or application capture.
void blur_pass(std::vector<BYTE> &pixels, int width, int height, int radius, bool horizontal) {
    std::vector<BYTE> output(pixels.size());
    int lines = horizontal ? height : width, length = horizontal ? width : height;
    auto index = [&](int line, int offset, int channel) {
        offset = std::clamp(offset, 0, length - 1);
        return ((horizontal ? line * width + offset : offset * width + line) * 4) + channel;
    };
    for (int line = 0; line < lines; ++line)
        for (int channel = 0; channel < 3; ++channel) {
            int sum = 0;
            for (int i = -radius; i <= radius; ++i)
                sum += pixels[index(line, i, channel)];
            for (int i = 0; i < length; ++i) {
                output[index(line, i, channel)] = static_cast<BYTE>(sum / (2 * radius + 1));
                sum +=
                    pixels[index(line, i + radius + 1, channel)] - pixels[index(line, i - radius, channel)];
            }
        }
    for (size_t i = 3; i < output.size(); i += 4)
        output[i] = 255;
    pixels.swap(output);
}
} // namespace
ID3D11ShaderResourceView *GlassMaterial::grain() {
    if (grain_texture)
        return grain_texture.Get();
    std::vector<BYTE> pixels(128 * 128 * 4);
    uint32_t random = 0xDE5CED6E;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        pixels[i] = pixels[i + 1] = pixels[i + 2] = random & 1 ? 255 : 0;
        pixels[i + 3] = 1;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 128;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 128 * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
        device->CreateShaderResourceView(texture.Get(), nullptr, &grain_texture);
    return grain_texture.Get();
}
ID3D11ShaderResourceView *GlassMaterial::wallpaper(const Monitor &screen) {
    if (auto found = surfaces.find(screen.name); found != surfaces.end())
        return found->second.blurred.Get();
    auto &surface = surfaces[screen.name];
    surface.bounds = screen.bounds;
    ComPtr<IDesktopWallpaper> desktop;
    std::wstring path;
    DESKTOP_WALLPAPER_POSITION placement = DWPOS_FILL;
    COLORREF background = RGB(32, 40, 52);
    RECT canvas = screen.bounds;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&desktop)))) {
        desktop->GetPosition(&placement);
        desktop->GetBackgroundColor(&background);
        UINT count = 0;
        desktop->GetMonitorDevicePathCount(&count);
        for (UINT i = 0; i < count; ++i) {
            PWSTR name = nullptr;
            if (FAILED(desktop->GetMonitorDevicePathAt(i, &name)))
                continue;
            RECT bounds{};
            desktop->GetMonitorRECT(name, &bounds);
            if (EqualRect(&bounds, &screen.bounds)) {
                PWSTR file = nullptr;
                if (SUCCEEDED(desktop->GetWallpaper(name, &file)) && file) {
                    path = file;
                    CoTaskMemFree(file);
                }
            }
            if (placement == DWPOS_SPAN)
                UnionRect(&canvas, &canvas, &bounds);
            CoTaskMemFree(name);
        }
    } else {
        wchar_t file[32768]{};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, 32768, file, 0);
        path = file;
    }
    UINT source_w = 0, source_h = 0, decoded_w = 0, decoded_h = 0;
    std::vector<BYTE> decoded;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (!path.empty() &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&source_w, &source_h)) &&
        source_w && source_h && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter))) {
        float reduction = std::min(1.f, 2048.f / std::max(source_w, source_h));
        decoded_w = std::max(1U, static_cast<UINT>(source_w * reduction));
        decoded_h = std::max(1U, static_cast<UINT>(source_h * reduction));
        if (SUCCEEDED(
                scaler->Initialize(frame.Get(), decoded_w, decoded_h, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA,
                                            WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom))) {
            decoded.resize(static_cast<size_t>(decoded_w) * decoded_h * 4);
            if (FAILED(converter->CopyPixels(nullptr, decoded_w * 4, static_cast<UINT>(decoded.size()),
                                             decoded.data())))
                decoded.clear();
        }
    }
    int width = std::max(1L, (screen.bounds.right - screen.bounds.left + 3) / 4);
    int height = std::max(1L, (screen.bounds.bottom - screen.bounds.top + 3) / 4);
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    double area_w = canvas.right - canvas.left, area_h = canvas.bottom - canvas.top;
    double factor_x = source_w ? area_w / source_w : 1., factor_y = source_h ? area_h / source_h : 1.;
    if (placement == DWPOS_FILL || placement == DWPOS_SPAN)
        factor_x = factor_y = std::max(factor_x, factor_y);
    else if (placement == DWPOS_FIT)
        factor_x = factor_y = std::min(factor_x, factor_y);
    else if (placement == DWPOS_CENTER || placement == DWPOS_TILE)
        factor_x = factor_y = 1.;
    double left = (area_w - source_w * factor_x) / 2, top = (area_h - source_h * factor_y) / 2;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto dst = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            dst[0] = GetRValue(background);
            dst[1] = GetGValue(background);
            dst[2] = GetBValue(background);
            dst[3] = 255;
            if (decoded.empty())
                continue;
            double px = screen.bounds.left - canvas.left +
                        (x + .5) * (screen.bounds.right - screen.bounds.left) / width;
            double py = screen.bounds.top - canvas.top +
                        (y + .5) * (screen.bounds.bottom - screen.bounds.top) / height;
            double sx = (px - left) / factor_x, sy = (py - top) / factor_y;
            if (placement == DWPOS_TILE) {
                sx = std::fmod(px, source_w);
                sy = std::fmod(py, source_h);
            }
            if (sx < 0 || sy < 0 || sx >= source_w || sy >= source_h)
                continue;
            UINT ix = std::min(decoded_w - 1, static_cast<UINT>(sx * decoded_w / source_w));
            UINT iy = std::min(decoded_h - 1, static_cast<UINT>(sy * decoded_h / source_h));
            auto src = decoded.data() + (static_cast<size_t>(iy) * decoded_w + ix) * 4;
            for (int c = 0; c < 3; ++c)
                dst[c] = src[c];
        }
    int radius = std::max(2, static_cast<int>(5 * screen.scale));
    for (int pass = 0; pass < 3; ++pass) {
        blur_pass(pixels, width, height, radius, true);
        blur_pass(pixels, width, height, radius, false);
    }
    for (size_t i = 0; i < pixels.size(); i += 4) {
        float luminance = .2126f * pixels[i] + .7152f * pixels[i + 1] + .0722f * pixels[i + 2];
        for (int c = 0; c < 3; ++c)
            pixels[i + c] =
                static_cast<BYTE>(std::clamp(luminance + 1.5f * (pixels[i + c] - luminance), 0.f, 255.f));
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), static_cast<UINT>(width * 4), 0};
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)))
        device->CreateShaderResourceView(texture.Get(), nullptr, &surface.blurred);
    return surface.blurred.Get();
}
} // namespace deskedge
