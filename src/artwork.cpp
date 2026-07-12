#include "artwork.h"

#include <windows.h>

#include <d3d11.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

#include "launch.h"  // logf

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T*() { return p; }
};

// Decode PNG/JPG to 32bpp RGBA. scaleToW > 0 downscales (aspect kept, Fant).
static bool decode_rgba(const std::filesystem::path& path, UINT scaleToW,
                        std::vector<uint8_t>& pixels, UINT& w, UINT& h) {
    if (!std::filesystem::exists(path)) return false;

    static bool comInit = [] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return true;
    }();
    (void)comInit;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        logf(L"artwork: failed to decode %s", path.c_str());
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return false;

    IWICBitmapSource* src = converter;
    ComPtr<IWICBitmapScaler> scaler;
    UINT sw = 0, sh = 0;
    converter->GetSize(&sw, &sh);
    if (!sw || !sh) return false;
    if (scaleToW > 0 && sw > scaleToW) {
        UINT th = std::max(1u, (UINT)((uint64_t)sh * scaleToW / sw));
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(converter, scaleToW, th,
                                      WICBitmapInterpolationModeFant)))
            return false;
        src = scaler;
        sw = scaleToW;
        sh = th;
    }

    pixels.resize((size_t)sw * sh * 4);
    if (FAILED(src->CopyPixels(nullptr, sw * 4, (UINT)pixels.size(), pixels.data())))
        return false;
    w = sw;
    h = sh;
    return true;
}

static Texture upload_rgba(ID3D11Device* device, const std::vector<uint8_t>& pixels,
                           UINT w, UINT h) {
    Texture tex;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{pixels.data(), w * 4, 0};
    ComPtr<ID3D11Texture2D> d3dTex;
    if (FAILED(device->CreateTexture2D(&td, &sd, &d3dTex))) return tex;

    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device->CreateShaderResourceView(d3dTex, nullptr, &srv))) return tex;

    tex.srv = srv;
    tex.w = (int)w;
    tex.h = (int)h;
    return tex;
}

// Separable box blur, RGBA, radius r. Sized for tiny backdrop bitmaps.
static void box_blur(std::vector<uint8_t>& px, int w, int h, int r) {
    std::vector<uint8_t> tmp(px.size());
    auto pass = [&](const uint8_t* in, uint8_t* out, int outer, int inner,
                    size_t outerStride, size_t innerStride) {
        for (int o = 0; o < outer; ++o) {
            const uint8_t* row = in + o * outerStride;
            uint8_t* orow = out + o * outerStride;
            for (int i = 0; i < inner; ++i) {
                int sum[4] = {0, 0, 0, 0}, cnt = 0;
                for (int k = i - r; k <= i + r; ++k) {
                    if (k < 0 || k >= inner) continue;
                    const uint8_t* p = row + k * innerStride;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                    ++cnt;
                }
                uint8_t* q = orow + i * innerStride;
                for (int c = 0; c < 4; ++c) q[c] = (uint8_t)(sum[c] / cnt);
            }
        }
    };
    pass(px.data(), tmp.data(), h, w, (size_t)w * 4, 4);              // horizontal
    pass(tmp.data(), px.data(), w, h, 4, (size_t)w * 4);              // vertical
}

Texture load_texture(ID3D11Device* device, const std::filesystem::path& path) {
    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    if (!decode_rgba(path, 0, pixels, w, h)) return {};
    return upload_rgba(device, pixels, w, h);
}

Texture load_texture_blurred(ID3D11Device* device, const std::filesystem::path& path) {
    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    if (!decode_rgba(path, 72, pixels, w, h)) return {};
    for (int i = 0; i < 3; ++i) box_blur(pixels, (int)w, (int)h, 3);
    return upload_rgba(device, pixels, w, h);
}
