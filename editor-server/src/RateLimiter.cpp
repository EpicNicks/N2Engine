#include "editor-server/RateLimiter.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <limits>
#include <utility>

namespace N2Engine::Editor
{
    // ==================== PeerAddress ====================

    PeerAddress PeerAddress::IPv4(const uint8_t a, const uint8_t b, const uint8_t c, const uint8_t d)
    {
        PeerAddress address;
        address._family = Family::IPv4;
        address._bytes[0] = a;
        address._bytes[1] = b;
        address._bytes[2] = c;
        address._bytes[3] = d;
        return address;
    }

    PeerAddress PeerAddress::IPv6(const std::array<uint8_t, 16> &bytes)
    {
        // ::ffff:a.b.c.d is an IPv4 peer that reached a dual-stack socket: the same peer as a.b.c.d
        bool mapped = bytes[10] == 0xFF && bytes[11] == 0xFF;
        for (size_t i = 0; i < 10 && mapped; ++i)
        {
            mapped = bytes[i] == 0;
        }
        if (mapped)
        {
            return IPv4(bytes[12], bytes[13], bytes[14], bytes[15]);
        }
        PeerAddress address;
        address._family = Family::IPv6;
        address._bytes = bytes;
        return address;
    }

    bool PeerAddress::IsLoopback() const
    {
        if (_family == Family::IPv4)
        {
            return _bytes[0] == 127;
        }
        if (_family == Family::IPv6)
        {
            for (size_t i = 0; i < 15; ++i)
            {
                if (_bytes[i] != 0)
                {
                    return false;
                }
            }
            return _bytes[15] == 1;
        }
        return false;
    }

    std::string PeerAddress::ToString() const
    {
        if (_family == Family::IPv4)
        {
            return std::format("{}.{}.{}.{}", _bytes[0], _bytes[1], _bytes[2], _bytes[3]);
        }
        if (_family != Family::IPv6)
        {
            return "unknown";
        }

        std::array<uint16_t, 8> groups{};
        for (size_t i = 0; i < 8; ++i)
        {
            groups[i] = static_cast<uint16_t>((_bytes[i * 2] << 8) | _bytes[i * 2 + 1]);
        }
        // The longest run of zero groups (the first of equals), at least two long, is written "::"
        size_t bestStart = 0;
        size_t bestLength = 0;
        for (size_t i = 0; i < 8;)
        {
            if (groups[i] != 0)
            {
                ++i;
                continue;
            }
            size_t end = i;
            while (end < 8 && groups[end] == 0)
            {
                ++end;
            }
            if (end - i > bestLength)
            {
                bestStart = i;
                bestLength = end - i;
            }
            i = end;
        }

        std::string text;
        for (size_t i = 0; i < 8; ++i)
        {
            if (bestLength >= 2 && i == bestStart)
            {
                text += "::";
                i += bestLength - 1;
                continue;
            }
            if (!text.empty() && !text.ends_with(':'))
            {
                text += ':';
            }
            text += std::format("{:x}", groups[i]);
        }
        return text;
    }

    // ==================== RateLimiter ====================

    RateLimiter::RateLimiter(RateLimitPolicy policy, Clock clock) : _policy(policy), _clock(std::move(clock))
    {
        _policy.maxEntries = std::max<size_t>(_policy.maxEntries, 1);
    }

    RateLimiter::TimePoint RateLimiter::Now() const
    {
        return _clock ? _clock() : std::chrono::steady_clock::now();
    }

    RateLimiter::Duration RateLimiter::BackoffFor(const uint32_t failures, const uint32_t freeFailures,
                                                  const Duration cap) const
    {
        if (failures <= freeFailures)
        {
            return Duration::zero();
        }
        // The failure after the free ones is the first backoff; each one after doubles it
        Duration backoff = _policy.initialBackoff;
        for (uint32_t doublings = failures - freeFailures - 1; doublings > 0 && backoff < cap; --doublings)
        {
            backoff *= 2;
        }
        return std::min(backoff, cap);
    }

    bool RateLimiter::IsIdle(const Entry &entry, const TimePoint now, const Duration idleReset)
    {
        return now >= entry.nextAllowed && now - entry.lastSeen >= idleReset;
    }

    RateLimiter::Entry &RateLimiter::Touch(std::map<std::string, Entry> &table, const std::string &key, const TimePoint now)
    {
        if (const auto found = table.find(key); found != table.end())
        {
            if (IsIdle(found->second, now, _policy.idleReset))
            {
                found->second = Entry{};
            }
            return found->second;
        }

        if (table.size() >= _policy.maxEntries)
        {
            // Forget what has gone quiet first
            for (auto it = table.begin(); it != table.end();)
            {
                it = IsIdle(it->second, now, _policy.idleReset) ? table.erase(it) : std::next(it);
            }
        }
        if (table.size() >= _policy.maxEntries)
        {
            // Still full: the least recently seen entry goes, one that is not serving a backoff before any that is
            auto victim = table.end();
            for (auto it = table.begin(); it != table.end(); ++it)
            {
                const bool backedOff = now < it->second.nextAllowed;
                const bool victimBackedOff = victim != table.end() && now < victim->second.nextAllowed;
                if (victim == table.end() || (victimBackedOff && !backedOff) ||
                    (victimBackedOff == backedOff && it->second.lastSeen < victim->second.lastSeen))
                {
                    victim = it;
                }
            }
            table.erase(victim);
        }
        return table.emplace(key, Entry{}).first->second;
    }

    RateLimiter::Admission RateLimiter::AdmitConnection(const PeerAddress &peer)
    {
        const TimePoint now = Now();
        Admission admission;

        if (peer.IsLoopback())
        {
            LoopbackState &state = _loopback;
            if (now >= state.nextAllowed && state.seen && now - state.lastSeen >= _policy.idleReset)
            {
                state = LoopbackState{};
            }
            if (now < state.nextAllowed)
            {
                admission.allowed = false;
                admission.retryAfter = std::chrono::duration_cast<Duration>(state.nextAllowed - now);
                state.suppressed = std::min(state.suppressed + 1, std::numeric_limits<uint32_t>::max() - 1);
                return admission;
            }
            admission.suppressed = std::exchange(state.suppressed, 0);
            return admission;
        }

        const auto found = _addresses.find(peer.ToString());
        if (found == _addresses.end())
        {
            return admission;
        }
        Entry &entry = found->second;
        if (IsIdle(entry, now, _policy.idleReset))
        {
            _addresses.erase(found);
            return admission;
        }
        if (now < entry.nextAllowed)
        {
            admission.allowed = false;
            admission.retryAfter = std::chrono::duration_cast<Duration>(entry.nextAllowed - now);
            entry.suppressed = std::min(entry.suppressed + 1, std::numeric_limits<uint32_t>::max() - 1);
            return admission;
        }
        admission.priorOffender = entry.failures > 0;
        admission.suppressed = std::exchange(entry.suppressed, 0);
        return admission;
    }

    bool RateLimiter::IsUserBackedOff(const std::string_view user)
    {
        const auto found = _users.find(std::string(user));
        return found != _users.end() && Now() < found->second.nextAllowed;
    }

    RateLimiter::FailureOutcome RateLimiter::RecordFailure(const PeerAddress &peer,
                                                           const std::optional<std::string_view> user)
    {
        const TimePoint now = Now();
        FailureOutcome outcome;

        if (peer.IsLoopback())
        {
            LoopbackState &state = _loopback;
            if (now >= state.nextAllowed && state.seen && now - state.lastSeen >= _policy.idleReset)
            {
                state = LoopbackState{};
            }
            if (!state.seen || now - state.windowStart >= _policy.loopbackWindow)
            {
                state.windowStart = now;
                state.windowFailures = 0;
            }
            state.seen = true;
            state.lastSeen = now;
            ++state.windowFailures;
            if (state.windowFailures >= _policy.loopbackFailures)
            {
                // A burst: the next one is counted from here, and the backoff doubles with every burst
                ++state.strikes;
                state.windowFailures = 0;
                state.windowStart = now;
                const Duration backoff = BackoffFor(state.strikes, 0, _policy.maxBackoff);
                state.nextAllowed = now + backoff;
                outcome.loopback = Backoff{backoff, state.strikes};
            }
        }
        else
        {
            Entry &entry = Touch(_addresses, peer.ToString(), now);
            entry.failures = std::min(entry.failures + 1, std::numeric_limits<uint32_t>::max() - 1);
            entry.lastSeen = now;
            const Duration backoff = BackoffFor(entry.failures, _policy.freeFailures, _policy.maxBackoff);
            if (backoff > Duration::zero())
            {
                entry.nextAllowed = now + backoff;
                outcome.address = Backoff{backoff, entry.failures};
            }
        }

        if (user.has_value() && !user->empty())
        {
            Entry &entry = Touch(_users, std::string(*user), now);
            entry.failures = std::min(entry.failures + 1, std::numeric_limits<uint32_t>::max() - 1);
            entry.lastSeen = now;
            const Duration backoff = BackoffFor(entry.failures, _policy.userFreeFailures, _policy.userMaxBackoff);
            if (backoff > Duration::zero())
            {
                entry.nextAllowed = now + backoff;
                outcome.user = Backoff{backoff, entry.failures};
            }
        }
        return outcome;
    }

    void RateLimiter::RecordSuccess(const PeerAddress &peer, const std::optional<std::string_view> user)
    {
        if (!peer.IsLoopback())
        {
            _addresses.erase(peer.ToString());
        }
        if (user.has_value() && !user->empty())
        {
            _users.erase(std::string(*user));
        }
    }
}
