// Everything except kernel32 is delay-loaded (see CMakeLists.txt). All of those are
// Windows DLLs, so load them from System32 only: never from TeamSpeak's folder, which
// is user-writable, or from the current directory.
#include <windows.h>
#include <delayimp.h>

namespace {

FARPROC WINAPI DelayLoadHook(unsigned notification, PDelayLoadInfo info)
{
    if (notification == dliNotePreLoadLibrary && info && info->szDll)
        return reinterpret_cast<FARPROC>(LoadLibraryExA(info->szDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    return nullptr; // default behaviour
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = DelayLoadHook;
