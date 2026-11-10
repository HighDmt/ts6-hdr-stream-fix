// convtest: runs the HDR -> SDR converter (src/converter.cpp + the real shader) on WARP,
// Windows' software GPU, and compares its output with reference_math.h.
// Needs no GPU or HDR monitor, so CI runs it. Exit code 0 = pass, 1 = fail, 2 = setup failed.
#include <windows.h>
#include <d3d11_1.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "../src/converter.h"
#include "reference_math.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Case {
    const char* name;
    double r, g, b; // linear light, 1.0 = SDR white
};

const Case kCases[] = {
    {"black", 0, 0, 0},
    {"grey below the knee", 0.5, 0.5, 0.5},
    {"just below the knee", 0.74, 0.74, 0.74},
    {"SDR white", 1, 1, 1},
    {"HDR highlight", 2, 2, 2},
    {"display peak", 1000.0 / 240, 1000.0 / 240, 1000.0 / 240},
    {"above the peak", 10, 10, 10},
    {"bright orange (hue kept)", 2, 1, 0.25},
    {"out of gamut (negative)", -0.2, 0.5, 0.3},
};

double Expected8(double c, double m, const FixParams& fp)
{
    const double lin = m > 0 ? std::max(c, 0.0) * ExpectedLinear(m, fp) / m : 0.0;
    return 255 * LinearToSrgb(lin);
}

} // namespace

int main()
{
    const FixParams fp{240, 1000, 0.75}; // knee 0.75 is Config's default
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        puts("could not create a WARP device");
        return 2;
    }

    // One pixel per case, in scRGB (1.0 = 80 nits) like the HDR desktop.
    const UINT n = static_cast<UINT>(std::size(kCases));
    DirectX::PackedVector::HALF src[std::size(kCases)][4];
    for (UINT i = 0; i < n; ++i) {
        const double c[3] = {kCases[i].r, kCases[i].g, kCases[i].b};
        for (int k = 0; k < 3; ++k)
            src[i][k] = DirectX::PackedVector::XMConvertFloatToHalf(float(c[k] * fp.sdrWhite / 80));
        src[i][3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = n;
    d.Height = 1;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{src, sizeof(src)};
    ComPtr<ID3D11Texture2D> input;
    DXGI_OUTPUT_DESC1 output{};
    output.MaxLuminance = float(fp.peak);
    Converter conv;
    ID3D11Texture2D* converted = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, &init, &input)) ||
        !conv.Init(dev.Get(), output, DXGI_FORMAT_B8G8R8A8_UNORM, float(fp.sdrWhite)) ||
        !(converted = conv.Convert(input.Get()))) {
        puts("converter setup failed");
        return 2;
    }

    converted->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
        return 2;
    ctx->CopyResource(staging.Get(), converted);
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return 2;
    const auto* px = static_cast<const unsigned char*>(map.pData); // BGRA

    int failures = 0;
    for (UINT i = 0; i < n; ++i) {
        const Case& t = kCases[i];
        const double m = std::max({t.r, t.g, t.b});
        const double want[3] = {Expected8(t.r, m, fp), Expected8(t.g, m, fp), Expected8(t.b, m, fp)};
        const int got[3] = {px[i * 4 + 2], px[i * 4 + 1], px[i * 4]};
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            ok &= std::abs(got[k] - want[k]) <= 1.0;
        failures += !ok;
        printf("%-26s got %3d %3d %3d, want %5.1f %5.1f %5.1f -> %s\n", t.name, got[0], got[1], got[2], want[0],
               want[1], want[2], ok ? "PASS" : "FAIL");
    }

    // Fixed points that don't depend on the reference math: below the knee nothing changes,
    // SDR white comes out slightly dimmed (about 240), the display peak is full white.
    const auto at = [&](int i) { return int(px[i * 4 + 1]); };
    const bool anchors = std::abs(at(1) - 255 * LinearToSrgb(0.5)) <= 1 && at(3) >= 230 && at(3) <= 250 &&
                         at(5) == 255;
    printf("fixed points (grey unchanged, white ~240, peak 255) -> %s\n", anchors ? "PASS" : "FAIL");
    failures += !anchors;

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
