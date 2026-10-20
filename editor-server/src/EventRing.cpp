#include "editor-server/EventRing.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace N2Engine::Editor
{
    EventRing::EventRing(const size_t maxEvents, const size_t maxBytes)
        : _maxEvents(std::max<size_t>(maxEvents, 1)), _maxBytes(maxBytes), _epoch(NewEpoch()),
          _publicEpoch(NewPublicEpoch(0, _epoch))
    {
    }

    uint32_t EventRing::Push(const std::string_view kind, nlohmann::json fields,
                             const Logger::LogSensitivity sensitivity)
    {
        nlohmann::json event = fields.is_object() ? std::move(fields) : nlohmann::json{{"value", std::move(fields)}};
        event["kind"] = std::string(kind);
        // What it will cost on the wire, roughly (seq is added under the lock); invalid UTF-8 is replaced, as WriteJson
        // replaces it, rather than throwing here
        const size_t bytes = event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size() +
                             EventOverheadBytes;
        return Store(std::move(event), bytes, sensitivity);
    }

    uint32_t EventRing::PushLog(const Logger::LogLevel level, const std::string_view message,
                                const int64_t unixMilliseconds, const Logger::LogSensitivity sensitivity)
    {
        std::string text = TruncateUtf8(message, MaxLogMessageBytes);
        size_t bytes = text.size() + EventOverheadBytes;

        // What a PublicOnly reader gets: only a line tagged Public reaches one, and with its host paths scrubbed. Kept
        // beside the full event only when the text changed.
        std::optional<nlohmann::json> publicEvent;
        if (sensitivity == Logger::LogSensitivity::Public)
        {
            std::string scrubbed = ScrubHostPaths(text);
            if (scrubbed != text)
            {
                bytes += scrubbed.size();
                publicEvent = nlohmann::json{
                    {"kind", LogKind},
                    {"level", std::string(LevelName(level))},
                    {"message", std::move(scrubbed)},
                    {"time", unixMilliseconds},
                };
            }
        }

        nlohmann::json event = {
            {"kind", LogKind},
            {"level", std::string(LevelName(level))},
            {"message", std::move(text)},
            {"time", unixMilliseconds},
        };
        return Store(std::move(event), bytes, sensitivity, std::move(publicEvent));
    }

    uint32_t EventRing::Store(nlohmann::json event, const size_t bytes, const Logger::LogSensitivity sensitivity,
                              std::optional<nlohmann::json> publicEvent)
    {
        std::lock_guard lock(_mutex);
        // Past the uint32 range seqs can't go on: a new epoch, numbered from 1, tells every reader to start over
        if (_lastSeq >= MaxSeq)
        {
            _droppedTotal += _entries.size();
            ClearLocked();
            _lastSeq = 0;
            _lastPublicSeq = 0;
            _epoch = NewEpoch(_epoch);
            _publicEpoch = NewPublicEpoch(_publicEpoch, _epoch);
        }

        // _lastSeq moves only once the event is stored: if anything here throws (out of memory), the seq isn't used
        const auto seq = static_cast<uint32_t>(_lastSeq + 1);
        const bool isPublic = sensitivity == Logger::LogSensitivity::Public;
        const auto publicSeq = isPublic ? static_cast<uint32_t>(_lastPublicSeq + 1) : 0u;
        event["seq"] = seq;
        _entries.push_back({seq, publicSeq, std::move(event), std::move(publicEvent), bytes, isPublic});
        _lastSeq = seq;
        if (isPublic)
            _lastPublicSeq = publicSeq;
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

    EventBatch EventRing::Read(uint32_t afterSeq, const uint32_t maxEvents, const uint32_t epoch,
                               const EventView view) const
    {
        EventBatch batch;
        std::lock_guard lock(_mutex);
        if (view == EventView::PublicOnly)
        {
            ReadPublicLocked(batch, afterSeq, maxEvents, epoch);
            return batch;
        }
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

    void EventRing::ReadPublicLocked(EventBatch &batch, uint32_t afterSeq, const uint32_t maxEvents,
                                     const uint32_t epoch) const
    {
        batch.epoch = _publicEpoch;
        // As in Read, but over the Public view's own numbering (publicSeq, consecutive over the Public events)
        if ((epoch != 0 && epoch != _publicEpoch) || afterSeq > _lastPublicSeq)
            afterSeq = 0;

        // The oldest Public event still held says how many after afterSeq fell off
        uint64_t oldest = _lastPublicSeq + 1;
        for (const Entry &entry : _entries)
        {
            if (entry.isPublic)
            {
                oldest = entry.publicSeq;
                break;
            }
        }
        const uint64_t after = afterSeq;
        if (oldest > after + 1)
            batch.dropped = static_cast<uint32_t>(oldest - after - 1);

        const size_t limit = std::min<size_t>(maxEvents, MaxEventsPerRead);
        uint32_t lastReturned = 0;
        for (const Entry &entry : _entries)
        {
            if (batch.events.size() >= limit)
                break;
            if (!entry.isPublic || entry.publicSeq <= after)
                continue;
            nlohmann::json event = entry.publicEvent.has_value() ? *entry.publicEvent : entry.event;
            event["seq"] = entry.publicSeq;
            batch.events.push_back(std::move(event));
            lastReturned = entry.publicSeq;
        }
        batch.nextSeq = batch.events.empty() ? static_cast<uint32_t>(_lastPublicSeq) : lastReturned;
    }

    uint32_t EventRing::LastSeq(const EventView view) const
    {
        std::lock_guard lock(_mutex);
        return static_cast<uint32_t>(view == EventView::PublicOnly ? _lastPublicSeq : _lastSeq);
    }

    uint32_t EventRing::Epoch(const EventView view) const
    {
        std::lock_guard lock(_mutex);
        return view == EventView::PublicOnly ? _publicEpoch : _epoch;
    }

    void EventRing::SetHostRoots(std::vector<std::string> roots)
    {
        std::lock_guard lock(_rootsMutex);
        _hostRoots = std::move(roots);
    }

    std::string EventRing::ScrubHostPaths(const std::string_view text) const
    {
        std::vector<std::string> roots;
        {
            std::lock_guard lock(_rootsMutex);
            roots = _hostRoots;
        }
        return ScrubHostPaths(text, roots);
    }

    namespace
    {
        bool IsSpace(const char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        bool IsAlpha(const char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        bool IsDigit(const char c)
        {
            return c >= '0' && c <= '9';
        }

        bool IsSeparator(const char c)
        {
            return c == '/' || c == '\\';
        }

        /// What may stand right before the start of a path in a line of text (a colon may not, so res://x and URLs
        /// are never taken for a path)
        bool IsPathBoundary(const char c)
        {
            return IsSpace(c) || c == '"' || c == '\'' || c == '`' || c == '(' || c == '[' || c == '{' || c == '=' ||
                   c == ',' || c == ';' || c == '<' || c == '>' || c == '|';
        }

        bool EndsPath(const char c)
        {
            return IsSpace(c) || c == '"' || c == '\'' || c == '`' || c == '<' || c == '>' || c == '|' || c == ')' ||
                   c == ']' || c == '}' || c == ',' || c == ';';
        }

        /// Where a path that has run from before `i` ends: at a character that ends one, at a colon that is not part
        /// of a name (a line number follows it, or nothing), and not at a space that is followed by a word holding a
        /// separator (a folder named "Nicholas Cerisano" is part of the path)
        size_t ScanPathEnd(const std::string_view s, size_t i)
        {
            while (i < s.size())
            {
                const char c = s[i];
                if (c == ':' && (i + 1 >= s.size() || IsDigit(s[i + 1]) || IsSpace(s[i + 1])))
                    break;
                if (c == ' ')
                {
                    size_t k = i + 1;
                    bool hasSeparator = false;
                    while (k < s.size() && !EndsPath(s[k]))
                    {
                        hasSeparator = hasSeparator || IsSeparator(s[k]);
                        ++k;
                    }
                    if (hasSeparator && k > i + 1)
                    {
                        i = k;
                        continue;
                    }
                    break;
                }
                if (EndsPath(c))
                    break;
                ++i;
            }
            return i;
        }

        bool IsNameChar(const char c)
        {
            return !IsSpace(c) && !IsSeparator(c) && c != '"' && c != '\'';
        }

        /// "/name/more": a separator, a name, a separator and another name
        bool IsPosixPathStart(const std::string_view s, const size_t i)
        {
            if (i + 1 >= s.size() || s[i] != '/' || !IsNameChar(s[i + 1]))
                return false;
            size_t j = i + 1;
            while (j < s.size() && IsNameChar(s[j]))
                ++j;
            return j + 1 < s.size() && s[j] == '/' && IsNameChar(s[j + 1]);
        }

        void ReplaceRoot(std::string &text, const std::string &root)
        {
            size_t at = 0;
            while ((at = text.find(root, at)) != std::string::npos)
            {
                const size_t after = at + root.size();
                size_t length = root.size();
                if (after < text.size() && IsSeparator(text[after]))
                {
                    ++length;
                }
                else if (after < text.size() && !EndsPath(text[after]) && text[after] != ':')
                {
                    // A longer name that only starts with the root ("/proj" in "/project2")
                    at = after;
                    continue;
                }
                text.replace(at, length, "res://");
                at += 6;
            }
        }
    }

    std::string EventRing::ScrubHostPaths(const std::string_view text, const std::vector<std::string> &roots)
    {
        std::string replaced(text);
        for (std::string root : roots)
        {
            while (!root.empty() && IsSeparator(root.back()))
                root.pop_back();
            if (root.empty())
                continue;
            // The folder as it is written with either separator
            std::string forward = root;
            std::string backward = root;
            std::ranges::replace(forward, '\\', '/');
            std::ranges::replace(backward, '/', '\\');
            ReplaceRoot(replaced, forward);
            if (backward != forward)
                ReplaceRoot(replaced, backward);
        }

        const std::string_view s = replaced;
        std::string result;
        result.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            size_t end = std::string_view::npos;
            if (i == 0 || IsPathBoundary(s[i - 1]))
            {
                const char c = s[i];
                if (IsAlpha(c) && i + 3 < s.size() && s[i + 1] == ':' && IsSeparator(s[i + 2]) &&
                    !(s[i + 2] == '/' && s[i + 3] == '/'))
                {
                    end = ScanPathEnd(s, i + 3);
                }
                else if (c == '\\' && i + 2 < s.size() && s[i + 1] == '\\' && IsNameChar(s[i + 2]))
                {
                    end = ScanPathEnd(s, i + 2);
                }
                else if (IsPosixPathStart(s, i))
                {
                    end = ScanPathEnd(s, i + 1);
                }
            }
            if (end != std::string_view::npos)
            {
                result += HiddenPathText;
                i = end;
                continue;
            }
            result += s[i];
            ++i;
        }
        return result;
    }

    void EventRing::SetLastSeqForTesting(const uint32_t lastSeq)
    {
        std::lock_guard lock(_mutex);
        ClearLocked();
        _lastSeq = lastSeq;
        _lastPublicSeq = 0;
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

    uint32_t EventRing::NewPublicEpoch(const uint32_t previous, const uint32_t fullEpoch)
    {
        // Never the full view's epoch: a client that changes view must restart rather than reuse a seq
        uint32_t epoch = NewEpoch(previous);
        while (epoch == fullEpoch)
            epoch = NewEpoch(previous);
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
