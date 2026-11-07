#include "engine/diagnostics/FrameStats.hpp"

#include <algorithm>
#include <cmath>

namespace N2Engine::Diagnostics
{
    void FrameStats::SetEnabled(const bool enabled)
    {
        if (enabled && !_enabled)
        {
            _head = 0;
            _count = 0;
            _frameOpen = false;
            _newestOpen = false;
            _hasLastStart = false;
        }
        if (!enabled)
        {
            _frameOpen = false;
            _newestOpen = false;
        }
        _enabled = enabled;
    }

    void FrameStats::BeginFrame(const Clock::time_point now)
    {
        // The frame before is complete: what follows it now belongs to this one
        _newestOpen = false;
        if (!_enabled)
        {
            // A host's frame number is for the frame about to run, enabled or not
            _nextFrame.reset();
            return;
        }
        _open = FrameRecord{};
        _open.frame = _nextFrame.value_or(_lastFrame + 1);
        _nextFrame.reset();
        if (_hasLastStart)
        {
            _open.intervalMs = static_cast<float>(std::chrono::duration<double, std::milli>(now - _lastStart).count());
        }
        _lastStart = now;
        _hasLastStart = true;
        _openStart = now;
        _frameOpen = true;
    }

    void FrameStats::EndFrame(const Clock::time_point now)
    {
        if (!_enabled || !_frameOpen)
        {
            return;
        }
        _open.tickMs = static_cast<float>(std::chrono::duration<double, std::milli>(now - _openStart).count());
        _ring[_head] = _open;
        _head = (_head + 1) % HistoryFrames;
        _count = std::min(_count + 1, HistoryFrames);
        _lastFrame = _open.frame;
        _frameOpen = false;
        _newestOpen = true;
    }

    void FrameStats::AddTo(FrameRecord &record, const FrameSection section, const float milliseconds)
    {
        switch (section)
        {
        case FrameSection::Fixed:
            record.fixedMs += milliseconds;
            break;
        case FrameSection::Physics:
            record.physicsMs += milliseconds;
            break;
        case FrameSection::Update:
            record.updateMs += milliseconds;
            break;
        case FrameSection::Audio:
            record.audioMs += milliseconds;
            break;
        case FrameSection::Render:
            record.renderMs += milliseconds;
            break;
        case FrameSection::Commands:
            record.commandsMs += milliseconds;
            break;
        case FrameSection::RenderRequest:
            record.renderRequestMs += milliseconds;
            break;
        case FrameSection::Readback:
            record.readbackMs += milliseconds;
            break;
        }
    }

    void FrameStats::Add(const FrameSection section, const double milliseconds)
    {
        if (!_enabled || !_frameOpen)
        {
            return;
        }
        AddTo(_open, section, static_cast<float>(milliseconds));
    }

    void FrameStats::NoteFixedTick()
    {
        if (_enabled && _frameOpen)
        {
            ++_open.fixedTicks;
        }
    }

    void FrameStats::AddToNewestFrame(const FrameSection section, const double milliseconds)
    {
        if (!_enabled || !_newestOpen || _count == 0)
        {
            return;
        }
        AddTo(_ring[(_head + HistoryFrames - 1) % HistoryFrames], section, static_cast<float>(milliseconds));
    }

    const FrameRecord &FrameStats::At(const std::size_t index) const
    {
        // The oldest held is _count records before the next slot
        return _ring[(_head + HistoryFrames - _count + index) % HistoryFrames];
    }

    std::optional<std::uint32_t> FrameStats::NewestFrame() const
    {
        if (_count == 0)
        {
            return std::nullopt;
        }
        return At(_count - 1).frame;
    }

    std::vector<FrameRecord> FrameStats::FramesAfter(const std::uint32_t sinceFrame) const
    {
        std::vector<FrameRecord> frames;
        if (_count == 0)
        {
            return frames;
        }
        // A client ahead of the newest frame saw another run of frames (a new play session): it gets them all
        const bool restarted = sinceFrame > At(_count - 1).frame;
        frames.reserve(_count);
        for (std::size_t i = 0; i < _count; ++i)
        {
            const FrameRecord &record = At(i);
            if (restarted || record.frame > sinceFrame)
            {
                frames.push_back(record);
            }
        }
        return frames;
    }

    FrameSummary FrameStats::Summarize() const
    {
        FrameSummary summary;
        summary.windowFrames = _count;
        if (_count == 0)
        {
            return summary;
        }

        std::vector<float> ticks;
        ticks.reserve(_count);
        double tickTotal = 0.0;
        double intervalTotal = 0.0;
        std::size_t intervals = 0;
        for (std::size_t i = 0; i < _count; ++i)
        {
            const FrameRecord &record = At(i);
            ticks.push_back(record.tickMs);
            tickTotal += record.tickMs;
            if (record.intervalMs > 0.0f)
            {
                intervalTotal += record.intervalMs;
                ++intervals;
            }
        }
        std::ranges::sort(ticks);
        summary.tickMs.avg = static_cast<float>(tickTotal / static_cast<double>(_count));
        summary.tickMs.max = ticks.back();
        // Nearest rank: the smallest value at least 95% of the frames are at or below (the small offset keeps 0.95 * 20,
        // which is not exact in binary, from rounding up to the next rank)
        const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(_count) - 1e-9));
        summary.tickMs.p95 = ticks[std::max<std::size_t>(rank, 1) - 1];
        if (intervals > 0 && intervalTotal > 0.0)
        {
            summary.fps = static_cast<float>(1000.0 * static_cast<double>(intervals) / intervalTotal);
        }
        return summary;
    }
}
