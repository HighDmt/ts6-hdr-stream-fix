// hdrtest: checks HDR desktop duplication with and without the hdrfix DLL.
//
// 1. Shows a row of grey patches (sRGB 255, 235, 192, 128, 64) on the HDR monitor.
// 2. Captures it with DuplicateOutput, DuplicateOutput1(BGRA8) and DuplicateOutput1(FP16),
//    first as-is, then after loading the fix DLL (which hooks those calls in this process).
// 3. Prints the measured patch values next to what they should be, saves each frame as PNG.
//
// While it runs (~10 s) it shows test windows on top and nudges the mouse cursor by 1 px.
//
// Usage: hdrtest [--dll <path to built version.dll>] [--out <folder>]
// Exit code: 0 = all checks passed, 1 = a check failed, 2 = no HDR monitor / no GPU.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/display_info.h"
#include "reference_math.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr int kPatchValues[] = {255, 235, 192, 128, 64};
constexpr int kPatchCount = static_cast<int>(std::size(kPatchValues));
constexpr int kPatchSize = 200;
constexpr int kMeasureInset = 50; // measure the middle of each patch only
HWND g_patchWindow = nullptr;

// ---- Patch window --------------------------------------------------------------

LRESULT CALLBACK PatchWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        for (int i = 0; i < kPatchCount; ++i) {
            const RECT r{i * kPatchSize, 0, (i + 1) * kPatchSize, kPatchSize};
            HBRUSH b = CreateSolidBrush(RGB(kPatchValues[i], kPatchValues[i], kPatchValues[i]));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PumpMessages(DWORD ms)
{
    const ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    } while (GetTickCount64() < end);
}

// Shows the patches centered on the monitor; returns the top-left in desktop coordinates.
HWND ShowPatches(const RECT& monitor, POINT& topLeft)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = PatchWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hdrtest_patches";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int w = kPatchCount * kPatchSize, h = kPatchSize;
    topLeft = {(monitor.left + monitor.right - w) / 2, (monitor.top + monitor.bottom - h) / 2};
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"hdrtest", WS_POPUP,
                                topLeft.x, topLeft.y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    PumpMessages(700); // let DWM compose it
    return hwnd;
}

// ---- Capture ---------------------------------------------------------------------

struct Frame {
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // of the texture we got
    DXGI_FORMAT reportedFormat = DXGI_FORMAT_UNKNOWN; // what GetDesc claims
    std::vector<uint8_t> pixels;
    UINT pitch = 0;
};

// Reads pixel (x, y) as linear-ish floats: BGRA8 -> 0..255 per channel, FP16 -> raw value.
void ReadPixel(const Frame& f, UINT x, UINT y, double rgb[3])
{
    const uint8_t* row = f.pixels.data() + size_t(y) * f.pitch;
    if (f.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        const uint8_t* p = row + x * 4;
        rgb[0] = p[2], rgb[1] = p[1], rgb[2] = p[0];
    } else {
        const auto* p = reinterpret_cast<const DirectX::PackedVector::HALF*>(row + x * 8);
        for (int c = 0; c < 3; ++c)
            rgb[c] = DirectX::PackedVector::XMConvertHalfToFloat(p[c]);
    }
}

// Average of R,G,B over the middle of each patch.
std::vector<double> MeasurePatches(const Frame& f, POINT origin)
{
    std::vector<double> out;
    for (int i = 0; i < kPatchCount; ++i) {
        double sum = 0;
        int n = 0;
        for (int y = kMeasureInset; y < kPatchSize - kMeasureInset; ++y)
            for (int x = i * kPatchSize + kMeasureInset; x < (i + 1) * kPatchSize - kMeasureInset; ++x) {
                const LONG px = origin.x + x, py = origin.y + y;
                if (px < 0 || py < 0 || UINT(px) >= f.width || UINT(py) >= f.height)
                    continue;
                double rgb[3];
                ReadPixel(f, px, py, rgb);
                sum += (rgb[0] + rgb[1] + rgb[2]) / 3;
                ++n;
            }
        out.push_back(n ? sum / n : NAN);
    }
    return out;
}

bool CopyToCpu(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIResource* res, Frame& f)
{
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))))
        return false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
        return false;
    ctx->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    f.width = d.Width;
    f.height = d.Height;
    f.format = d.Format;
    f.pitch = m.RowPitch;
    f.pixels.assign(static_cast<uint8_t*>(m.pData), static_cast<uint8_t*>(m.pData) + size_t(m.RowPitch) * d.Height);
    ctx->Unmap(staging.Get(), 0);
    return true;
}

struct CaptureResult {
    bool ok = false;
    HRESULT hr = S_OK;
    Frame frame;
    int pointerOnlyFrames = 0;      // frames with only a cursor update
    bool pointerOnlyFramesOk = true; // each still returned a full, correct desktop image
};

// Captures one frame. Then wiggles the cursor by 1 px to produce pointer-only updates
// and checks that those still come with a valid desktop texture.
CaptureResult Capture(IDXGIOutput5* output, ID3D11Device* dev, bool legacy, DXGI_FORMAT fmt, POINT origin)
{
    CaptureResult r;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    ComPtr<IDXGIOutputDuplication> dup;
    r.hr = legacy ? output->DuplicateOutput(dev, &dup) : output->DuplicateOutput1(dev, 0, 1, &fmt, &dup);
    if (FAILED(r.hr))
        return r;
    DXGI_OUTDUPL_DESC dd;
    dup->GetDesc(&dd);

    // The first frame of a new duplication can be an empty (black) surface. Repaint the
    // patch window so the desktop presents, and wait for a frame with a real update.
    for (int attempt = 0; attempt < 20 && !r.ok; ++attempt) {
        InvalidateRect(g_patchWindow, nullptr, FALSE);
        PumpMessages(30);
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> res;
        r.hr = dup->AcquireNextFrame(250, &info, &res);
        if (r.hr == DXGI_ERROR_WAIT_TIMEOUT)
            continue;
        if (FAILED(r.hr))
            return r;
        if (info.LastPresentTime.QuadPart != 0)
            r.ok = res && CopyToCpu(dev, ctx.Get(), res.Get(), r.frame);
        res.Reset();
        dup->ReleaseFrame();
    }
    r.frame.reportedFormat = dd.ModeDesc.Format;
    if (!r.ok)
        return r;

    const std::vector<double> reference = MeasurePatches(r.frame, origin);
    POINT cursor;
    GetCursorPos(&cursor);
    for (int i = 0; i < 12; ++i) {
        SetCursorPos(cursor.x + (i % 2), cursor.y);
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> res;
        if (FAILED(dup->AcquireNextFrame(50, &info, &res)))
            continue;
        if (info.LastPresentTime.QuadPart == 0 && info.LastMouseUpdateTime.QuadPart != 0) {
            ++r.pointerOnlyFrames;
            Frame f;
            const bool same = res && CopyToCpu(dev, ctx.Get(), res.Get(), f) && f.format == r.frame.format &&
                              std::abs(MeasurePatches(f, origin)[0] - reference[0]) < 1e-3;
            r.pointerOnlyFramesOk &= same;
        }
        res.Reset();
        dup->ReleaseFrame();
    }
    SetCursorPos(cursor.x, cursor.y);
    return r;
}

// ---- PNG -------------------------------------------------------------------------

bool SavePng(const fs::path& path, const Frame& f)
{
    // FP16 is shown the way a naive SDR consumer would: clip at 1.0, sRGB-encode.
    std::vector<uint8_t> bgra(size_t(f.width) * f.height * 4);
    for (UINT y = 0; y < f.height; ++y)
        for (UINT x = 0; x < f.width; ++x) {
            uint8_t* o = &bgra[(size_t(y) * f.width + x) * 4];
            double rgb[3];
            ReadPixel(f, x, y, rgb);
            for (int c = 0; c < 3; ++c) {
                const double v = f.format == DXGI_FORMAT_B8G8R8A8_UNORM ? rgb[c] : 255 * LinearToSrgb(rgb[c]);
                o[2 - c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
            }
            o[3] = 255;
        }

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
           SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(f.width, f.height)) && SUCCEEDED(frame->SetPixelFormat(&pf)) &&
           pf == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(frame->WritePixels(f.height, f.width * 4, UINT(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

// ---- Reporting ---------------------------------------------------------------------

const char* FormatName(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "FP16";
    default: return "?";
    }
}

struct Variant {
    const char* name;
    const char* file;
    bool legacy;
    DXGI_FORMAT format;
};
constexpr Variant kVariants[] = {
    {"DuplicateOutput", "legacy_bgra8", true, DXGI_FORMAT_UNKNOWN},
    {"DuplicateOutput1(BGRA8)", "dup1_bgra8", false, DXGI_FORMAT_B8G8R8A8_UNORM},
    {"DuplicateOutput1(FP16)", "dup1_fp16", false, DXGI_FORMAT_R16G16B16A16_FLOAT},
};

// Runs all variants. withFix: compare against the fix's expected output.
// Returns the number of checks that failed.
int RunVariants(IDXGIOutput5* output, ID3D11Device* dev, POINT origin, bool withFix, const FixParams& fp,
                const fs::path& outDir)
{
    int failures = 0;
    printf("\n=== %s ===\n", withFix ? "WITH hdrfix" : "WITHOUT hdrfix");
    printf("  on-screen sRGB value:          ");
    for (int v : kPatchValues)
        printf("%9d", v);
    printf("\n");

    for (const Variant& v : kVariants) {
        const CaptureResult r = Capture(output, dev, v.legacy, v.format, origin);
        if (!r.ok) {
            printf("  %-26s FAILED, hr=0x%08lX\n", v.name, static_cast<unsigned long>(r.hr));
            if (withFix)
                ++failures;
            continue;
        }
        const Frame& f = r.frame;
        const std::vector<double> m = MeasurePatches(f, origin);
        const bool fp16 = f.format == DXGI_FORMAT_R16G16B16A16_FLOAT;

        printf("  %-26s got %s (GetDesc says %s), %ux%u\n", v.name, FormatName(f.format),
               FormatName(f.reportedFormat), f.width, f.height);
        printf("    measured:                    ");
        for (double x : m)
            printf(fp16 ? "%9.3f" : "%9.1f", x);
        printf(fp16 ? "   (linear, raw value)\n" : "\n");

        if (fp16) {
            printf("    as SDR if clipped at 1.0:    ");
            for (double x : m)
                printf("%9.1f", 255 * LinearToSrgb(x));
            printf("\n");
        }

        // Expected: without the fix, FP16 is the raw scRGB value; with it, the converted value.
        printf("    expected:                    ");
        bool pass = true;
        for (int i = 0; i < kPatchCount; ++i) {
            const double lin = SrgbToLinear(kPatchValues[i] / 255.0);
            double e;
            if (!withFix)
                e = fp16 ? lin * fp.sdrWhite / 80 : NAN; // no defined target for the OS's own BGRA8 conversion
            else
                e = fp16 ? ExpectedLinear(lin, fp) : 255 * LinearToSrgb(ExpectedLinear(lin, fp));
            const double tol = fp16 ? std::max(0.01, 0.02 * e) : 2.5;
            if (std::isnan(e))
                printf("%9s", "-");
            else
                printf(fp16 ? "%9.3f" : "%9.1f", e);
            if (withFix && !(std::abs(m[i] - e) <= tol))
                pass = false;
        }
        printf("\n");
        if (withFix && (f.format != (v.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? v.format : DXGI_FORMAT_B8G8R8A8_UNORM) ||
                        f.reportedFormat != f.format))
            pass = false;
        if (r.pointerOnlyFrames)
            printf("    pointer-only frames: %d, all returned the full converted desktop: %s\n", r.pointerOnlyFrames,
                   r.pointerOnlyFramesOk ? "yes" : "NO");
        if (withFix) {
            pass &= r.pointerOnlyFramesOk;
            printf("    -> %s\n", pass ? "PASS" : "FAIL");
            failures += pass ? 0 : 1;
        }

        const fs::path png = outDir / (std::string(withFix ? "fix_" : "nofix_") + v.file + ".png");
        printf("    saved %s\n", SavePng(png, f) ? png.string().c_str() : "(PNG save failed)");
    }
    return failures;
}

// ---- App (single window) sharing ------------------------------------------------------
//
// A window rendered the way Chrome renders with HDR on: an scRGB FP16 flip-model swap
// chain, SDR content at the SDR white level. Captured like TeamSpeak's GDI window
// capturer: a 32-bpp DIB of GetWindowRect's size, filled by PrintWindow.

struct HdrWindow {
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain1> swapChain;
};

HdrWindow ShowHdrWindow(ID3D11Device* dev, const RECT& monitor, double sdrWhite)
{
    HdrWindow w;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hdrtest_hdr_window";
    RegisterClassW(&wc);

    // Patches in the top half, black in the bottom half: an upside-down capture can't pass.
    RECT r{0, 0, kPatchCount * kPatchSize, 2 * kPatchSize};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOPMOST);
    const int x = (monitor.left + monitor.right - (r.right - r.left)) / 2;
    const int y = (monitor.top + monitor.bottom) / 2 + kPatchSize; // below the patch row
    w.hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"hdrtest HDR window", WS_OVERLAPPEDWINDOW, x, y,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(w.hwnd, SW_SHOWNOACTIVATE);

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    dev->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = kPatchCount * kPatchSize;
    sd.Height = 2 * kPatchSize;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(dev, w.hwnd, &sd, nullptr, nullptr, &w.swapChain)))
        return w;
    ComPtr<IDXGISwapChain3> sc3;
    w.swapChain.As(&sc3);
    sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    dev->GetImmediateContext(&ctx);
    ctx.As(&ctx1);
    for (int frame = 0; frame < 3; ++frame) {
        ComPtr<ID3D11Texture2D> back;
        ComPtr<ID3D11RenderTargetView> rtv;
        w.swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        const float black[4] = {0, 0, 0, 1};
        ctx1->ClearView(rtv.Get(), black, nullptr, 0);
        for (int i = 0; i < kPatchCount; ++i) {
            const float c = float(SrgbToLinear(kPatchValues[i] / 255.0) * sdrWhite / 80);
            const float color[4] = {c, c, c, 1};
            const D3D11_RECT rect{i * kPatchSize, 0, (i + 1) * kPatchSize, kPatchSize};
            ctx1->ClearView(rtv.Get(), color, &rect, 1);
        }
        w.swapChain->Present(1, 0);
        PumpMessages(50);
    }
    PumpMessages(300);
    return w;
}

// Captures the window like WebRTC's GDI window capturer, several times in a row (a stream).
// callMs: median time of the GDI call once the stream is running (the last 5 captures).
bool CaptureWindowGdi(HWND hwnd, bool bottomUp, Frame& out, POINT& clientOffset, double& callMs)
{
    bool ok = false;
    std::vector<double> times;
    for (int i = 0; i < 10; ++i) {
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = bottomUp ? h : -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC windowDc = GetWindowDC(hwnd);
        HDC mem = CreateCompatibleDC(windowDc);
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(windowDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceCounter(&t0);
        ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
        GdiFlush();
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        if (i >= 5)
            times.push_back(1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
        if (ok) {
            out.width = w;
            out.height = h;
            out.format = out.reportedFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            out.pitch = w * 4;
            out.pixels.resize(size_t(w) * h * 4);
            for (int row = 0; row < h; ++row) // store top-down
                memcpy(&out.pixels[size_t(row) * w * 4],
                       static_cast<uint8_t*>(bits) + size_t(bottomUp ? h - 1 - row : row) * w * 4, size_t(w) * 4);
            POINT client{0, 0};
            ClientToScreen(hwnd, &client);
            clientOffset = {client.x - wr.left, client.y - wr.top};
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(hwnd, windowDc);
        PumpMessages(33);
    }
    std::sort(times.begin(), times.end());
    callMs = times[times.size() / 2];
    return ok;
}

int RunWindowTest(HWND hwnd, bool withFix, const FixParams& fp, const fs::path& outDir)
{
    int failures = 0;
    printf("\n  App sharing (HDR-rendered window, like Chrome), %s hdrfix:\n", withFix ? "WITH" : "WITHOUT");
    struct Case {
        const char* name;
        const char* file;
        bool bottomUp;
    };
    for (const Case c : {Case{"PrintWindow, top-down DIB", "app_printwindow_topdown", false},
                         Case{"PrintWindow, bottom-up DIB", "app_printwindow_bottomup", true}}) {
        const char* name = c.name;
        Frame f;
        POINT origin{};
        double callMs = 0;
        if (!CaptureWindowGdi(hwnd, c.bottomUp, f, origin, callMs)) {
            printf("  %-26s FAILED\n", name);
            failures += withFix ? 1 : 0;
            continue;
        }
        const std::vector<double> m = MeasurePatches(f, origin);
        printf("  %-28s measured:    ", name);
        for (double x : m)
            printf("%9.1f", x);
        printf("   (%.1f ms per capture)\n", callMs);
        if (withFix) {
            bool pass = true;
            printf("  %-28s expected:    ", "");
            for (int i = 0; i < kPatchCount; ++i) {
                const double e = 255 * LinearToSrgb(ExpectedLinear(SrgbToLinear(kPatchValues[i] / 255.0), fp));
                printf("%9.1f", e);
                pass &= std::abs(m[i] - e) <= 2.5;
            }
            printf("\n    -> %s\n", pass ? "PASS" : "FAIL");
            failures += pass ? 0 : 1;
        }
        const fs::path png = outDir / (std::string(withFix ? "fix_" : "nofix_") + c.file + ".png");
        SavePng(png, f);
    }
    return failures;
}

float IniFloat(const fs::path& ini, const wchar_t* key, float fallback)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"hdrfix", key, L"", buf, 64, ini.c_str());
    return buf[0] ? static_cast<float>(_wtof(buf)) : fallback;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Default DLL: ..\version.dll (release zip layout) or ..\dll\version.dll (build tree).
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const fs::path parent = fs::path(exePath).parent_path().parent_path();
    fs::path dllPath = fs::exists(parent / L"version.dll") ? parent / L"version.dll" : parent / L"dll" / L"version.dll";
    fs::path outDir = fs::current_path() / L"hdrtest_out";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--dll"))
            dllPath = argv[++i];
        else if (!wcscmp(argv[i], L"--out"))
            outDir = argv[++i];
    }
    fs::create_directories(outDir);

    // Find the HDR monitor.
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> hdrAdapter;
    ComPtr<IDXGIOutput6> hdrOutput;
    DXGI_OUTPUT_DESC1 od{};
    ComPtr<IDXGIAdapter1> adapter;
    printf("Outputs:\n");
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            ComPtr<IDXGIOutput6> o6;
            DXGI_OUTPUT_DESC1 d{};
            if (FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&d)))
                continue;
            const bool hdr = IsHdrColorSpace(d.ColorSpace); // same test as the DLL
            printf("  %ls  %ldx%ld  %s  peak %.0f nits  SDR white %.0f nits\n", d.DeviceName,
                   d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                   d.DesktopCoordinates.bottom - d.DesktopCoordinates.top, hdr ? "HDR" : "SDR", d.MaxLuminance,
                   QuerySdrWhiteNits(d.DeviceName));
            if (hdr && !hdrOutput) {
                hdrOutput = o6;
                hdrAdapter = adapter;
                od = d;
            }
        }
    }
    if (!hdrOutput) {
        printf("No HDR output found. Turn on HDR in Windows display settings and run again.\n");
        return 2;
    }
    if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        printf("Warning: the HDR monitor is rotated; patch positions will be wrong.\n");

    // Reference parameters for the fix: same ini the DLL reads.
    const fs::path ini = dllPath.parent_path() / L"hdrfix.ini";
    FixParams fp;
    fp.sdrWhite = IniFloat(ini, L"sdr_white_override_nits", 0);
    if (fp.sdrWhite <= 0)
        fp.sdrWhite = QuerySdrWhiteNits(od.DeviceName);
    fp.peak = IniFloat(ini, L"peak_override_nits", 0);
    if (fp.peak <= 0)
        fp.peak = od.MaxLuminance > 0 ? od.MaxLuminance : 1000;
    fp.knee = std::clamp(IniFloat(ini, L"knee", 0.75f), 0.0f, 1.0f);
    printf("\nTesting %ls: SDR white %.0f nits (scRGB %.3f), peak %.0f nits, knee %.2f\n", od.DeviceName, fp.sdrWhite,
           fp.sdrWhite / 80, fp.peak, fp.knee);

    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(hdrAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) {
        printf("D3D11CreateDevice failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    ComPtr<IDXGIOutput5> output5;
    hdrOutput.As(&output5);

    POINT patchTopLeft;
    HWND patches = g_patchWindow = ShowPatches(od.DesktopCoordinates, patchTopLeft);
    const POINT origin{patchTopLeft.x - od.DesktopCoordinates.left, patchTopLeft.y - od.DesktopCoordinates.top};

    const HdrWindow hdrWindow = ShowHdrWindow(device.Get(), od.DesktopCoordinates, fp.sdrWhite);

    RunVariants(output5.Get(), device.Get(), origin, false, fp, outDir);
    if (hdrWindow.swapChain)
        RunWindowTest(hdrWindow.hwnd, false, fp, outDir);
    PumpMessages(200);

    printf("\nLoading %ls\n", dllPath.c_str());
    HMODULE fix = LoadLibraryW(dllPath.c_str());
    using WaitFn = int(WINAPI*)(DWORD);
    auto wait = fix ? reinterpret_cast<WaitFn>(GetProcAddress(fix, "HdrFix_WaitReady")) : nullptr;
    if (!wait || !wait(5000)) {
        printf("The fix DLL did not load or did not install its hooks (see hdrfix.log next to it).\n");
        DestroyWindow(patches);
        return 1;
    }
    int failures = RunVariants(output5.Get(), device.Get(), origin, true, fp, outDir);
    if (hdrWindow.swapChain)
        failures += RunWindowTest(hdrWindow.hwnd, true, fp, outDir);
    else
        printf("\n  App sharing test skipped: could not create an FP16 swap chain window.\n");
    DestroyWindow(hdrWindow.hwnd);
    DestroyWindow(patches);

    // The proxy must still behave as version.dll: read kernel32's version through it.
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto getSize = reinterpret_cast<SizeFn>(GetProcAddress(fix, "GetFileVersionInfoSizeW"));
    auto getInfo = reinterpret_cast<InfoFn>(GetProcAddress(fix, "GetFileVersionInfoW"));
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(fix, "VerQueryValueW"));
    DWORD handle = 0;
    const DWORD size = getSize ? getSize(L"kernel32.dll", &handle) : 0;
    std::vector<uint8_t> info(size);
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    const bool versionOk = size && getInfo(L"kernel32.dll", 0, size, info.data()) &&
                           query(info.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi;
    if (versionOk)
        printf("\nversion.dll forwarding: kernel32.dll is %u.%u.%u.%u -> PASS\n", HIWORD(ffi->dwFileVersionMS),
               LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    else
        printf("\nversion.dll forwarding -> FAIL\n");
    failures += versionOk ? 0 : 1;

    printf("\n%s (%d failed check%s). Log: %ls\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failures,
           failures == 1 ? "" : "s", (dllPath.parent_path() / L"hdrfix.log").c_str());
    return failures ? 1 : 0;
}
