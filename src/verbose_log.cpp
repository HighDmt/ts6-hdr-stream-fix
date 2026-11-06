#include "verbose_log.h"

#include "log_config.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace {

struct Counter {
    int count = 0;
    std::string detail;
};

std::atomic<bool> g_on{false};
std::mutex g_lock;
std::map<std::string, Counter> g_counts; // events of the current second

DWORD WINAPI FlushThread(void*)
{
    for (;;) {
        Sleep(1000);
        std::map<std::string, Counter> counts;
        {
            std::lock_guard lock(g_lock);
            counts.swap(g_counts);
        }
        if (counts.empty())
            continue;
        std::string line;
        for (const auto& [key, c] : counts) {
            line += (line.empty() ? "" : " | ") + key + " x" + std::to_string(c.count);
            if (!c.detail.empty())
                line += " (" + c.detail + ")";
        }
        Log("verbose: %s", line.c_str());
    }
}

} // namespace

void StartVerboseLog()
{
    if (g_on.exchange(true))
        return;
    if (HANDLE thread = CreateThread(nullptr, 0, FlushThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    Log("verbose logging is on (verbose_log=1): one summary line per second while capturing");
}

bool VerboseLogOn() { return g_on; }

void VerboseCount(const char* key, const char* detail)
{
    if (!g_on)
        return;
    std::lock_guard lock(g_lock);
    Counter& c = g_counts[key];
    ++c.count;
    if (detail)
        c.detail = detail;
}
