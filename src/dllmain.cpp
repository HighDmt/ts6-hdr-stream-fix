// Entry point. DllMain decides whether this process is one to fix and, if so, starts
// the init thread; everything else runs there, outside the loader lock.
#include <windows.h>

#include "hooks.h"
#include "log_config.h"
#include "verbose_log.h"
#include "version.h"
#include "window_fix.h"

#include <atomic>
#include <cwchar>

namespace {

HMODULE g_self = nullptr;
HANDLE g_ready = nullptr; // signaled when init is done (hooked or not)
std::atomic<bool> g_hooked{false};

// TeamSpeak is a Chromium app: the same exe also runs as GPU, utility, renderer and
// crash-handler child processes (all with a --type= argument), and they all load
// version.dll. Screen capture happens in the main process only, so children get
// nothing but the version.dll forwarding: no thread, no file access, no hooks.
bool IsChromiumChildProcess() { return wcsstr(GetCommandLineW(), L"--type=") != nullptr; }

// Returns why hooks can't be installed in this process, or nullptr if they can.
const char* SkipReason()
{
    // Without win32k access, loading user32/dxgi would crash the process.
    PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY syscalls{};
    if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessSystemCallDisablePolicy, &syscalls,
                                   sizeof(syscalls)) &&
        syscalls.DisallowWin32kSystemCalls)
        return "win32k is locked down in this process";

    // MinHook needs to allocate executable memory.
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamicCode,
                                   sizeof(dynamicCode)) &&
        dynamicCode.ProhibitDynamicCode)
        return "dynamic code is prohibited in this process";
    return nullptr;
}

// Windows version for the log (GetVersionEx lies to apps without a manifest).
void LogWindowsVersion(const wchar_t* exeName)
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    auto rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (rtlGetVersion)
        rtlGetVersion(&v);
    Log("hdrfix %s loaded into %ls (Windows %lu.%lu build %lu)", HDRFIX_VERSION, exeName, v.dwMajorVersion,
        v.dwMinorVersion, v.dwBuildNumber);
}

DWORD WINAPI InitThread(void*)
{
    LoadConfig(g_self);

    // Only the file name: full paths would put the Windows user name into the log.
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* slash = wcsrchr(exe, L'\\');
    LogWindowsVersion(n ? (slash ? slash + 1 : exe) : L"?");

    if (!Cfg().enabled) {
        Log("disabled in hdrfix.ini (enabled=0)");
    } else if (const char* why = SkipReason()) {
        Log("not hooking: %s", why);
    } else {
        // Hooks point into this DLL: make sure it can never be unloaded.
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&InitThread), &pinned);
        g_hooked = InstallHooks();
        if (g_hooked) {
            InstallWindowCaptureHooks();
            if (Cfg().verboseLog)
                StartVerboseLog();
        }
    }
    SetEvent(g_ready);
    return 0;
}

} // namespace

// Exported for hdrtest: waits for init, returns 1 if the hooks are active.
extern "C" int WINAPI HdrFix_WaitReady(DWORD timeoutMs)
{
    WaitForSingleObject(g_ready, timeoutMs);
    return g_hooked ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        // (No DisableThreadLibraryCalls: the static CRT needs thread notifications.)
        g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (IsChromiumChildProcess()) {
            SetEvent(g_ready);
            return TRUE;
        }
        // The thread starts running once the loader lock is released.
        if (HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
            CloseHandle(thread);
        else
            SetEvent(g_ready);
    }
    return TRUE;
}
