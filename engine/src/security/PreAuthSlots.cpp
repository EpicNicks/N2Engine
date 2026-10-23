#include "engine/security/PreAuthSlots.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace N2Engine::Security
{
    namespace
    {
        void Bump(uint64_t &counter)
        {
            if (counter < std::numeric_limits<uint64_t>::max())
            {
                ++counter;
            }
        }
    }

    // ==================== Handle ====================

    PreAuthSlots::Handle::Handle(Handle &&other) noexcept
        : _owner(std::exchange(other._owner, nullptr)), _key(std::move(other._key)), _loopback(other._loopback),
          _usedReserve(other._usedReserve)
    {
    }

    PreAuthSlots::Handle &PreAuthSlots::Handle::operator=(Handle &&other) noexcept
    {
        if (this != &other)
        {
            Release();
            _owner = std::exchange(other._owner, nullptr);
            _key = std::move(other._key);
            _loopback = other._loopback;
            _usedReserve = other._usedReserve;
        }
        return *this;
    }

    PreAuthSlots::Handle::~Handle()
    {
        Release();
    }

    void PreAuthSlots::Handle::Release()
    {
        if (PreAuthSlots *const owner = std::exchange(_owner, nullptr))
        {
            owner->Release(_key, _loopback, _usedReserve);
        }
    }

    // ==================== PreAuthSlots ====================

    PreAuthSlots::PreAuthSlots(PreAuthSlotsOptions options, Clock clock) : _options(options), _clock(std::move(clock))
    {
        _options.totalSlots = std::clamp<size_t>(_options.totalSlots, 1, 16);
        _options.perAddressSlots = std::max<size_t>(_options.perAddressSlots, 1);
        _options.maxRecentAddresses = std::max<size_t>(_options.maxRecentAddresses, 1);
        _options.reservedSlots = std::min(_options.reservedSlots, _options.totalSlots - 1);
        _reserved = _options.reserveForRecentlyAuthenticated ? _options.reservedSlots : 0;
    }

    PreAuthSlots::TimePoint PreAuthSlots::Now() const
    {
        return _clock ? _clock() : std::chrono::steady_clock::now();
    }

    bool PreAuthSlots::IsRecentLocked(const std::string &key, const TimePoint now)
    {
        const auto found = _recent.find(key);
        if (found == _recent.end())
        {
            return false;
        }
        // A clock that went backwards gives a negative age, which is inside the window: the entry stays, and the table
        // is bounded either way
        if (now - found->second >= _options.recentAuthWindow)
        {
            _recent.erase(found);
            return false;
        }
        return true;
    }

    PreAuthSlots::Handle PreAuthSlots::TryAcquire(const PeerAddress &peer)
    {
        const TimePoint now = Now();
        const bool loopback = peer.IsLoopback();
        std::string key = peer.ToString();

        const std::lock_guard lock(_mutex);
        if (!loopback)
        {
            const auto found = _perAddress.find(key);
            if (found != _perAddress.end() && found->second >= _options.perAddressSlots)
            {
                Bump(_sinceSummary.refusedPerAddress);
                Bump(_total.refusedPerAddress);
                return {};
            }
        }

        // Everyone may use the open slots; only a recently authenticated non-loopback address may also use the reserve
        const size_t open = _options.totalSlots - _reserved;
        bool usedReserve = false;
        if (_inUse >= open)
        {
            const bool mayUseReserve = _reserved > 0 && !loopback && IsRecentLocked(key, now);
            if (!mayUseReserve || _inUse >= _options.totalSlots)
            {
                Bump(_sinceSummary.refusedFull);
                Bump(_total.refusedFull);
                return {};
            }
            usedReserve = true;
        }

        ++_inUse;
        if (!loopback)
        {
            ++_perAddress[key];
        }
        Bump(_sinceSummary.admitted);
        Bump(_total.admitted);
        return Handle(this, std::move(key), loopback, usedReserve);
    }

    void PreAuthSlots::Release(const std::string &key, const bool loopback, const bool /*usedReserve*/)
    {
        const std::lock_guard lock(_mutex);
        if (_inUse > 0)
        {
            --_inUse;
        }
        if (!loopback)
        {
            const auto found = _perAddress.find(key);
            if (found != _perAddress.end())
            {
                if (found->second <= 1)
                {
                    _perAddress.erase(found);
                }
                else
                {
                    --found->second;
                }
            }
        }
    }

    void PreAuthSlots::RecordAuthenticated(const PeerAddress &peer)
    {
        if (_reserved == 0 || peer.IsLoopback())
        {
            return;
        }
        const TimePoint now = Now();
        std::string key = peer.ToString();

        const std::lock_guard lock(_mutex);
        if (const auto found = _recent.find(key); found != _recent.end())
        {
            found->second = now;
            return;
        }
        if (_recent.size() >= _options.maxRecentAddresses)
        {
            // Forget what has expired; if the table is still full the oldest entry goes
            for (auto it = _recent.begin(); it != _recent.end();)
            {
                it = (now - it->second >= _options.recentAuthWindow) ? _recent.erase(it) : std::next(it);
            }
            if (_recent.size() >= _options.maxRecentAddresses)
            {
                auto oldest = _recent.begin();
                for (auto it = _recent.begin(); it != _recent.end(); ++it)
                {
                    if (it->second < oldest->second)
                    {
                        oldest = it;
                    }
                }
                _recent.erase(oldest);
            }
        }
        _recent.emplace(std::move(key), now);
    }

    PreAuthSlots::Counters PreAuthSlots::TakeCounters()
    {
        const std::lock_guard lock(_mutex);
        return std::exchange(_sinceSummary, Counters{});
    }

    PreAuthSlots::Counters PreAuthSlots::TotalCounters() const
    {
        const std::lock_guard lock(_mutex);
        return _total;
    }

    size_t PreAuthSlots::InUse() const
    {
        const std::lock_guard lock(_mutex);
        return _inUse;
    }

    size_t PreAuthSlots::InUse(const PeerAddress &peer) const
    {
        const std::lock_guard lock(_mutex);
        const auto found = _perAddress.find(peer.ToString());
        return found == _perAddress.end() ? 0 : found->second;
    }

    size_t PreAuthSlots::RecentAddressCount() const
    {
        const std::lock_guard lock(_mutex);
        return _recent.size();
    }
}
