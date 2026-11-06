#include "window_fix.h"

#include "display_info.h"
#include "log_config.h"
#include "verbose_log.h"
#include "window_capture.h"

#include <MinHook.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

constexpr int kActivateAfterCalls = 3; // captures within kActivateWindowMs that mean "stream"
constexpr ULONGLONG kActivateWindowMs = 2000;
constexpr ULONGLONG kIdleCloseMs = 3000; // stop our capture once TeamSpeak stops
constexpr ULONGLONG kMonitorCacheMs = 2000;
constexpr ULONGLONG kRetryMs = 5000; // after a failure, try again this much later

std::atomic<bool> g_crashed{false}; // set after a crash in our code: the fix stays off

// ---- Lock --------------------------------------------------------------------------
//
// One lock guards everything below (tracked windows, their captures, the monitor cache).
// It's an SRW lock with a per-thread "held" flag so the crash handler can release it:
// with /EHsc, destructors don't reliably run when an SEH exception (a crash) unwinds,
// and a lock left held would hang the next thread that captures.

SRWLOCK g_lock = SRWLOCK_INIT;
thread_local bool t_holdsLock = false;

class Lock {
public:
    Lock()
    {
        AcquireSRWLockExclusive(&g_lock);
        t_holdsLock = true;
    }
    ~Lock()
    {
        t_holdsLock = false;
        ReleaseSRWLockExclusive(&g_lock);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

// For the crash handler: releases the lock if this thread still holds it.
void ReleaseLockAfterCrash()
{
    if (t_holdsLock) {
        t_holdsLock = false;
        ReleaseSRWLockExclusive(&g_lock);
    }
}

std::wstring ClassOf(HWND hwnd)
{
    wchar_t cls[128] = L"?";
    GetClassNameW(hwnd, cls, 128);
    return cls;
}

// Verbose log only: counts an event for a window (class + handle, never its title).
void Verbose(const char* what, HWND hwnd, const char* detail = nullptr)
{
    if (!VerboseLogOn())
        return;
    char key[200];
    snprintf(key, sizeof(key), "%s %ls %p", what, ClassOf(hwnd).c_str(), static_cast<void*>(hwnd));
    VerboseCount(key, detail);
}

// ---- Monitors ----------------------------------------------------------------------

struct MonitorState {
    ULONGLONG checked = 0;
    bool hdr = false;
    DXGI_OUTPUT_DESC1 desc{};
};

// Whether a monitor is in HDR mode, and its DXGI output (cached briefly). Under g_lock.
const MonitorState& LookupMonitor(HMONITOR monitor)
{
    static auto& cache = *new std::map<HMONITOR, MonitorState>; // never destroyed (see window_capture.cpp)
    MonitorState& m = cache[monitor];
    const ULONGLONG now = GetTickCount64();
    if (m.checked && now - m.checked < kMonitorCacheMs)
        return m;
    m = MonitorState{now};
    if (FindOutputForMonitor(monitor, m.desc))
        m.hdr = IsHdrColorSpace(m.desc.ColorSpace);
    return m;
}

// ---- Windows TeamSpeak captures ------------------------------------------------------

struct Tracked {
    ULONGLONG burstStart = 0; // activation heuristic
    int burstCalls = 0;
    ULONGLONG lastUse = 0;
    ULONGLONG retryAfter = 0; // after a failure, leave GDI output alone until then
    std::wstring className;   // for the log, also after the window is gone
    HMONITOR monitor = nullptr; // where the capture was started
    std::unique_ptr<WindowCapture> capture;
};

// Captured recently, on HDR monitors. Under g_lock; never destroyed (see window_capture.cpp).
std::map<HWND, Tracked>& g_windows = *new std::map<HWND, Tracked>;

// Forgets windows (and closes our captures) once TeamSpeak stops asking for them.
DWORD WINAPI SweepThread(void*)
{
    for (;;) {
        Sleep(1000);
        Lock lock;
        if (g_crashed)
            return 0; // the state may be inconsistent: leave it alone
        const ULONGLONG now = GetTickCount64();
        for (auto it = g_windows.begin(); it != g_windows.end();) {
            if (now - it->second.lastUse > kIdleCloseMs) {
                if (it->second.capture)
                    Log("window fix: '%ls' is no longer captured, stopping", it->second.className.c_str());
                it = g_windows.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void StartSweeperOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (HANDLE thread = CreateThread(nullptr, 0, SweepThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    });
}

// Verbose log only: average colour of a DIB section on a coarse grid.
std::string DibAverage(const DIBSECTION& ds)
{
    const int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    double sum[4] = {};
    int n = 0;
    for (int gy = 1; gy < 10; ++gy)
        for (int gx = 1; gx < 10; ++gx) {
            const uint8_t* p = static_cast<const uint8_t*>(ds.dsBm.bmBits) + size_t(h * gy / 10) * ds.dsBm.bmWidthBytes +
                               size_t(w * gx / 10) * 4;
            for (int c = 0; c < 4; ++c)
                sum[c] += p[c];
            ++n;
        }
    char buf[64];
    snprintf(buf, sizeof(buf), "rgb %.0f %.0f %.0f a %.0f", sum[2] / n, sum[1] / n, sum[0] / n, sum[3] / n);
    return buf;
}

// Starts or updates our capture of hwnd. Returns it if it has a frame to use. Under g_lock.
WindowCapture* ActiveCapture(HWND hwnd, HMONITOR monitor, const MonitorState& mon, const char* diag)
{
    Tracked& t = g_windows[hwnd];
    const ULONGLONG now = GetTickCount64();
    t.lastUse = now;
    if (now < t.retryAfter) {
        Verbose("skip: waiting to retry", hwnd, diag);
        return nullptr;
    }

    if (!t.capture || t.monitor != monitor) {
        if (!t.capture) {
            if (now - t.burstStart > kActivateWindowMs) {
                t.burstStart = now;
                t.burstCalls = 0;
            }
            if (++t.burstCalls < kActivateAfterCalls) {
                Verbose("skip: not a stream (yet)", hwnd, diag);
                return nullptr; // probably a thumbnail, not a stream
            }
        }
        t.capture.reset();
        t.className = ClassOf(hwnd);
        t.monitor = monitor;
        const char* step = "?";
        HRESULT hr = E_FAIL;
        t.capture = WindowCapture::Start(hwnd, mon.desc, t.className.c_str(), step, hr);
        if (!t.capture) {
            Log("window fix: could not capture '%ls' (%s failed, hr=0x%08lX), retrying in %llu s",
                t.className.c_str(), step, static_cast<unsigned long>(hr), kRetryMs / 1000);
            t.retryAfter = now + kRetryMs;
            return nullptr;
        }
    }

    if (!t.capture->Update()) {
        Log("window fix: lost the capture of '%ls', retrying in %llu s", t.className.c_str(), kRetryMs / 1000);
        t.capture.reset();
        t.retryAfter = now + kRetryMs;
        return nullptr;
    }
    return t.capture.get();
}

// Overwrites what GDI just drew into dc's DIB with our converted capture of the window.
// (dstX, dstY, w, h): destination rectangle; (srcX, srcY): its origin in window coordinates
// (relative to GetWindowRect), which is what PrintWindow and window-DC BitBlt use.
void ReplacePixels(HWND hwnd, HDC dc, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
    if (!IsWindow(hwnd) || IsIconic(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
        return;

    DIBSECTION ds{};
    HGDIOBJ bitmap = GetCurrentObject(dc, OBJ_BITMAP);
    if (!bitmap || GetObjectW(bitmap, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
        Verbose("skip: not a 32-bit DIB", hwnd);
        return; // not a 32-bpp DIB section: not a capture
    }
    char diag[200] = "";
    if (VerboseLogOn())
        snprintf(diag, sizeof(diag), "dib %dx%d dst %d,%d src %d,%d gdi %s", ds.dsBm.bmWidth, ds.dsBm.bmHeight, dstX,
                 dstY, srcX, srcY, DibAverage(ds).c_str());

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    Lock lock;
    StartSweeperOnce();
    const MonitorState& mon = LookupMonitor(monitor);
    if (!mon.hdr) {
        g_windows.erase(hwnd); // e.g. moved to an SDR monitor: GDI is already correct there
        Verbose("skip: SDR monitor", hwnd, diag);
        return;
    }
    WindowCapture* capture = ActiveCapture(hwnd, monitor, mon, diag);
    if (!capture)
        return;
    const WindowCapture::Frame f = capture->Current();
    if (!f.data) {
        Verbose("skip: no frame yet", hwnd, diag);
        return;
    }

    // WGC frames start at the window's visible bounds; GDI coordinates at GetWindowRect,
    // which also includes the invisible resize borders.
    RECT wr, visible;
    GetWindowRect(hwnd, &wr);
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
        visible = wr;
    const int offX = wr.left + srcX - visible.left - dstX; // frame x = destination x + offX
    const int offY = wr.top + srcY - visible.top - dstY;

    const int dibW = ds.dsBm.bmWidth, dibH = ds.dsBm.bmHeight;
    const int x0 = std::max({dstX, 0, -offX});
    const int x1 = std::min({dstX + std::min(w, dibW), dibW, int(f.width) - offX});
    const int y0 = std::max({dstY, 0, -offY});
    const int y1 = std::min({dstY + std::min(h, dibH), dibH, int(f.height) - offY});
    if (x0 >= x1 || y0 >= y1) {
        Verbose("skip: no overlap", hwnd, diag);
        return;
    }

    // GDI copies the region straight out of the read-back frame. The source is described
    // as a top-down DIB that starts at the region's first row and is exactly as tall as the
    // region (so GDI's top-down/bottom-up row rules don't matter) and as wide as the row
    // pitch; the region's columns are picked with the source x. The destination bitmap's
    // own row order is GDI's business. (GetObject doesn't report it reliably anyway:
    // dsBmih.biHeight is positive for top-down DIBs too.)
    const int cw = x1 - x0, ch = y1 - y0;
    const int sx = x0 + offX, sy = y0 + offY; // region's top-left in the frame
    if (size_t(sy + ch) * f.pitch > f.bytes) {
        Verbose("skip: unexpected frame layout", hwnd, diag);
        return;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(f.pitch / 4);
    bi.bmiHeader.biHeight = -ch; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, x0, y0, cw, ch, sx, 0, cw, ch, f.data + size_t(sy) * f.pitch, &bi, DIB_RGB_COLORS, SRCCOPY);
    GdiFlush(); // the frame memory is only valid until the next update

    if (VerboseLogOn())
        Verbose("replaced", hwnd, (std::string(diag) + " -> ours " + DibAverage(ds)).c_str());
}

// ---- Hooks -------------------------------------------------------------------------

using PrintWindowFn = BOOL(WINAPI*)(HWND, HDC, UINT);
using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
PrintWindowFn g_origPrintWindow = nullptr;
BitBltFn g_origBitBlt = nullptr;
thread_local bool t_inFix = false; // our own GDI calls must not re-enter the fix

void ReplacePixelsCatch(const char* api, HWND hwnd, HDC dc, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
    try {
        ReplacePixels(hwnd, dc, dstX, dstY, w, h, srcX, srcY);
    } catch (const std::exception& e) {
        Log("window fix: unexpected error after %s: %s", api, e.what());
    } catch (...) {
        Log("window fix: unexpected error after %s", api);
    }
}

// Last line of defence: a crash in our code must not take TeamSpeak down. GDI's own output
// stays in place, the lock is released, and the fix stays off until TeamSpeak restarts.
void ReplacePixelsGuarded(const char* api, HWND hwnd, HDC dc, int dstX, int dstY, int w, int h, int srcX, int srcY)
{
    if (t_inFix || g_crashed || !Cfg().enabled || !Cfg().fixWindowCapture)
        return;
    t_inFix = true;
    __try {
        ReplacePixelsCatch(api, hwnd, dc, dstX, dstY, w, h, srcX, srcY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_crashed = true; // before releasing the lock: whoever gets it next must see this
        ReleaseLockAfterCrash();
        Log("window fix: crashed (exception 0x%08lX after %s), turned off until TeamSpeak restarts", GetExceptionCode(),
            api);
    }
    t_inFix = false;
}

BOOL WINAPI HookPrintWindow(HWND hwnd, HDC dc, UINT flags)
{
    const BOOL ok = g_origPrintWindow(hwnd, dc, flags);
    if (!t_inFix && VerboseLogOn()) {
        char what[48];
        snprintf(what, sizeof(what), "PrintWindow(flags %u)%s", flags, ok ? "" : " FAILED");
        Verbose(what, hwnd);
    }
    if (ok)
        ReplacePixelsGuarded("PrintWindow", hwnd, dc, 0, 0, INT_MAX, INT_MAX, 0, 0);
    return ok;
}

BOOL WINAPI HookBitBlt(HDC dst, int x, int y, int cx, int cy, HDC src, int x1, int y1, DWORD rop)
{
    const BOOL ok = g_origBitBlt(dst, x, y, cx, cy, src, x1, y1, rop);
    if (ok && src && (rop & ~CAPTUREBLT) == SRCCOPY && !t_inFix) {
        // Only blits from a window DC count; WindowFromDC is null for memory DCs.
        HWND hwnd = WindowFromDC(src);
        if (hwnd && hwnd != GetDesktopWindow()) {
            Verbose("BitBlt from window DC", hwnd);
            ReplacePixelsGuarded("BitBlt", hwnd, dst, x, y, cx, cy, x1, y1);
        }
    }
    return ok;
}

void Hook(const wchar_t* module, const char* name, void* detour, void** original)
{
    HMODULE m = LoadLibraryW(module);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
    const MH_STATUS st = target ? MH_CreateHook(target, detour, original) : MH_ERROR_FUNCTION_NOT_FOUND;
    const MH_STATUS en = st == MH_OK ? MH_EnableHook(target) : st;
    Log("hook %s: %s", name, en == MH_OK ? "installed" : MH_StatusToString(en));
}

} // namespace

void InstallWindowCaptureHooks()
{
    if (!Cfg().fixWindowCapture) {
        Log("window fix: disabled in hdrfix.ini (fix_window_capture=0)");
        return;
    }
    Hook(L"user32.dll", "PrintWindow", reinterpret_cast<void*>(&HookPrintWindow),
         reinterpret_cast<void**>(&g_origPrintWindow));
    Hook(L"gdi32.dll", "BitBlt", reinterpret_cast<void*>(&HookBitBlt), reinterpret_cast<void**>(&g_origBitBlt));
}
