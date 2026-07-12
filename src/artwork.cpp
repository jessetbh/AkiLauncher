#include "artwork.h"

#include <windows.h>

#include <d3d11.h>
#include <wincodec.h>

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

Texture load_texture(ID3D11Device* device, const std::filesystem::path& path) {
    Texture tex;
    if (!std::filesystem::exists(path)) return tex;

    static bool comInit = [] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return true;
    }();
    (void)comInit;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return tex;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        logf(L"artwork: failed to decode %s", path.c_str());
        return tex;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return tex;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return tex;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return tex;

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (!w || !h) return tex;

    std::vector<uint8_t> pixels((size_t)w * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data())))
        return tex;

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
