#pragma once
#include <windows.h>

// Settings from hdrfix.ini (next to the DLL). Defaults apply when the file or a key is missing.
struct Config {
    bool enabled = true;
    float sdrWhiteOverrideNits = 0.0f; // 0 = read the Windows SDR white level
    float knee = 0.75f;                // roll-off start, in SDR-white units
    float peakOverrideNits = 0.0f;     // 0 = DXGI_OUTPUT_DESC1::MaxLuminance
    bool fixWindowCapture = true;      // also fix single-window (app) sharing
    bool log = true;
    bool verboseLog = false;           // per-second capture statistics, for troubleshooting
};

const Config& Cfg();
void LoadConfig(HMODULE self);

// Appends one line to hdrfix.log (printf-style). Safe to call from any thread/process.
void Log(const char* fmt, ...);
