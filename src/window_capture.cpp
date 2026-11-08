#include "window_capture.h"

#include "converter.h"
#include "log_config.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
using Microsoft::WRL::ComPtr;

namespace {

// WGC delivers a frame per window update, e.g. 144+ per second for a game, each an FP16 copy
// made by DWM on the game's GPU. TeamSpeak streams at most 60 fps: a ~80 fps cap leaves
// margin for timing jitter and skips the copies nobody uses. (Windows 11 24H2 and later.)
constexpr std::chrono::milliseconds kMinFrameInterval{12};
constexpr auto kFormat = wgd::DirectXPixelFormat::R16G16B16A16Float;

// One D3D11 device for all window captures, used under the caller's lock.
// Heap-allocated and never destroyed: the DLL is pinned, so destructors would only run at
// process exit, when graphics/WinRT DLLs may already be shut down and releasing captures
// or devices crashes the exiting process.
struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    wgd::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
};
Device& g_dev = *new Device;

bool EnsureDevice()
{
    if (g_dev.device)
        return true;

    // WinRT capture objects live in the multithreaded COM apartment. Make sure one exists
    // for the rest of the process, whatever TeamSpeak's threads do with COM.
    static CO_MTA_USAGE_COOKIE mta = [] {
        CO_MTA_USAGE_COOKIE cookie{};
        CoIncrementMTAUsage(&cookie);
        return cookie;
    }();
    (void)mta;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &g_dev.device, nullptr, &g_dev.context);
    ComPtr<IDXGIDevice> dxgi;
    winrt::com_ptr<::IInspectable> inspectable;
    if (SUCCEEDED(hr))
        hr = g_dev.device.As(&dxgi);
    if (SUCCEEDED(hr))
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
    if (FAILED(hr)) {
        Log("window fix: could not create a D3D11 device, hr=0x%08lX", static_cast<unsigned long>(hr));
        g_dev.device.Reset();
        g_dev.context.Reset();
        return false;
    }
    g_dev.winrtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    // Ask once for capture without the yellow border (granted automatically to desktop apps
    // on Windows 11; the API doesn't exist on Windows 10). Fire and forget.
    try {
        wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless);
    } catch (...) {
    }
    return true;
}

// A CPU-readable copy of one converted frame.
struct Readback {
    ComPtr<ID3D11Texture2D> texture;
    D3D11_MAPPED_SUBRESOURCE map{};
    bool mapped = false;
    UINT width = 0, height = 0; // content size (the texture can be larger)
};

} // namespace

struct WindowCapture::Impl {
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    Converter converter;
    Readback buffers[2];
    int current = -1; // mapped: what Current() hands out
    int pending = -1; // copy issued, not mapped yet

    ~Impl()
    {
        for (Readback& b : buffers)
            Unmap(b);
        try {
            if (session)
                session.Close();
            if (pool)
                pool.Close();
        } catch (...) {
        }
    }

    static void Unmap(Readback& b)
    {
        if (b.mapped)
            g_dev.context->Unmap(b.texture.Get(), 0);
        b.mapped = false;
    }

    // Makes the pending copy the current frame once the GPU has finished it. Without
    // `wait`, a copy still in flight is left for next time. False only on a real error.
    bool CollectPending(bool wait)
    {
        if (pending < 0)
            return true;
        Readback& b = buffers[pending];
        const HRESULT hr =
            g_dev.context->Map(b.texture.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &b.map);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return true;
        if (FAILED(hr)) {
            Log("window fix: reading back a frame failed, hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        b.mapped = true;
        if (current >= 0)
            Unmap(buffers[current]);
        else
            Log("window fix: first frame %ux%u, row pitch %u, %u bytes", b.width, b.height, b.map.RowPitch,
                b.map.DepthPitch);
        current = pending;
        pending = -1;
        return true;
    }

    bool Update()
    {
        // The copy issued on the previous call is normally finished by now: no waiting.
        if (!CollectPending(false))
            return false;

        // Newest frame from WGC; older ones go back to the pool. Never waits: until the first
        // frame is ready, TeamSpeak's own capture is used.
        wgc::Direct3D11CaptureFrame latest{nullptr};
        while (auto frame = pool.TryGetNextFrame())
            latest = frame;
        if (!latest)
            return true; // nothing new: keep the current frame
        const auto size = latest.ContentSize();
        if (size.Width <= 0 || size.Height <= 0)
            return true; // minimized or closing: keep the current frame
        if (size.Width != poolSize.Width || size.Height != poolSize.Height) {
            pool.Recreate(g_dev.winrtDevice, kFormat, 2, size); // window was resized
            poolSize = size;
        }

        ComPtr<ID3D11Texture2D> texture;
        auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
        ID3D11Texture2D* converted = converter.Convert(texture.Get());
        if (!converted) {
            Log("window fix: converting a frame failed");
            return false;
        }

        // GPU so busy that the previous copy isn't done after a whole capture interval:
        // wait for it rather than overwrite a frame that was never shown.
        if (!CollectPending(true))
            return false;

        // Copy into the buffer that isn't mapped.
        Readback& b = buffers[current == 0 ? 1 : 0];
        D3D11_TEXTURE2D_DESC d;
        converted->GetDesc(&d);
        D3D11_TEXTURE2D_DESC have{};
        if (b.texture)
            b.texture->GetDesc(&have);
        if (!b.texture || have.Width != d.Width || have.Height != d.Height) {
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = 0;
            b.texture.Reset();
            const HRESULT hr = g_dev.device->CreateTexture2D(&d, nullptr, &b.texture);
            if (FAILED(hr)) {
                Log("window fix: CreateTexture2D(readback) failed, hr=0x%08lX", static_cast<unsigned long>(hr));
                return false;
            }
        }
        g_dev.context->CopyResource(b.texture.Get(), converted);
        g_dev.context->Flush(); // start the GPU work now, so it's done by the next call
        latest.Close();
        b.width = std::min<UINT>(size.Width, d.Width);
        b.height = std::min<UINT>(size.Height, d.Height);
        pending = static_cast<int>(&b - buffers);
        return true;
    }
};

WindowCapture::WindowCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WindowCapture::~WindowCapture() = default;

std::unique_ptr<WindowCapture> WindowCapture::Start(HWND hwnd, const DXGI_OUTPUT_DESC1& output, float sdrWhiteNits,
                                                    const wchar_t* label, const char*& step, HRESULT& hr)
{
    hr = E_FAIL;
    try {
        step = "D3D11 device";
        if (!EnsureDevice())
            return nullptr;
        auto s = std::make_unique<Impl>();
        step = "converter setup";
        if (!s->converter.Init(g_dev.device.Get(), output, DXGI_FORMAT_B8G8R8A8_UNORM, sdrWhiteNits))
            return nullptr;

        step = "CreateForWindow";
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(
            interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(s->item)));
        s->poolSize = s->item.Size();
        if (s->poolSize.Width <= 0 || s->poolSize.Height <= 0) {
            step = "window has no size (minimized?)";
            return nullptr;
        }
        step = "CreateFreeThreaded";
        s->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(g_dev.winrtDevice, kFormat, 2, s->poolSize);
        step = "CreateCaptureSession";
        s->session = s->pool.CreateCaptureSession(s->item);
        try {
            s->session.IsCursorCaptureEnabled(false); // GDI captures don't include the cursor either
            s->session.IsBorderRequired(false);       // Windows 11 only; Windows 10 shows a yellow border
        } catch (...) {
        }
        try {
            s->session.MinUpdateInterval(kMinFrameInterval); // Windows 11 24H2+ only
        } catch (...) {
        }
        step = "StartCapture";
        s->session.StartCapture();
        Log("window fix: streaming '%ls' on %ls -> capturing it in FP16 (%dx%d), SDR white %.0f nits", label,
            output.DeviceName, s->poolSize.Width, s->poolSize.Height, s->converter.SdrWhiteNits());
        return std::unique_ptr<WindowCapture>(new WindowCapture(std::move(s)));
    } catch (const winrt::hresult_error& e) {
        hr = e.code();
        return nullptr;
    }
}

bool WindowCapture::Update()
{
    try {
        return impl_->Update();
    } catch (const winrt::hresult_error& e) {
        Log("window fix: frame update failed, hr=0x%08lX", static_cast<unsigned long>(e.code()));
        return false;
    }
}

WindowCapture::Frame WindowCapture::Current() const
{
    if (impl_->current < 0)
        return {};
    const Readback& b = impl_->buffers[impl_->current];
    return {static_cast<const uint8_t*>(b.map.pData), b.map.RowPitch, b.width, b.height, b.map.DepthPitch};
}
