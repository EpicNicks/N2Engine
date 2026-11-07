#pragma once

#include <cstdint>
#include <optional>

namespace N2Engine::Diagnostics
{
    /// The process's memory as the platform reports it (EN5, the editor's Profiler)
    struct ProcessMemory
    {
        /// The memory the process has in RAM now: the working set on Windows (GetProcessMemoryInfo), the resident set on
        /// Linux (/proc/self/statm)
        std::uint64_t workingSetBytes = 0;
        /// The most it has had: PeakWorkingSetSize on Windows, VmHWM (/proc/self/status) on Linux; empty when the platform
        /// did not say
        std::optional<std::uint64_t> peakWorkingSetBytes;
    };

    /// Asks the platform. Empty where there is no query (macOS, the web build) or when it fails.
    [[nodiscard]] std::optional<ProcessMemory> QueryProcessMemory();
}
