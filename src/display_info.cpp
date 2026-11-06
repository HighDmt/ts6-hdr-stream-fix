#include "display_info.h"

#include <windows.h>
#include <vector>

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
