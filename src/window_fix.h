#pragma once

// Fixes single-window ("app") sharing on HDR monitors.
//
// TeamSpeak captures one window with GDI: PrintWindow(PW_RENDERFULLCONTENT) into a
// 32-bpp DIB. (It falls back to BitBlt only if PrintWindow fails; that isn't hooked.)
// For windows rendered in HDR (Chrome, Edge, games, ... use scRGB swap chains when
// HDR is on) Windows clips that to 0..1 at 80 nits, so the result is ~3x too bright
// and the lost detail can't be recovered from the GDI pixels.
//
// For windows captured repeatedly (a stream, not a one-off thumbnail) on an HDR
// monitor, this captures the same window with Windows.Graphics.Capture in FP16
// (unclipped) and converts it with the same shader as the desktop path. Its frames
// then replace PrintWindow's output: overwriting it while our capture starts up,
// and in place of the (slow) PrintWindow call once it's streaming.
//
// Call after MinHook is initialized.
void InstallWindowCaptureHooks();
