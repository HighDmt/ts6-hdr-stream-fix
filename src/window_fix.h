#pragma once

// Fixes single-window ("app") sharing on HDR monitors.
//
// TeamSpeak captures one window with GDI: PrintWindow(PW_RENDERFULLCONTENT) or
// BitBlt from the window's DC into a 32-bpp DIB. For windows rendered in HDR
// (Chrome, Edge, games, ... use scRGB swap chains when HDR is on) Windows
// clips that to 0..1 at 80 nits, so the result is ~3x too bright and the lost
// detail can't be recovered from the GDI pixels.
//
// After TeamSpeak's call, this captures the same window with
// Windows.Graphics.Capture in FP16 (unclipped), converts it with the same
// shader as the desktop path, and overwrites the pixels GDI just wrote.
// It only kicks in for windows captured repeatedly (a stream, not a one-off
// thumbnail) on an HDR monitor.
//
// Call after MinHook is initialized.
void InstallWindowCaptureHooks();
