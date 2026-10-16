#include "editor-server/LogCoalescer.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace N2Engine::Editor
{
    namespace
    {
        LogCoalescerOptions Clamped(LogCoalescerOptions options)
        {
            options.window = std::max(options.window, std::chrono::milliseconds(1));
            options.burst = std::max<uint32_t>(options.burst, 1);
            return options;
        }

        std::string Seconds(const std::chrono::milliseconds window)
        {
            if (window.count() % 1000 == 0)
            {
                return std::to_string(window.count() / 1000);
            }
            return std::format("{:.1f}", static_cast<double>(window.count()) / 1000.0);
        }
    }

    LogCoalescer::LogCoalescer(LogCoalescerOptions options, Clock clock)
        : _options(Clamped(options)), _clock(std::move(clock))
    {
    }

    LogCoalescer::TimePoint LogCoalescer::Now() const
    {
        return _clock ? _clock() : std::chrono::steady_clock::now();
    }

    LogCoalescer::Decision LogCoalescer::Record(const std::string_view kind)
    {
        const TimePoint now = Now();
        const std::lock_guard lock(_mutex);
        Decision decision;
        auto found = _windows.find(kind);
        if (found == _windows.end())
        {
            _windows.emplace(std::string(kind), Window{now, 1, 0});
            decision.write = true;
            return decision;
        }
        Window &window = found->second;
        if (now < window.start)
        {
            // The clock went backwards: the window now counts from the new time, with what it has counted so far
            window.start = now;
        }
        else if (now - window.start >= _options.window)
        {
            if (window.suppressed > 0)
            {
                decision.closed = Summary{found->first, window.suppressed, _options.window};
            }
            window = Window{now, 1, 0};
            decision.write = true;
            return decision;
        }
        if (window.written < _options.burst)
        {
            ++window.written;
            decision.write = true;
            return decision;
        }
        ++window.suppressed;
        ++_totalSuppressed;
        return decision;
    }

    std::vector<LogCoalescer::Summary> LogCoalescer::TakeClosed()
    {
        const TimePoint now = Now();
        const std::lock_guard lock(_mutex);
        std::vector<Summary> closed;
        for (auto it = _windows.begin(); it != _windows.end();)
        {
            Window &window = it->second;
            if (now < window.start)
            {
                window.start = now; // as in Record
                ++it;
                continue;
            }
            if (now - window.start < _options.window)
            {
                ++it;
                continue;
            }
            if (window.suppressed > 0)
            {
                closed.push_back(Summary{it->first, window.suppressed, _options.window});
            }
            it = _windows.erase(it);
        }
        return closed;
    }

    std::vector<LogCoalescer::Summary> LogCoalescer::TakeAll()
    {
        const std::lock_guard lock(_mutex);
        std::vector<Summary> all;
        for (const auto &[kind, window] : _windows)
        {
            if (window.suppressed > 0)
            {
                all.push_back(Summary{kind, window.suppressed, _options.window});
            }
        }
        _windows.clear();
        return all;
    }

    uint64_t LogCoalescer::TotalSuppressed() const
    {
        const std::lock_guard lock(_mutex);
        return _totalSuppressed;
    }

    std::string LogCoalescer::Describe(const Summary &summary, const uint32_t burst)
    {
        return std::format("{} more pre-auth failure(s) of kind '{}' in the last {} s were not logged one by one (at most "
                           "{} line(s) per kind every {} s are)",
                           summary.suppressed, summary.kind, Seconds(summary.window), burst, Seconds(summary.window));
    }
}
