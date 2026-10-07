#include "editor-server/EventRing.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace N2Engine::Editor
{
    EventRing::EventRing(const size_t maxEvents, const size_t maxBytes)
        : _maxEvents(std::max<size_t>(maxEvents, 1)), _maxBytes(maxBytes), _epoch(NewEpoch())
    {
    }

    uint32_t EventRing::Push(const std::string_view kind, nlohmann::json fields)
    {
        nlohmann::json event = fields.is_object() ? std::move(fields) : nlohmann::json{{"value", std::move(fields)}};
        event["kind"] = std::string(kind);
        // What it will cost on the wire, roughly (seq is added under the lock); invalid UTF-8 is replaced, as WriteJson
        // replaces it, rather than throwing here
        const size_t bytes = event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size() +
                             EventOverheadBytes;
        return Store(std::move(event), bytes);
    }

    uint32_t EventRing::PushLog(const Logger::LogLevel level, const std::string_view message,
                                const int64_t unixMilliseconds)
    {
        std::string text = TruncateUtf8(message, MaxLogMessageBytes);
        const size_t bytes = text.size() + EventOverheadBytes;
        nlohmann::json event = {
            {"kind", LogKind},
            {"level", std::string(LevelName(level))},
            {"message", std::move(text)},
            {"time", unixMilliseconds},
        };
        return Store(std::move(event), bytes);
    }

    uint32_t EventRing::Store(nlohmann::json event, const size_t bytes)
    {
        std::lock_guard lock(_mutex);
        // Past the uint32 range seqs can't go on: a new epoch, numbered from 1, tells every reader to start over
        if (_lastSeq >= MaxSeq)
        {
            _droppedTotal += _entries.size();
            ClearLocked();
            _lastSeq = 0;
            _epoch = NewEpoch(_epoch);
        }

        // _lastSeq moves only once the event is stored: if anything here throws (out of memory), the seq isn't used
        const auto seq = static_cast<uint32_t>(_lastSeq + 1);
        event["seq"] = seq;
        _entries.push_back({seq, std::move(event), bytes});
        _lastSeq = seq;
        _bytes += bytes;

        // The oldest go first; the newest is always kept, even when it alone is over the byte limit
        while (_entries.size() > 1 && (_entries.size() > _maxEvents || _bytes > _maxBytes))
        {
            _bytes -= _entries.front().bytes;
            _entries.pop_front();
            ++_droppedTotal;
        }
        return seq;
    }

    EventBatch EventRing::Read(uint32_t afterSeq, const uint32_t maxEvents, const uint32_t epoch) const
    {
        EventBatch batch;
        std::lock_guard lock(_mutex);
        batch.epoch = _epoch;

        // afterSeq counts within an epoch: one from another (an earlier host process, or before the seqs started over)
        // says nothing about this one, so read from the start. A reader that doesn't know the epoch (0) can still pass
        // a seq this epoch hasn't reached, which can only come from another epoch too.
        if ((epoch != 0 && epoch != _epoch) || afterSeq > _lastSeq)
            afterSeq = 0;

        // Seqs are consecutive, so the oldest retained one says how many after afterSeq fell off (64-bit, so seq
        // MaxSeq + 1 doesn't wrap)
        const uint64_t oldest = _entries.empty() ? _lastSeq + 1 : _entries.front().seq;
        const uint64_t after = afterSeq;
        if (oldest > after + 1)
            batch.dropped = static_cast<uint32_t>(oldest - after - 1);

        const size_t first = after >= oldest ? static_cast<size_t>(after - oldest + 1) : 0;
        const size_t available = first < _entries.size() ? _entries.size() - first : 0;
        const size_t count = std::min<size_t>(available, std::min(maxEvents, MaxEventsPerRead));

        batch.events.reserve(count);
        for (size_t i = first; i < first + count; ++i)
        {
            batch.events.push_back(_entries[i].event);
        }
        batch.nextSeq = count > 0 ? _entries[first + count - 1].seq : static_cast<uint32_t>(_lastSeq);
        return batch;
    }

    uint32_t EventRing::LastSeq() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<uint32_t>(_lastSeq);
    }

    uint32_t EventRing::Epoch() const
    {
        std::lock_guard lock(_mutex);
        return _epoch;
    }

    void EventRing::SetLastSeqForTesting(const uint32_t lastSeq)
    {
        std::lock_guard lock(_mutex);
        ClearLocked();
        _lastSeq = lastSeq;
    }

    void EventRing::ClearLocked()
    {
        _entries.clear();
        _bytes = 0;
    }

    uint32_t EventRing::NewEpoch(const uint32_t previous)
    {
        // Random, so a host that restarts (or a ring that starts over) is told apart from the one before it
        static std::mutex generatorMutex;
        static std::mt19937 generator = []
        {
            std::random_device device;
            std::seed_seq seed{device(), device(),
                               static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
            return std::mt19937(seed);
        }();
        std::uniform_int_distribution<uint32_t> distribution(1, std::numeric_limits<uint32_t>::max());

        std::lock_guard lock(generatorMutex);
        uint32_t epoch = distribution(generator);
        while (epoch == previous)
            epoch = distribution(generator);
        return epoch;
    }

    size_t EventRing::Size() const
    {
        std::lock_guard lock(_mutex);
        return _entries.size();
    }

    uint64_t EventRing::DroppedTotal() const
    {
        std::lock_guard lock(_mutex);
        return _droppedTotal;
    }

    std::string_view EventRing::LevelName(const Logger::LogLevel level)
    {
        switch (level)
        {
        case Logger::LogLevel::Warn:
            return "warn";
        case Logger::LogLevel::Error:
            return "error";
        case Logger::LogLevel::Info:
        default:
            return "info";
        }
    }

    std::string EventRing::TruncateUtf8(std::string_view text, const size_t maxBytes)
    {
        if (text.size() <= maxBytes)
            return std::string(text);

        // Back up over UTF-8 continuation bytes (10xxxxxx), so the cut never splits a character
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        std::string result(text.substr(0, cut));
        result += "...";
        return result;
    }

    int64_t EventRing::NowUnixMilliseconds()
    {
        // system_clock is Unix time (since C++20)
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
}
