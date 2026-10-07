#include "editor-server/EventRing.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace N2Engine::Editor
{
    EventRing::EventRing(const size_t maxEvents, const size_t maxBytes)
        : _maxEvents(std::max<size_t>(maxEvents, 1)), _maxBytes(maxBytes)
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
        const uint32_t seq = ++_lastSeq;
        event["seq"] = seq;
        _entries.push_back({seq, std::move(event), bytes});
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

    EventBatch EventRing::Read(uint32_t afterSeq, const uint32_t maxEvents) const
    {
        EventBatch batch;
        std::lock_guard lock(_mutex);

        // A seq this ring never issued comes from an earlier host process (seqs restart with the host): read from the
        // start, so a client that reconnects to a restarted host gets its backlog instead of waiting for seqs to catch up
        if (afterSeq > _lastSeq)
            afterSeq = 0;

        // Seqs are consecutive, so the oldest retained one says how many after afterSeq fell off
        const uint32_t oldest = _entries.empty() ? _lastSeq + 1 : _entries.front().seq;
        if (oldest > afterSeq + 1)
            batch.dropped = oldest - afterSeq - 1;

        const size_t first = afterSeq >= oldest ? static_cast<size_t>(afterSeq - oldest + 1) : 0;
        const size_t available = first < _entries.size() ? _entries.size() - first : 0;
        const size_t count = std::min<size_t>(available, std::min(maxEvents, MaxEventsPerRead));

        batch.events.reserve(count);
        for (size_t i = first; i < first + count; ++i)
        {
            batch.events.push_back(_entries[i].event);
        }
        batch.nextSeq = count > 0 ? _entries[first + count - 1].seq : _lastSeq;
        return batch;
    }

    uint32_t EventRing::LastSeq() const
    {
        std::lock_guard lock(_mutex);
        return _lastSeq;
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
