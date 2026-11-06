#pragma once

// Windows "SDR content brightness" (SDR white level) in nits for the display whose
// GDI name is gdiDeviceName (DXGI_OUTPUT_DESC::DeviceName, e.g. "\\.\DISPLAY1").
// Returns 0 if it can't be determined.
float QuerySdrWhiteNits(const wchar_t* gdiDeviceName);
