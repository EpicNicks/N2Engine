#include "engine/network/RollbackEvents.hpp"

#include <algorithm>
#include <vector>

namespace N2Engine::Network
{
    void RollbackEventQueue::BeginTick(const std::uint32_t tick)
    {
        _tick = tick;
        _occurrences.clear();
    }

    RollbackEventKey RollbackEventQueue::Emit(const std::uint32_t netId, const std::uint32_t eventId,
                                              const std::span<const std::uint8_t> payload)
    {
        RollbackEventKey key;
        key.tick = _tick;
        key.netId = netId;
        key.eventId = eventId;
        key.occurrence = _occurrences[std::make_tuple(netId, eventId, 0u)]++;
        ++_stats.emitted;

        const auto found = _log.find(key);
        if (found != _log.end())
        {
            Entry &entry = found->second;
            const bool same = entry.event.payload.size() == payload.size() &&
                              std::equal(payload.begin(), payload.end(), entry.event.payload.begin());
            if (same)
            {
                entry.seen = true;
                ++_stats.matched;
                return key;
            }
            // The same effect with other contents: the old one is taken back, this one is new
            if (entry.played && _cancel)
            {
                _cancel(entry.event);
                ++_stats.cancelled;
            }
            _log.erase(found);
        }

        Entry entry;
        entry.event.key = key;
        entry.event.payload.assign(payload.begin(), payload.end());
        entry.seen = true;
        if (_policy == RollbackEventPolicy::Immediate)
        {
            entry.played = true;
            ++_stats.played;
            if (_play)
            {
                _play(entry.event);
            }
        }
        _log.emplace(key, std::move(entry));
        return key;
    }

    void RollbackEventQueue::BeginResimulation(const std::uint32_t fromTick)
    {
        _resimulating = true;
        _resimFrom = fromTick;
        for (auto &entry : _log)
        {
            if (static_cast<std::int32_t>(entry.first.tick - fromTick) >= 0)
            {
                entry.second.seen = false;
            }
        }
    }

    void RollbackEventQueue::EndResimulation()
    {
        if (!_resimulating)
        {
            return;
        }
        _resimulating = false;
        for (auto it = _log.begin(); it != _log.end();)
        {
            if (!it->second.seen)
            {
                if (it->second.played)
                {
                    if (_cancel)
                    {
                        _cancel(it->second.event);
                    }
                    ++_stats.cancelled;
                }
                else
                {
                    ++_stats.dropped;
                }
                it = _log.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void RollbackEventQueue::Confirm(const std::uint32_t firstUnconfirmed)
    {
        for (auto it = _log.begin(); it != _log.end();)
        {
            if (static_cast<std::int32_t>(it->first.tick - firstUnconfirmed) >= 0)
            {
                // The log is ordered by tick, but a wrapped tick order is not: stop at the first unconfirmed one
                break;
            }
            if (!it->second.played)
            {
                it->second.played = true;
                ++_stats.played;
                if (_play)
                {
                    _play(it->second.event);
                }
            }
            it = _log.erase(it);
        }
    }
}
