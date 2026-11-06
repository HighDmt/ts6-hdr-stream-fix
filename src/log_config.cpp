#include "log_config.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {

Config g_config;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
SRWLOCK g_logLock = SRWLOCK_INIT;

constexpr LONGLONG kMaxLogBytes = 1 << 20; // start over once the log passes 1 MB

std::wstring DirOf(HMODULE module)
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring dir(path, n);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

float ReadFloat(const wchar_t* ini, const wchar_t* key, float fallback)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"hdrfix", key, L"", buf, 64, ini);
    return buf[0] ? static_cast<float>(_wtof(buf)) : fallback;
}

HANDLE OpenLog(const std::wstring& path)
{
    // FILE_APPEND_DATA makes each write an atomic append, even with several TeamSpeak instances.
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size.QuadPart > kMaxLogBytes) {
        CloseHandle(h);
        h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            SetFilePointer(h, 0, nullptr, FILE_END);
    }
    return h;
}

} // namespace

const Config& Cfg() { return g_config; }

void LoadConfig(HMODULE self)
{
    const std::wstring dir = DirOf(self);
    const std::wstring ini = dir + L"hdrfix.ini";
    const wchar_t* p = ini.c_str();

    g_config.enabled = GetPrivateProfileIntW(L"hdrfix", L"enabled", 1, p) != 0;
    g_config.sdrWhiteOverrideNits = std::max(0.0f, ReadFloat(p, L"sdr_white_override_nits", 0.0f));
    g_config.knee = std::clamp(ReadFloat(p, L"knee", Config{}.knee), 0.0f, 1.0f);
    g_config.peakOverrideNits = std::max(0.0f, ReadFloat(p, L"peak_override_nits", 0.0f));
    g_config.fixWindowCapture = GetPrivateProfileIntW(L"hdrfix", L"fix_window_capture", 1, p) != 0;
    g_config.log = GetPrivateProfileIntW(L"hdrfix", L"log", 1, p) != 0;
    g_config.verboseLog = g_config.log && GetPrivateProfileIntW(L"hdrfix", L"verbose_log", 0, p) != 0;

    if (!g_config.log)
        return;
    g_logFile = OpenLog(dir + L"hdrfix.log");
    if (g_logFile == INVALID_HANDLE_VALUE) {
        // e.g. TeamSpeak installed under Program Files, where the folder isn't writable.
        wchar_t temp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, temp);
        if (n > 0 && n < MAX_PATH)
            g_logFile = OpenLog(std::wstring(temp, n) + L"hdrfix.log");
    }
}

void Log(const char* fmt, ...)
{
    if (g_logFile == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2048];
    int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu] ", t.wYear,
                     t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                     GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    const int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);
    n = (m < 0) ? n : std::min<int>(n + m, sizeof(line) - 3);
    line[n++] = '\r';
    line[n++] = '\n';

    AcquireSRWLockExclusive(&g_logLock);
    DWORD written;
    WriteFile(g_logFile, line, static_cast<DWORD>(n), &written, nullptr);
    ReleaseSRWLockExclusive(&g_logLock);
}
