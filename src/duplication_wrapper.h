#pragma once
#include "converter.h"

#include <atomic>
#include <dxgi1_6.h>
#include <wrl/client.h>

// IDXGIOutputDuplication handed to TeamSpeak. Wraps a real FP16 duplication and
// returns converted frames; everything else is passed through.
class DuplicationWrapper final : public IDXGIOutputDuplication {
public:
    // Returns a new wrapper (refcount 1), or nullptr if the converter can't be set up.
    static IDXGIOutputDuplication* Create(IDXGIOutputDuplication* inner, ID3D11Device* device,
                                          const DXGI_OUTPUT_DESC1& output, DXGI_FORMAT outFormat, float sdrWhiteNits);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override;

    // IDXGIOutputDuplication
    void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT timeoutMs, DXGI_OUTDUPL_FRAME_INFO* info,
                                               IDXGIResource** resource) override;
    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT size, RECT* rects, UINT* required) override;
    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT size, DXGI_OUTDUPL_MOVE_RECT* rects,
                                                UINT* required) override;
    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT size, void* buffer, UINT* required,
                                                   DXGI_OUTDUPL_POINTER_SHAPE_INFO* info) override;
    HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT* rect) override;
    HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override;
    HRESULT STDMETHODCALLTYPE ReleaseFrame() override;

private:
    DuplicationWrapper(IDXGIOutputDuplication* inner, DXGI_FORMAT outFormat);
    ~DuplicationWrapper() = default;

    std::atomic<ULONG> refs_{1};
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> inner_;
    Converter converter_;
    DXGI_FORMAT outFormat_;
    bool hadGoodFrame_ = false;
};
