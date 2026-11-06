#include "duplication_wrapper.h"

#include "verbose_log.h"
#include "log_config.h"

using Microsoft::WRL::ComPtr;

DuplicationWrapper::DuplicationWrapper(IDXGIOutputDuplication* inner, DXGI_FORMAT outFormat)
    : inner_(inner), outFormat_(outFormat)
{
}

IDXGIOutputDuplication* DuplicationWrapper::Create(IDXGIOutputDuplication* inner, ID3D11Device* device,
                                                   const DXGI_OUTPUT_DESC1& output, DXGI_FORMAT outFormat)
{
    auto* wrapper = new (std::nothrow) DuplicationWrapper(inner, outFormat);
    if (wrapper && !wrapper->converter_.Init(device, output, outFormat)) {
        wrapper->Release();
        return nullptr;
    }
    return wrapper;
}

// --- IUnknown ---------------------------------------------------------------

HRESULT DuplicationWrapper::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIOutputDuplication)) {
        *ppv = static_cast<IDXGIOutputDuplication*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG DuplicationWrapper::AddRef() { return ++refs_; }

ULONG DuplicationWrapper::Release()
{
    const ULONG left = --refs_;
    if (left == 0)
        delete this;
    return left;
}

// --- IDXGIObject: pass-through ---------------------------------------------------

HRESULT DuplicationWrapper::SetPrivateData(REFGUID name, UINT size, const void* data)
{
    return inner_->SetPrivateData(name, size, data);
}
HRESULT DuplicationWrapper::SetPrivateDataInterface(REFGUID name, const IUnknown* unknown)
{
    return inner_->SetPrivateDataInterface(name, unknown);
}
HRESULT DuplicationWrapper::GetPrivateData(REFGUID name, UINT* size, void* data)
{
    return inner_->GetPrivateData(name, size, data);
}
HRESULT DuplicationWrapper::GetParent(REFIID riid, void** parent) { return inner_->GetParent(riid, parent); }

// --- IDXGIOutputDuplication -----------------------------------------------------

void DuplicationWrapper::GetDesc(DXGI_OUTDUPL_DESC* desc)
{
    inner_->GetDesc(desc);
    if (desc)
        desc->ModeDesc.Format = outFormat_; // what we actually hand out
}

HRESULT DuplicationWrapper::AcquireNextFrame(UINT timeoutMs, DXGI_OUTDUPL_FRAME_INFO* info,
                                             IDXGIResource** resource)
{
    if (!info || !resource)
        return inner_->AcquireNextFrame(timeoutMs, info, resource); // lets DXGI report the error

    ComPtr<IDXGIResource> frame;
    const HRESULT hr = inner_->AcquireNextFrame(timeoutMs, info, &frame);
    *resource = nullptr;
    if (FAILED(hr))
        return hr; // timeouts, access lost, ...: TeamSpeak handles these as usual

    ComPtr<ID3D11Texture2D> desktop;
    ID3D11Texture2D* converted = nullptr;
    if (frame && SUCCEEDED(frame.As(&desktop))) {
        if (!loggedFirstFrame_) {
            D3D11_TEXTURE2D_DESC d;
            desktop->GetDesc(&d);
            Log("first frame: %ux%u format %d bind 0x%X misc 0x%X; SDR white %.0f nits, peak %.0f nits",
                d.Width, d.Height, d.Format, d.BindFlags, d.MiscFlags, converter_.SdrWhiteNits(),
                converter_.PeakNits());
            loggedFirstFrame_ = true;
        }
        // Pointer-only update (LastPresentTime == 0): the desktop didn't change, so the
        // last converted frame is still correct. Otherwise convert the new frame.
        VerboseCount(info->LastPresentTime.QuadPart == 0 ? "DXGI frame (cursor only)" : "DXGI frame");
        converted = (info->LastPresentTime.QuadPart == 0) ? converter_.LastOutput() : nullptr;
        if (!converted)
            converted = converter_.Convert(desktop.Get());
    }

    if (!converted || FAILED(converted->QueryInterface(IID_PPV_ARGS(resource)))) {
        // Can't produce a correct frame. Make TeamSpeak recreate the duplication;
        // the hook then calls the original API.
        *resource = nullptr;
        inner_->ReleaseFrame();
        DisableInterception("frame conversion failed");
        return DXGI_ERROR_ACCESS_LOST;
    }
    return hr;
}

HRESULT DuplicationWrapper::GetFrameDirtyRects(UINT size, RECT* rects, UINT* required)
{
    return inner_->GetFrameDirtyRects(size, rects, required);
}
HRESULT DuplicationWrapper::GetFrameMoveRects(UINT size, DXGI_OUTDUPL_MOVE_RECT* rects, UINT* required)
{
    return inner_->GetFrameMoveRects(size, rects, required);
}
HRESULT DuplicationWrapper::GetFramePointerShape(UINT size, void* buffer, UINT* required,
                                                 DXGI_OUTDUPL_POINTER_SHAPE_INFO* info)
{
    return inner_->GetFramePointerShape(size, buffer, required, info);
}
HRESULT DuplicationWrapper::MapDesktopSurface(DXGI_MAPPED_RECT* rect) { return inner_->MapDesktopSurface(rect); }
HRESULT DuplicationWrapper::UnMapDesktopSurface() { return inner_->UnMapDesktopSurface(); }
HRESULT DuplicationWrapper::ReleaseFrame() { return inner_->ReleaseFrame(); }
