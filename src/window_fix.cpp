#include "window_fix.h"

#include "converter.h"
#include "log_config.h"
#include "verbose_log.h"

#include <MinHook.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kActivateAfterCalls = 3;           // captures within kActivateWindowMs that mean "stream"
constexpr ULONGLONG kActivateWindowMs = 2000;
constexpr ULONGLONG kIdleCloseMs = 3000;         // stop our capture once TeamSpeak stops
constexpr ULONGLONG kMonitorCacheMs = 2000;
constexpr ULONGLONG kFirstFrameWaitMs = 250;
constexpr ULONGLONG kRetryMs = 5000;             // after a failure, try again this much later
constexpr auto kFormat = wgd::DirectXPixelFormat::R16G16B16A16Float;

std::wstring ClassOf(HWND hwnd)
{
    wchar_t cls[128] = L"?";
    GetClassNameW(hwnd, cls, 128);
    return cls;
}

// Verbose log only: counts an event for a window (class + handle, never its title).
void Verbose(const char* what, HWND hwnd, const char* detail = nullptr)
{
    if (!VerboseLogOn())
        return;
    char key[200];
    snprintf(key, sizeof(key), "%s %ls %p", what, ClassOf(hwnd).c_str(), static_cast<void*>(hwnd));
    VerboseCount(key, detail);
}

// ---- Monitors --------------------------------------------------------------------

struct MonitorState {
    ULONGLONG checked = 0;
    bool hdr = false;
    DXGI_OUTPUT_DESC1 desc{};
};

bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE cs)
{
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

// Finds the DXGI output for a monitor and whether it's in HDR mode (cached briefly).
const MonitorState& LookupMonitor(HMONITOR monitor)
{
    static auto& cache = *new std::map<HMONITOR, MonitorState>; // guarded by g_lock; never destroyed
    MonitorState& m = cache[monitor];
    const ULONGLONG now = GetTickCount64();
    if (m.checked && now - m.checked < kMonitorCacheMs)
        return m;
    m = MonitorState{now};

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    ComPtr<IDXGIFactory1> factory;
    if (!GetMonitorInfoW(monitor, &mi) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return m;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&d)) &&
                _wcsicmp(d.DeviceName, mi.szDevice) == 0) {
                m.hdr = IsHdrColorSpace(d.ColorSpace);
                m.desc = d;
                return m;
            }
        }
    }
    return m;
}

// ---- Our own D3D11 device + WGC sessions ----------------------------------------------
//
// Global state lives on the heap and is never destroyed. The DLL is pinned, so the only
// time destructors would run is process exit, when graphics/WinRT DLLs may already be
// shut down: releasing captures or devices then crashes the exiting process.

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    wgd::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
};
Device& g_ourDevice = *new Device;
ComPtr<ID3D11Device>& g_device = g_ourDevice.device;
ComPtr<ID3D11DeviceContext>& g_context = g_ourDevice.context;
wgd::Direct3D11::IDirect3DDevice& g_winrtDevice = g_ourDevice.winrtDevice;

bool EnsureDevice()
{
    if (g_device)
        return true;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    ComPtr<IDXGIDevice> dxgi;
    winrt::com_ptr<::IInspectable> inspectable;
    if (SUCCEEDED(hr))
        hr = g_device.As(&dxgi);
    if (SUCCEEDED(hr))
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
    if (FAILED(hr)) {
        Log("window fix: could not create a D3D11 device, hr=0x%08lX", static_cast<unsigned long>(hr));
        g_device.Reset();
        return false;
    }
    g_winrtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    // Ask once for capture without the yellow border (granted automatically to desktop apps
    // on Windows 11; the API doesn't exist on Windows 10). Fire and forget.
    try {
        wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless);
    } catch (...) {
    }
    return true;
}

// One FP16 capture of a window that TeamSpeak is streaming.
struct Session {
    std::wstring className; // for the log, also after the window is gone
    HMONITOR monitor = nullptr;
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession capture{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    Converter converter;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint8_t> pixels; // last converted frame, BGRA8, top-down, width x height
    UINT width = 0, height = 0;

    ~Session()
    {
        try {
            if (capture)
                capture.Close();
            if (pool)
                pool.Close();
        } catch (...) {
        }
    }
};

struct Tracked {
    ULONGLONG burstStart = 0; // activation heuristic
    int burstCalls = 0;
    ULONGLONG lastUse = 0;
    ULONGLONG retryAfter = 0; // after a failure, leave GDI output alone until then
    std::unique_ptr<Session> session;
};

std::mutex& g_lock = *new std::mutex;
std::map<HWND, Tracked>& g_windows = *new std::map<HWND, Tracked>; // captured recently, on HDR monitors

// Forgets windows (and closes our captures) once TeamSpeak stops asking for them.
DWORD WINAPI SweepThread(void*)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded); // also keeps an MTA alive for WGC
    for (;;) {
        Sleep(1000);
        std::lock_guard lock(g_lock);
        const ULONGLONG now = GetTickCount64();
        for (auto it = g_windows.begin(); it != g_windows.end();) {
            if (now - it->second.lastUse > kIdleCloseMs) {
                if (it->second.session)
                    Log("window fix: '%ls' is no longer captured, stopping", it->second.session->className.c_str());
                it = g_windows.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void StartSweeperOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (HANDLE thread = CreateThread(nullptr, 0, SweepThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    });
}

// Starts an FP16 WGC capture of hwnd. `step` names the call that failed if this throws.
std::unique_ptr<Session> StartSession(HWND hwnd, HMONITOR monitor, const DXGI_OUTPUT_DESC1& output, const char*& step)
{
    step = "D3D11 device";
    if (!EnsureDevice())
        return nullptr;
    auto s = std::make_unique<Session>();
    s->className = ClassOf(hwnd);
    s->monitor = monitor;
    step = "converter setup";
    if (!s->converter.Init(g_device.Get(), output, DXGI_FORMAT_B8G8R8A8_UNORM))
        return nullptr;

    step = "CreateForWindow";
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(s->item)));
    s->poolSize = s->item.Size();
    if (s->poolSize.Width <= 0 || s->poolSize.Height <= 0) {
        step = "window has no size (minimized?)";
        return nullptr;
    }
    step = "CreateFreeThreaded";
    s->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(g_winrtDevice, kFormat, 2, s->poolSize);
    step = "CreateCaptureSession";
    s->capture = s->pool.CreateCaptureSession(s->item);
    try {
        s->capture.IsCursorCaptureEnabled(false); // GDI captures don't include the cursor either
        s->capture.IsBorderRequired(false);       // Windows 11 only; Windows 10 shows a yellow border
    } catch (...) {
    }
    step = "StartCapture";
    s->capture.StartCapture();
    Log("window fix: streaming '%ls' on %ls -> capturing it in FP16 (%dx%d), SDR white %.0f nits",
        s->className.c_str(), output.DeviceName, s->poolSize.Width, s->poolSize.Height,
        s->converter.SdrWhiteNits());
    return s;
}

// Pulls the newest WGC frame (if any) and converts it into s.pixels.
bool UpdateFrame(Session& s)
{
    wgc::Direct3D11CaptureFrame latest{nullptr};
    const ULONGLONG deadline = GetTickCount64() + (s.pixels.empty() ? kFirstFrameWaitMs : 0);
    for (;;) {
        while (auto frame = s.pool.TryGetNextFrame())
            latest = frame; // older frames go back to the pool
        if (latest || GetTickCount64() >= deadline)
            break;
        Sleep(5);
    }
    if (!latest)
        return true; // nothing new (the window didn't change): keep the previous frame

    const auto size = latest.ContentSize();
    if (size.Width <= 0 || size.Height <= 0)
        return true; // minimized or closing: keep the previous frame
    if (size.Width != s.poolSize.Width || size.Height != s.poolSize.Height) {
        s.pool.Recreate(g_winrtDevice, kFormat, 2, size); // window was resized
        s.poolSize = size;
    }

    ComPtr<ID3D11Texture2D> texture;
    auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
    ID3D11Texture2D* converted = s.converter.Convert(texture.Get());
    if (!converted)
        return false;

    D3D11_TEXTURE2D_DESC d;
    converted->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd{};
    if (s.staging)
        s.staging->GetDesc(&sd);
    if (!s.staging || sd.Width != d.Width || sd.Height != d.Height) {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        s.staging.Reset();
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &s.staging)))
            return false;
    }
    g_context->CopyResource(s.staging.Get(), converted);
    latest.Close();

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;
    s.width = std::min<UINT>(size.Width, d.Width);
    s.height = std::min<UINT>(size.Height, d.Height);
    s.pixels.resize(size_t(s.width) * s.height * 4);
    for (UINT y = 0; y < s.height; ++y)
        memcpy(&s.pixels[size_t(y) * s.width * 4], static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch,
               size_t(s.width) * 4);
    g_context->Unmap(s.staging.Get(), 0);
    return true;
}

// Verbose log only: average colour of a DIB section on a coarse grid.
std::string DibAverage(const DIBSECTION& ds)
{
    const int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    double sum[4] = {};
    int n = 0;
    for (int gy = 1; gy < 10; ++gy)
        for (int gx = 1; gx < 10; ++gx) {
            const uint8_t* p = static_cast<const uint8_t*>(ds.dsBm.bmBits) + size_t(h * gy / 10) * ds.dsBm.bmWidthBytes +
                               size_t(w * gx / 10) * 4;
            for (int c = 0; c < 4; ++c)
                sum[c] += p[c];
            ++n;
        }
    char buf[64];
    snprintf(buf, sizeof(buf), "rgb %.0f %.0f %.0f a %.0f", sum[2] / n, sum[1] / n, sum[0] / n, sum[3] / n);
    return buf;
}

// Overwrites what GDI just drew into dc's DIB with the converted WGC frame.
// (dstX, dstY, w, h): destination rectangle; (srcX, srcY): its origin in window coordinates
// (relative to GetWindowRect), which is what PrintWindow and window-DC BitBlt use.
void ReplacePixels(HWND hwnd, HDC dc, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
    if (!IsWindow(hwnd) || IsIconic(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
        return;

    DIBSECTION ds{};
    HGDIOBJ bitmap = GetCurrentObject(dc, OBJ_BITMAP);
    if (!bitmap || GetObjectW(bitmap, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
        Verbose("skip: not a 32-bit DIB", hwnd);
        return; // not a 32-bpp DIB section: not a capture
    }
    char diag[200] = "";
    if (VerboseLogOn())
        snprintf(diag, sizeof(diag), "dib %dx%d dst %d,%d src %d,%d gdi %s", ds.dsBm.bmWidth, ds.dsBm.bmHeight, dstX,
                 dstY, srcX, srcY, DibAverage(ds).c_str());

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    std::lock_guard lock(g_lock);
    StartSweeperOnce();
    const MonitorState& mon = LookupMonitor(monitor);
    if (!mon.hdr) {
        g_windows.erase(hwnd); // e.g. moved to an SDR monitor: GDI is already correct there
        Verbose("skip: SDR monitor", hwnd, diag);
        return;
    }

    Tracked& t = g_windows[hwnd];
    const ULONGLONG now = GetTickCount64();
    t.lastUse = now;
    if (now < t.retryAfter) {
        Verbose("skip: waiting to retry", hwnd, diag);
        return;
    }
    if (!t.session || t.session->monitor != monitor) {
        if (!t.session) {
            if (now - t.burstStart > kActivateWindowMs) {
                t.burstStart = now;
                t.burstCalls = 0;
            }
            if (++t.burstCalls < kActivateAfterCalls) {
                Verbose("skip: not a stream (yet)", hwnd, diag);
                return; // probably a thumbnail, not a stream
            }
        }
        t.session.reset();
        const char* step = "?";
        try {
            t.session = StartSession(hwnd, monitor, mon.desc, step);
        } catch (const winrt::hresult_error& e) {
            Log("window fix: could not capture '%ls' (%s failed, hr=0x%08lX), retrying in %llu s",
                ClassOf(hwnd).c_str(), step, static_cast<unsigned long>(e.code()), kRetryMs / 1000);
        }
        if (!t.session) {
            t.retryAfter = now + kRetryMs;
            return;
        }
    }

    Session& s = *t.session;
    bool ok = false;
    try {
        ok = UpdateFrame(s);
    } catch (const winrt::hresult_error& e) {
        Log("window fix: frame update failed, hr=0x%08lX", static_cast<unsigned long>(e.code()));
    }
    if (!ok) {
        Log("window fix: lost the capture of '%ls', retrying in %llu s", s.className.c_str(), kRetryMs / 1000);
        t.session.reset();
        t.retryAfter = now + kRetryMs;
        return;
    }
    if (s.pixels.empty()) {
        Verbose("skip: no frame yet", hwnd, diag);
        return;
    }

    // WGC frames start at the window's visible bounds; GDI coordinates at GetWindowRect,
    // which also includes the invisible resize borders.
    RECT wr, visible;
    GetWindowRect(hwnd, &wr);
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
        visible = wr;
    const int offX = wr.left + srcX - visible.left - dstX; // WGC x = destination x + offX
    const int offY = wr.top + srcY - visible.top - dstY;

    const int dibW = ds.dsBm.bmWidth, dibH = ds.dsBm.bmHeight;
    const int x0 = std::max({dstX, 0, -offX});
    const int x1 = std::min({dstX + std::min(w, dibW), dibW, int(s.width) - offX});
    const int y0 = std::max({dstY, 0, -offY});
    const int y1 = std::min({dstY + std::min(h, dibH), dibH, int(s.height) - offY});
    if (x0 >= x1 || y0 >= y1) {
        Verbose("skip: no overlap", hwnd, diag);
        return;
    }

    // GDI draws the region into the DC from a top-down buffer, so the destination bitmap's
    // row order (top-down or bottom-up) is GDI's business, not ours. (GetObject doesn't
    // report it reliably: dsBmih.biHeight is positive for top-down DIBs too.)
    // When the region covers whole rows of our frame (the usual PrintWindow case), GDI
    // reads our frame in place; otherwise the region is copied out first.
    const int cw = x1 - x0, ch = y1 - y0;
    const int sx = x0 + offX, sy = y0 + offY; // region's top-left in s.pixels
    const uint8_t* src = &s.pixels[size_t(sy) * s.width * 4];
    std::vector<uint8_t> region;
    if (sx != 0 || cw != int(s.width)) {
        region.resize(size_t(cw) * ch * 4);
        for (int y = 0; y < ch; ++y)
            memcpy(&region[size_t(y) * cw * 4], &s.pixels[(size_t(sy + y) * s.width + sx) * 4], size_t(cw) * 4);
        src = region.data();
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, x0, y0, cw, ch, 0, 0, cw, ch, src, &bi, DIB_RGB_COLORS, SRCCOPY);

    if (VerboseLogOn()) {
        GdiFlush();
        Verbose("replaced", hwnd, (std::string(diag) + " -> ours " + DibAverage(ds)).c_str());
    }
}

// ---- Hooks -------------------------------------------------------------------------

using PrintWindowFn = BOOL(WINAPI*)(HWND, HDC, UINT);
using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
PrintWindowFn g_origPrintWindow = nullptr;
BitBltFn g_origBitBlt = nullptr;
thread_local bool t_inFix = false;   // our own GDI calls must not re-enter the fix
std::atomic<bool> g_crashed{false}; // set after a crash in our code: the fix stays off

void ReplacePixelsCatch(const char* api, HWND hwnd, HDC dc, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
    try {
        ReplacePixels(hwnd, dc, dstX, dstY, w, h, srcX, srcY);
    } catch (const winrt::hresult_error& e) {
        Log("window fix: unexpected error 0x%08lX after %s", static_cast<unsigned long>(e.code()), api);
    } catch (...) {
        Log("window fix: unexpected error after %s", api);
    }
}

// Last line of defence: a crash in our code must not take TeamSpeak down. GDI's own output
// stays in place, and the fix stays off from then on (our lock might still be held).
void ReplacePixelsGuarded(const char* api, HWND hwnd, HDC dc, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
    if (t_inFix || g_crashed || !Cfg().enabled || !Cfg().fixWindowCapture)
        return;
    t_inFix = true;
    __try {
        ReplacePixelsCatch(api, hwnd, dc, dstX, dstY, w, h, srcX, srcY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_crashed = true;
        Log("window fix: crashed (exception 0x%08lX after %s), turned off until TeamSpeak restarts", GetExceptionCode(),
            api);
    }
    t_inFix = false;
}

BOOL WINAPI HookPrintWindow(HWND hwnd, HDC dc, UINT flags)
{
    const BOOL ok = g_origPrintWindow(hwnd, dc, flags);
    if (!t_inFix && VerboseLogOn()) {
        char what[48];
        snprintf(what, sizeof(what), "PrintWindow(flags %u)%s", flags, ok ? "" : " FAILED");
        Verbose(what, hwnd);
    }
    if (ok)
        ReplacePixelsGuarded("PrintWindow", hwnd, dc, 0, 0, INT_MAX, INT_MAX, 0, 0);
    return ok;
}

BOOL WINAPI HookBitBlt(HDC dst, int x, int y, int cx, int cy, HDC src, int x1, int y1, DWORD rop)
{
    const BOOL ok = g_origBitBlt(dst, x, y, cx, cy, src, x1, y1, rop);
    if (ok && src && (rop & ~CAPTUREBLT) == SRCCOPY && !t_inFix) {
        // Only blits from a window DC count; WindowFromDC is null for memory DCs.
        HWND hwnd = WindowFromDC(src);
        if (hwnd && hwnd != GetDesktopWindow()) {
            Verbose("BitBlt from window DC", hwnd);
            ReplacePixelsGuarded("BitBlt", hwnd, dst, x, y, cx, cy, x1, y1);
        }
    }
    return ok;
}

void Hook(const wchar_t* module, const char* name, void* detour, void** original)
{
    HMODULE m = LoadLibraryW(module);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    const MH_STATUS st = target ? MH_CreateHook(target, detour, original) : MH_ERROR_FUNCTION_NOT_FOUND;
    const MH_STATUS en = st == MH_OK ? MH_EnableHook(target) : st;
    Log("hook %s: %s", name, en == MH_OK ? "installed" : MH_StatusToString(en));
}

} // namespace

void InstallWindowCaptureHooks()
{
    if (!Cfg().fixWindowCapture) {
        Log("window fix: disabled in hdrfix.ini (fix_window_capture=0)");
        return;
    }
    Hook(L"user32.dll", "PrintWindow", reinterpret_cast<void*>(&HookPrintWindow),
         reinterpret_cast<void**>(&g_origPrintWindow));
    Hook(L"gdi32.dll", "BitBlt", reinterpret_cast<void*>(&HookBitBlt), reinterpret_cast<void**>(&g_origBitBlt));
}
