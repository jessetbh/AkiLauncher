#pragma once
#include <filesystem>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0;
    int h = 0;
    bool ok() const { return srv != nullptr; }
};

// Loads PNG/JPG via WIC into an immutable D3D11 texture. Returns empty on any
// failure (missing file, decode error) - callers draw a placeholder instead.
Texture load_texture(ID3D11Device* device, const std::filesystem::path& path);
