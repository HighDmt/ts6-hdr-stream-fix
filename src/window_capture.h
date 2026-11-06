#pragma once
#include <windows.h>
#include <dxgi1_6.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// An FP16 Windows.Graphics.Capture of one window, converted to SDR (BGRA8, sRGB-encoded)
// with the same shader as screen sharing, and read back to the CPU.
//
// The readback is double-buffered: Update() starts copying the newest frame and makes the
// one copied on the previous call current, so the caller (TeamSpeak's capture thread)
// doesn't wait for the GPU. The stream therefore runs one capture behind the window.
// Two exceptions wait: the very first frame (up to 250 ms, so it's used right away),
// and a GPU so busy that the previous copy isn't done yet (a frame is never dropped).
//
// Not thread-safe: the caller serializes all calls (window_fix.cpp holds its lock).
class WindowCapture {
public:
    // Starts capturing hwnd. On failure returns nullptr with `step` naming what failed
    // and `hr` its error code. `label` is only used in the log.
    static std::unique_ptr<WindowCapture> Start(HWND hwnd, const DXGI_OUTPUT_DESC1& output, const wchar_t* label,
                                                const char*& step, HRESULT& hr);
    ~WindowCapture();

    // Picks up new frames. Returns false (after logging why) if the capture is broken.
    bool Update();

    // The current frame: BGRA8 rows, top-down, `pitch` bytes apart, `bytes` readable in
    // total. data is null until the first frame. Valid until the next Update().
    struct Frame {
        const uint8_t* data = nullptr;
        UINT pitch = 0, width = 0, height = 0;
        size_t bytes = 0;
    };
    Frame Current() const;

private:
    struct Impl;
    explicit WindowCapture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
