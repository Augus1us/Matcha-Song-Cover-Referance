#include "media.h"
#include "music_player_host.h"
#include "music_player_internal.h"

#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace native_music_player::detail {

namespace {

ID3D11ShaderResourceView* s_artSrv = nullptr;
ID3D11Texture2D*          s_artTex = nullptr;
ID3D11ShaderResourceView* s_atmosphereSrv = nullptr;
ID3D11Texture2D*          s_atmosphereTex = nullptr;
float                     s_atmosphereFade = 1.f;
int s_artW = 0, s_artH = 0;

void ReleaseArtTexture() {
    if (s_artSrv) { s_artSrv->Release(); s_artSrv = nullptr; }
    if (s_artTex) { s_artTex->Release(); s_artTex = nullptr; }
}

void ReleaseAtmosphereTexture() {
    if (s_atmosphereSrv) { s_atmosphereSrv->Release(); s_atmosphereSrv = nullptr; }
    if (s_atmosphereTex) { s_atmosphereTex->Release(); s_atmosphereTex = nullptr; }
}

bool UploadArtTexture(ID3D11Device* dev, const media::NowPlaying& np) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = np.artW; td.Height = np.artH;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = np.artBgra;
    sd.SysMemPitch = np.artW * 4;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &s_artTex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(s_artTex, &svd, &s_artSrv);
    s_artW = np.artW;
    s_artH = np.artH;
    return true;
}

void BuildAtmosphereTexture(ID3D11Device* dev, const media::NowPlaying& np) {
    constexpr int kRegionGrid = 4;
    float regions[kRegionGrid * kRegionGrid * 3] = {};
    SamplePaletteRegionsBGRA(np.artBgra, np.artW, np.artH, kRegionGrid, regions);

    constexpr int kAtmosphereSide = 256;
    std::vector<uint8_t> atmosphere(
        (size_t)kAtmosphereSide * kAtmosphereSide * 4u);

    auto region = [&](int x, int y) {
        const float* p = regions + (y * kRegionGrid + x) * 3;
        return std::array<float, 3>{ p[0], p[1], p[2] };
    };
    auto mix = [](std::array<float, 3> a, std::array<float, 3> b, float t) {
        return std::array<float, 3>{
            a[0] + (b[0] - a[0]) * t,
            a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t
        };
    };

    for (int y = 0; y < kAtmosphereSide; ++y) {
        const float gy = (float)y / (kAtmosphereSide - 1) * (kRegionGrid - 1);
        const int y0 = (int)std::floor(gy);
        const int y1 = std::min(y0 + 1, kRegionGrid - 1);
        const float ty = gy - y0;
        for (int x = 0; x < kAtmosphereSide; ++x) {
            const float gx = (float)x / (kAtmosphereSide - 1) * (kRegionGrid - 1);
            const int x0 = (int)std::floor(gx);
            const int x1 = std::min(x0 + 1, kRegionGrid - 1);
            const float tx = gx - x0;
            auto top = mix(region(x0, y0), region(x1, y0), tx);
            auto bottom = mix(region(x0, y1), region(x1, y1), tx);
            auto c = mix(top, bottom, ty);
            uint8_t* out = atmosphere.data() +
                ((size_t)y * kAtmosphereSide + x) * 4u;
            out[0] = (uint8_t)std::clamp(c[2] * 255.f, 0.f, 255.f);
            out[1] = (uint8_t)std::clamp(c[1] * 255.f, 0.f, 255.f);
            out[2] = (uint8_t)std::clamp(c[0] * 255.f, 0.f, 255.f);
            out[3] = 255;
        }
    }

    std::vector<uint8_t> scratch(atmosphere.size());
    auto blurAxis = [&](bool horizontal, int radius) {
        for (int y = 0; y < kAtmosphereSide; ++y) {
            for (int x = 0; x < kAtmosphereSide; ++x) {
                uint32_t sums[4] = {};
                int samples = 0;
                for (int off = -radius; off <= radius; ++off) {
                    int sx = horizontal
                        ? std::clamp(x + off, 0, kAtmosphereSide - 1) : x;
                    int sy = horizontal
                        ? y : std::clamp(y + off, 0, kAtmosphereSide - 1);
                    const uint8_t* sample = atmosphere.data() +
                        ((size_t)sy * kAtmosphereSide + sx) * 4u;
                    for (int ch = 0; ch < 4; ++ch) sums[ch] += sample[ch];
                    ++samples;
                }
                uint8_t* dest = scratch.data() +
                    ((size_t)y * kAtmosphereSide + x) * 4u;
                for (int ch = 0; ch < 4; ++ch)
                    dest[ch] = (uint8_t)(sums[ch] / samples);
            }
        }
        atmosphere.swap(scratch);
    };
    for (int pass = 0; pass < 3; ++pass) {
        blurAxis(true, 24);
        blurAxis(false, 24);
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kAtmosphereSide;
    td.Height = kAtmosphereSide;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = atmosphere.data();
    sd.SysMemPitch = kAtmosphereSide * 4;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &s_atmosphereTex))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
        svd.Format = td.Format;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(s_atmosphereTex, &svd, &s_atmosphereSrv);
        s_atmosphereFade = 0.f;
    }
}

}  // namespace

void UpdateAlbumArt(const media::NowPlaying& np) {
    if (!np.artBgra || np.artW <= 0 || np.artH <= 0) return;
    if (np.artDirty) SetPaletteFromArt(np.artBgra, np.artW, np.artH);
    bool sameSize = (s_artW == np.artW && s_artH == np.artH);
    if (!np.artDirty && s_artSrv && sameSize) return;
    auto* dev = (ID3D11Device*)music_host::overlay::GetD3DDevice();
    if (!dev) return;
    ReleaseArtTexture();
    ReleaseAtmosphereTexture();
    if (!UploadArtTexture(dev, np)) return;
    BuildAtmosphereTexture(dev, np);
    const_cast<media::NowPlaying&>(np).artDirty = false;
}

bool HasAlbumArt() {
    return s_artSrv != nullptr;
}

ImTextureID AlbumArtTexture() {
    return (ImTextureID)(uintptr_t)s_artSrv;
}

ImTextureID AlbumAtmosphereTexture() {
    return (ImTextureID)(uintptr_t)s_atmosphereSrv;
}

float& AlbumAtmosphereFadeRef() {
    return s_atmosphereFade;
}

void ReleaseVisualAssets() {
    ReleaseArtTexture();
    ReleaseAtmosphereTexture();
    s_artW = s_artH = 0;
    s_atmosphereFade = 1.f;
    ResetPaletteState();
}

}  // namespace native_music_player::detail
