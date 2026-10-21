#include "engine/security/TokenBucket.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace N2Engine::Security
{
    namespace
    {
        constexpr int64_t MilliPerToken = 1000;
        // Rates and bursts are clamped here so no product below can overflow an int64
        constexpr uint32_t MaxRate = 1'000'000;
        constexpr uint32_t MaxBurst = 1'000'000;

        void Bump(uint64_t &counter)
        {
            if (counter < std::numeric_limits<uint64_t>::max())
            {
                ++counter;
            }
        }
    }

    TokenBucket::TokenBucket(TokenBucketOptions options, Clock clock) : _options(options), _clock(std::move(clock))
    {
        _options.globalPerSecond = std::clamp<uint32_t>(_options.globalPerSecond, 1, MaxRate);
        _options.perAddressPerSecond = std::clamp<uint32_t>(_options.perAddressPerSecond, 1, MaxRate);
        _options.globalBurst =
            std::clamp<uint32_t>(_options.globalBurst == 0 ? _options.globalPerSecond : _options.globalBurst, 1, MaxBurst);
        _options.perAddressBurst = std::clamp<uint32_t>(
            _options.perAddressBurst == 0 ? _options.perAddressPerSecond : _options.perAddressBurst, 1, MaxBurst);
        _options.maxAddresses = std::max<size_t>(_options.maxAddresses, 1);
        _globalCapacityMilli = static_cast<int64_t>(_options.globalBurst) * MilliPerToken;
        _addressCapacityMilli = static_cast<int64_t>(_options.perAddressBurst) * MilliPerToken;
    }

    TokenBucket::TimePoint TokenBucket::Now() const
    {
        return _clock ? _clock() : std::chrono::steady_clock::now();
    }

    void TokenBucket::Refill(Bucket &bucket, const TimePoint now, const int64_t capacityMilli, const int64_t perSecond)
    {
        if (!bucket.anchored)
        {
            // A new bucket starts full
            bucket.milliTokens = capacityMilli;
            bucket.last = now;
            bucket.anchored = true;
            return;
        }
        if (now <= bucket.last)
        {
            // No time passed, or the clock went backwards: nothing to add, and later time counts from here
            bucket.last = now;
            return;
        }
        const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.last).count();
        if (elapsedMs == 0)
        {
            // Under a millisecond: keep the anchor, so calls that come faster than that still add up
            return;
        }
        // perSecond tokens/s = perSecond milli-tokens per millisecond. Past the time that fills the bucket the exact
        // elapsed time doesn't matter, and capping it keeps the product small.
        const int64_t missing = capacityMilli - bucket.milliTokens;
        if (missing > 0)
        {
            const int64_t fillMs = missing / perSecond + 1;
            const int64_t usable = std::min(elapsedMs, fillMs);
            bucket.milliTokens = std::min(capacityMilli, bucket.milliTokens + usable * perSecond);
        }
        bucket.last = now;
    }

    void TokenBucket::MakeRoomLocked(const TimePoint now)
    {
        if (_addresses.size() < _options.maxAddresses)
        {
            return;
        }
        // A bucket that has refilled completely is the same as no bucket
        for (auto it = _addresses.begin(); it != _addresses.end();)
        {
            // On a copy: this look must not count as a use (the least recently used bucket is evicted below)
            Bucket projected = it->second;
            Refill(projected, now, _addressCapacityMilli, _options.perAddressPerSecond);
            it = (projected.milliTokens >= _addressCapacityMilli) ? _addresses.erase(it) : std::next(it);
        }
        if (_addresses.size() < _options.maxAddresses)
        {
            return;
        }
        auto oldest = _addresses.begin();
        for (auto it = _addresses.begin(); it != _addresses.end(); ++it)
        {
            if (it->second.last < oldest->second.last)
            {
                oldest = it;
            }
        }
        _addresses.erase(oldest);
    }

    bool TokenBucket::TryTake(const PeerAddress &peer)
    {
        const TimePoint now = Now();
        const bool loopback = peer.IsLoopback();

        const std::lock_guard lock(_mutex);
        Refill(_global, now, _globalCapacityMilli, _options.globalPerSecond);
        if (_global.milliTokens < MilliPerToken)
        {
            Bump(_counters.refusedGlobal);
            return false;
        }

        if (loopback)
        {
            _global.milliTokens -= MilliPerToken;
            Bump(_counters.allowed);
            return true;
        }

        std::string key = peer.ToString();
        auto found = _addresses.find(key);
        if (found == _addresses.end())
        {
            MakeRoomLocked(now);
            found = _addresses.emplace(std::move(key), Bucket{}).first;
        }
        Bucket &bucket = found->second;
        Refill(bucket, now, _addressCapacityMilli, _options.perAddressPerSecond);
        if (bucket.milliTokens < MilliPerToken)
        {
            Bump(_counters.refusedAddress);
            return false;
        }
        bucket.milliTokens -= MilliPerToken;
        _global.milliTokens -= MilliPerToken;
        Bump(_counters.allowed);
        return true;
    }

    TokenBucket::Counters TokenBucket::TotalCounters() const
    {
        const std::lock_guard lock(_mutex);
        return _counters;
    }

    size_t TokenBucket::AddressCount() const
    {
        const std::lock_guard lock(_mutex);
        return _addresses.size();
    }
}
