#include "engine/diagnostics/ProcessMemory.hpp"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    // After windows.h, which it needs
    #include <psapi.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    #include <fstream>
    #include <sstream>
    #include <string>
    #include <unistd.h>
#endif

namespace N2Engine::Diagnostics
{
    std::optional<ProcessMemory> QueryProcessMemory()
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
            return std::nullopt;
        }
        ProcessMemory memory;
        memory.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
        memory.peakWorkingSetBytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
        return memory;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
        // statm: sizes in pages; the second is the resident set
        std::ifstream statm("/proc/self/statm");
        unsigned long long totalPages = 0;
        unsigned long long residentPages = 0;
        if (!(statm >> totalPages >> residentPages))
        {
            return std::nullopt;
        }
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0)
        {
            return std::nullopt;
        }
        ProcessMemory memory;
        memory.workingSetBytes = static_cast<std::uint64_t>(residentPages) * static_cast<std::uint64_t>(pageSize);

        // status: "VmHWM:    12345 kB", the resident set's high-water mark
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line))
        {
            if (line.rfind("VmHWM:", 0) == 0)
            {
                std::istringstream fields(line.substr(6));
                unsigned long long kilobytes = 0;
                if (fields >> kilobytes)
                {
                    memory.peakWorkingSetBytes = static_cast<std::uint64_t>(kilobytes) * 1024u;
                }
                break;
            }
        }
        return memory;
#else
        return std::nullopt;
#endif
    }
}
