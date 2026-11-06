// Forwards every version.dll export to the real DLL in the system directory.
//
// The real DLL is loaded on the first forwarded call, never from DllMain.
// Each forwarder takes and passes eight pointer-sized arguments. On x64 the
// first four travel in registers and the rest on the stack, so passing eight
// covers every version.dll function (the largest, VerInstallFile/VerFindFile,
// take exactly eight), whatever their real types: all are integers/pointers.
#include <windows.h>
#include <cstdint>

namespace {

HMODULE RealVersionDll()
{
    static const HMODULE module = [] {
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16)
            return HMODULE{};
        wcscat_s(path, L"\\version.dll");
        return LoadLibraryW(path);
    }();
    return module;
}

using Fn = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                              uintptr_t, uintptr_t, uintptr_t, uintptr_t);

Fn Resolve(const char* name)
{
    const HMODULE real = RealVersionDll();
    return real ? reinterpret_cast<Fn>(GetProcAddress(real, name)) : nullptr;
}

} // namespace

#define HDRFIX_FORWARD(name)                                                              \
    extern "C" uintptr_t WINAPI Fwd_##name(uintptr_t a, uintptr_t b, uintptr_t c,         \
                                           uintptr_t d, uintptr_t e, uintptr_t f,         \
                                           uintptr_t g, uintptr_t h)                      \
    {                                                                                     \
        static const Fn real = Resolve(#name);                                            \
        if (!real) {                                                                      \
            SetLastError(ERROR_PROC_NOT_FOUND);                                           \
            return 0;                                                                     \
        }                                                                                 \
        return real(a, b, c, d, e, f, g, h);                                              \
    }

HDRFIX_FORWARD(GetFileVersionInfoA)
HDRFIX_FORWARD(GetFileVersionInfoByHandle)
HDRFIX_FORWARD(GetFileVersionInfoExA)
HDRFIX_FORWARD(GetFileVersionInfoExW)
HDRFIX_FORWARD(GetFileVersionInfoSizeA)
HDRFIX_FORWARD(GetFileVersionInfoSizeExA)
HDRFIX_FORWARD(GetFileVersionInfoSizeExW)
HDRFIX_FORWARD(GetFileVersionInfoSizeW)
HDRFIX_FORWARD(GetFileVersionInfoW)
HDRFIX_FORWARD(VerFindFileA)
HDRFIX_FORWARD(VerFindFileW)
HDRFIX_FORWARD(VerInstallFileA)
HDRFIX_FORWARD(VerInstallFileW)
HDRFIX_FORWARD(VerQueryValueA)
HDRFIX_FORWARD(VerQueryValueW)
