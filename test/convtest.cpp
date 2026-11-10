// convtest: runs the HDR -> SDR converter (src/converter.cpp + the real shader) on WARP,
// Windows' software GPU, and compares its output with reference_math.h, at the default knee
// and with the roll-off off (knee=1).
// Needs no GPU or HDR monitor, so CI runs it. Exit code 0 = pass, 1 = fail, 2 = setup failed.
#include <windows.h>
#include <d3d11_1.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "../src/converter.h"
#include "../src/log_config.h"
#include "reference_math.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Case {
    const char* name;
    double r, g, b; // linear light, 1.0 = SDR white
};

const Case kCases[] = {
    {"black", 0, 0, 0},
    {"mid grey", 0.5, 0.5, 0.5},
    {"just below the default knee", 0.74, 0.74, 0.74},
    {"SDR white", 1, 1, 1},
    {"HDR highlight", 2, 2, 2},
    {"display peak", 1000.0 / 240, 1000.0 / 240, 1000.0 / 240},
    {"above the peak", 10, 10, 10},
    {"bright orange (hue kept)", 2, 1, 0.25},
    {"out of gamut (negative)", -0.2, 0.5, 0.3},
};
enum { kGrey = 1, kWhite = 3, kPeak = 5, kOrange = 7 }; // rows of kCases used as fixed points

double Expected8(double c, double m, const FixParams& fp)
{
    const double lin = m > 0 ? std::max(c, 0.0) * ExpectedLinear(m, fp) / m : 0.0;
    return 255 * LinearToSrgb(lin);
}

// Converts one pixel per case with the given knee and checks the result.
// Returns the number of failed checks, or -1 if the converter couldn't be set up.
int Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, double knee)
{
    const FixParams fp{240, 1000, knee};
    const_cast<Config&>(Cfg()).knee = float(knee); // where the converter reads hdrfix.ini's knee
    printf("\nknee %.2f\n", knee);

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
        !conv.Init(dev, output, DXGI_FORMAT_B8G8R8A8_UNORM, float(fp.sdrWhite)) ||
        !(converted = conv.Convert(input.Get())))
        return -1;

    converted->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
        return -1;
    ctx->CopyResource(staging.Get(), converted);
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return -1;
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
        printf("%-28s got %3d %3d %3d, want %5.1f %5.1f %5.1f -> %s\n", t.name, got[0], got[1], got[2], want[0],
               want[1], want[2], ok ? "PASS" : "FAIL");
    }

    // Fixed points that don't depend on the reference math.
    const auto red = [&](int i) { return int(px[i * 4 + 2]); };
    const auto green = [&](int i) { return int(px[i * 4 + 1]); };
    const auto blue = [&](int i) { return int(px[i * 4]); };
    bool anchors;
    const char* what;
    if (knee < 1) {
        what = "grey unchanged, white ~240, peak 255";
        anchors = std::abs(green(kGrey) - 255 * LinearToSrgb(0.5)) <= 1 && green(kWhite) >= 230 &&
                  green(kWhite) <= 250 && green(kPeak) == 255;
    } else {
        // No roll-off: white is exact, and an orange twice as bright as white stays orange
        // (2 : 1 : 0.25 becomes 1 : 0.5 : 0.125) instead of clipping to yellow.
        what = "white 255, orange stays 255 188 99";
        anchors = green(kWhite) == 255 && red(kOrange) == 255 && std::abs(green(kOrange) - 188) <= 1 &&
                  std::abs(blue(kOrange) - 99) <= 1;
    }
    printf("fixed points (%s) -> %s\n", what, anchors ? "PASS" : "FAIL");
    failures += !anchors;
    ctx->Unmap(staging.Get(), 0);
    return failures;
}

} // namespace

int main()
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) {
        puts("could not create a WARP device");
        return 2;
    }

    int failures = 0;
    for (const double knee : {0.75, 1.0}) { // Config's default, and roll-off off
        const int failed = Run(dev.Get(), ctx.Get(), knee);
        if (failed < 0) {
            puts("converter setup failed");
            return 2;
        }
        failures += failed;
    }
    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
