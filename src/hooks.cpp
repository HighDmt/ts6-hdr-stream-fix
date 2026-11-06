#include "hooks.h"

#include "display_info.h"
#include "duplication_wrapper.h"
#include "log_config.h"

#include <MinHook.h>
#include <atomic>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

extern "C" const size_t kSlot_IDXGIOutput1_DuplicateOutput;  // vtable_index.c
extern "C" const size_t kSlot_IDXGIOutput5_DuplicateOutput1;

namespace {

using DuplicateOutputFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput1*, IUnknown*, IDXGIOutputDuplication**);
using DuplicateOutput1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput5*, IUnknown*, UINT, UINT,
                                                       const DXGI_FORMAT*, IDXGIOutputDuplication**);

DuplicateOutputFn g_origDuplicateOutput = nullptr;   // MinHook trampolines
DuplicateOutput1Fn g_origDuplicateOutput1 = nullptr;

// Failure handling. An ordinary failure (e.g. a GPU driver reset mid-stream) makes TeamSpeak
// restart the capture, which hdrfix intercepts again. Only kMaxFailuresInARow failures
// without a good frame in between pause interception, for kPauseMs, so a persistent
// problem can't leave TeamSpeak restarting its capture forever. A crash in our setup
// code is different: that's a bug, and interception stays off until TeamSpeak restarts.
constexpr int kMaxFailuresInARow = 3;
constexpr ULONGLONG kPauseMs = 60000;
std::atomic<int> g_failuresInARow{0};
std::atomic<ULONGLONG> g_pausedUntil{0};
std::atomic<bool> g_crashed{false};

// >0 while we are inside one of our hooks on this thread. DXGI may implement
// DuplicateOutput on top of DuplicateOutput1; such inner calls must not be intercepted.
thread_local int t_hookDepth = 0;
struct ReentryGuard {
    ReentryGuard() { ++t_hookDepth; }
    ~ReentryGuard() { --t_hookDepth; }
};

// DuplicateOutput1 requires a per-monitor-DPI-aware caller. Chromium processes
// normally are; setting it on the thread for the call makes sure.
class PerMonitorDpiScope {
public:
    PerMonitorDpiScope()
    {
        using SetFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
            set_ = reinterpret_cast<SetFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (set_)
            previous_ = set_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~PerMonitorDpiScope()
    {
        if (set_ && previous_)
            set_(previous_);
    }

private:
    DPI_AWARENESS_CONTEXT(WINAPI* set_)(DPI_AWARENESS_CONTEXT) = nullptr;
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

const char* FormatName(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "FP16";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    default: return "other";
    }
}

HRESULT CallOriginalDuplicateOutput1(IDXGIOutput5* output, IUnknown* device, UINT flags, UINT count,
                                     const DXGI_FORMAT* formats, IDXGIOutputDuplication** out)
{
    ReentryGuard guard;
    return g_origDuplicateOutput1 ? g_origDuplicateOutput1(output, device, flags, count, formats, out)
                                  : output->DuplicateOutput1(device, flags, count, formats, out); // not hooked
}

// Sets up an FP16 duplication + converter for an HDR output. Returns false (having
// logged why) whenever the caller should just run the original API instead.
bool TryIntercept(IDXGIOutput* output, IUnknown* device, UINT flags, DXGI_FORMAT outFormat,
                  const char* api, IDXGIOutputDuplication** out)
{
    if (!Cfg().enabled || g_crashed)
        return false;
    if (GetTickCount64() < g_pausedUntil) {
        Log("%s: paused after repeated failures, passing through", api);
        return false;
    }

    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 od{};
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output6))) || FAILED(output6->GetDesc1(&od))) {
        Log("%s: IDXGIOutput6 unavailable, passing through", api);
        return false;
    }
    if (!IsHdrColorSpace(od.ColorSpace)) {
        Log("%s on %ls: SDR output (color space %d), passing through", api, od.DeviceName, od.ColorSpace);
        return false;
    }

    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIOutput5> output5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d3d))) || FAILED(output->QueryInterface(IID_PPV_ARGS(&output5)))) {
        Log("%s on %ls: not a D3D11 device or no IDXGIOutput5, passing through", api, od.DeviceName);
        return false;
    }

    const DXGI_FORMAT fp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<IDXGIOutputDuplication> inner;
    HRESULT hr;
    {
        PerMonitorDpiScope dpi;
        hr = CallOriginalDuplicateOutput1(output5.Get(), device, flags, 1, &fp16, &inner);
    }
    if (FAILED(hr)) {
        Log("%s on %ls: DuplicateOutput1(FP16) failed, hr=0x%08lX, passing through", api, od.DeviceName,
            static_cast<unsigned long>(hr));
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    inner->GetDesc(&dd);
    if (dd.ModeDesc.Format != fp16 || dd.DesktopImageInSystemMemory) {
        Log("%s on %ls: got format %d (system memory %d) instead of an FP16 GPU surface, passing through",
            api, od.DeviceName, dd.ModeDesc.Format, dd.DesktopImageInSystemMemory);
        return false;
    }

    IDXGIOutputDuplication* wrapper = DuplicationWrapper::Create(inner.Get(), d3d.Get(), od, outFormat);
    if (!wrapper) {
        Log("%s on %ls: converter setup failed, passing through", api, od.DeviceName);
        return false;
    }
    *out = wrapper;
    Log("%s on %ls intercepted: %ux%u FP16 -> %s, knee %.2f", api, od.DeviceName, dd.ModeDesc.Width,
        dd.ModeDesc.Height, FormatName(outFormat), Cfg().knee);
    return true;
}

// Never let an unexpected fault escape into TeamSpeak: treat it as "use the original".
bool TryInterceptSafe(IDXGIOutput* output, IUnknown* device, UINT flags, DXGI_FORMAT outFormat,
                      const char* api, IDXGIOutputDuplication** out)
{
    __try {
        return TryIntercept(output, device, flags, outFormat, api, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("%s: crashed (exception 0x%08lX) during setup; screen captures are left alone until TeamSpeak restarts",
            api, GetExceptionCode());
        g_crashed = true;
        return false;
    }
}

HRESULT STDMETHODCALLTYPE HookDuplicateOutput(IDXGIOutput1* self, IUnknown* device, IDXGIOutputDuplication** out)
{
    if (t_hookDepth == 0 && device && out) {
        ReentryGuard guard;
        if (TryInterceptSafe(self, device, 0, DXGI_FORMAT_B8G8R8A8_UNORM, "DuplicateOutput", out))
            return S_OK;
    }
    ReentryGuard guard;
    return g_origDuplicateOutput(self, device, out);
}

HRESULT STDMETHODCALLTYPE HookDuplicateOutput1(IDXGIOutput5* self, IUnknown* device, UINT flags, UINT count,
                                               const DXGI_FORMAT* formats, IDXGIOutputDuplication** out)
{
    if (t_hookDepth == 0 && device && out && formats) {
        bool wantsFp16 = false, wantsBgra = false;
        for (UINT i = 0; i < count; ++i) {
            wantsFp16 |= formats[i] == DXGI_FORMAT_R16G16B16A16_FLOAT;
            wantsBgra |= formats[i] == DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        // An app asking for FP16 gets corrected linear FP16; otherwise BGRA8 if it accepts that.
        if (wantsFp16 || wantsBgra) {
            ReentryGuard guard;
            const DXGI_FORMAT outFormat = wantsFp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
            if (TryInterceptSafe(self, device, flags, outFormat, "DuplicateOutput1", out))
                return S_OK;
        } else {
            Log("DuplicateOutput1: none of the %u requested formats is BGRA8/FP16, passing through", count);
        }
    }
    ReentryGuard guard;
    return g_origDuplicateOutput1(self, device, flags, count, formats, out);
}

bool HookSlot(void* object, size_t slot, void* detour, void** original, const char* name)
{
    void* target = (*static_cast<void***>(object))[slot];
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    Log("hook %s (vtable slot %zu, %p): %s", name, slot, target, st == MH_OK ? "installed" : MH_StatusToString(st));
    return st == MH_OK;
}

} // namespace

void ReportConversionFailure(const char* reason)
{
    const int n = ++g_failuresInARow;
    if (n < kMaxFailuresInARow) {
        Log("%s; TeamSpeak will restart the capture and hdrfix will try again", reason);
        return;
    }
    g_failuresInARow = 0;
    g_pausedUntil = GetTickCount64() + kPauseMs;
    Log("%s, %d times in a row; leaving new screen captures alone for %llu s", reason, n, kPauseMs / 1000);
}

void ReportConversionSuccess() { g_failuresInARow = 0; }

bool InstallHooks()
{
    // Any output works: all IDXGIOutput objects share dxgi.dll's implementation,
    // and MinHook patches the function itself, not one object's vtable.
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Log("CreateDXGIFactory1 failed, hr=0x%08lX; hooks not installed", static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; !output && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;
    if (!output || FAILED(output.As(&output1))) {
        Log("no DXGI output found; hooks not installed");
        return false;
    }
    ComPtr<IDXGIOutput5> output5;
    output.As(&output5);

    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed; hooks not installed");
        return false;
    }
    const bool ok = HookSlot(output1.Get(), kSlot_IDXGIOutput1_DuplicateOutput,
                             reinterpret_cast<void*>(&HookDuplicateOutput),
                             reinterpret_cast<void**>(&g_origDuplicateOutput), "IDXGIOutput1::DuplicateOutput");
    if (output5)
        HookSlot(output5.Get(), kSlot_IDXGIOutput5_DuplicateOutput1, reinterpret_cast<void*>(&HookDuplicateOutput1),
                 reinterpret_cast<void**>(&g_origDuplicateOutput1), "IDXGIOutput5::DuplicateOutput1");
    return ok;
}
