#pragma once
#include <windows.h>
#include <dxgi1_6.h>

// True for the color spaces Windows reports for an output in HDR mode: HDR10 (G2084/P2020)
// for the signal, scRGB (G10/P709) on some systems.
bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE cs);

// Finds the DXGI output that shows `monitor` and returns its description.
// Returns false if there is none (e.g. the monitor was just unplugged).
bool FindOutputForMonitor(HMONITOR monitor, DXGI_OUTPUT_DESC1& desc);

// Windows "SDR content brightness" (SDR white level) in nits for the display whose
// GDI name is gdiDeviceName (DXGI_OUTPUT_DESC::DeviceName, e.g. "\\.\DISPLAY1").
// Returns 0 if it can't be determined.
float QuerySdrWhiteNits(const wchar_t* gdiDeviceName);
