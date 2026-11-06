#include "display_info.h"

#include <windows.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE cs)
{
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

bool FindOutputForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    ComPtr<IDXGIFactory1> factory;
    if (!GetMonitorInfoW(monitor, &mi) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&d)) &&
                _wcsicmp(d.DeviceName, mi.szDevice) == 0) {
                desc = d;
                return true;
            }
        }
    }
    return false;
}

float QuerySdrWhiteNits(const wchar_t* gdiDeviceName)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc;
    do {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return 0.0f;
        paths.resize(pathCount);
        modes.resize(modeCount);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                modes.data(), nullptr);
        paths.resize(pathCount);
    } while (rc == ERROR_INSUFFICIENT_BUFFER); // displays changed between the two calls
    if (rc != ERROR_SUCCESS)
        return 0.0f;

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        // Match the path's source (\\.\DISPLAYn) with the DXGI output...
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, gdiDeviceName) != 0)
            continue;

        // ...then ask its target (the monitor) for the SDR white level.
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel != 0)
            return white.SDRWhiteLevel / 1000.0f * 80.0f; // SDRWhiteLevel is in units of 80/1000 nits
    }
    return 0.0f;
}
