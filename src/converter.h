#pragma once
#include <d3d11_4.h> // ID3D11Multithread
#include <dxgi1_6.h>
#include <wrl/client.h>

// SDR white level for a capture of `output`: hdrfix.ini's override, else the Windows
// "SDR content brightness" slider. Read once per capture and never polled:
// QueryDisplayConfig takes a system-wide display lock, and calling it every second
// made games stutter once a second while sharing.
float SdrWhiteNitsFor(const DXGI_OUTPUT_DESC1& output);

// Converts scRGB desktop frames to SDR on the caller's own D3D11 device.
// All rendering happens inside our own ID3DDeviceContextState, so the caller's
// pipeline state on the immediate context is left exactly as it was.
class Converter {
public:
    // outFormat: DXGI_FORMAT_B8G8R8A8_UNORM (sRGB-encoded) or
    //            DXGI_FORMAT_R16G16B16A16_FLOAT (linear, 1.0 = SDR white).
    // sdrWhiteNits: from SdrWhiteNitsFor().
    bool Init(ID3D11Device* device, const DXGI_OUTPUT_DESC1& output, DXGI_FORMAT outFormat, float sdrWhiteNits);

    // Renders src into the output texture and returns it (no extra reference), or nullptr.
    ID3D11Texture2D* Convert(ID3D11Texture2D* src);

    // The last successfully converted frame, or nullptr.
    ID3D11Texture2D* LastOutput() const { return valid_ ? out_.Get() : nullptr; }

    float SdrWhiteNits() const { return sdrWhiteNits_; }
    float PeakNits() const { return peakNits_; }

private:
    bool EnsureOutput(UINT width, UINT height);
    bool EnsureSrcCopy(const D3D11_TEXTURE2D_DESC& srcDesc);

    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Device1> device1_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext1> ctx1_;
    ComPtr<ID3D11Multithread> multithread_;
    ComPtr<ID3DDeviceContextState> state_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;

    ComPtr<ID3D11Texture2D> out_;
    ComPtr<ID3D11RenderTargetView> outRtv_;
    UINT outWidth_ = 0, outHeight_ = 0;
    bool valid_ = false;

    ComPtr<ID3D11ShaderResourceView> srcView_; // view of the last source texture...
    ID3D11Texture2D* srcViewOf_ = nullptr;     // ...which is this one

    // Only used if the desktop texture can't be bound as a shader resource.
    ComPtr<ID3D11Texture2D> srcCopy_;
    ComPtr<ID3D11ShaderResourceView> srcCopySrv_;

    DXGI_FORMAT outFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    float sdrWhiteNits_ = 80.0f;
    float peakNits_ = 1000.0f;
};
