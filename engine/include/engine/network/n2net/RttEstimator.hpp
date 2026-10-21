#pragma once

#include <algorithm>
#include <cstdint>

namespace N2Engine::N2Net
{
    /// Round-trip time estimate and retransmission timeout as in RFC 6298 (smoothed RTT, variance, RTO = srtt + 4 *
    /// variance), in integer microseconds, clamped to [min, max]. Before the first sample the RTO is the initial one.
    class RttEstimator
    {
    public:
        RttEstimator(std::uint64_t initialRto, std::uint64_t minRto, std::uint64_t maxRto) noexcept
            : initialRto_(initialRto), minRto_(minRto), maxRto_(std::max(minRto, maxRto))
        {
        }

        void AddSample(std::uint64_t rttMicros) noexcept
        {
            // Far beyond any real round trip (about 12 days): clamped so the arithmetic below cannot wrap
            rttMicros = std::min<std::uint64_t>(rttMicros, 1ull << 40);
            latest_ = rttMicros;
            if (!hasSample_)
            {
                smoothed_ = rttMicros;
                variance_ = rttMicros / 2;
                min_ = rttMicros;
                hasSample_ = true;
                return;
            }
            const std::uint64_t error = smoothed_ > rttMicros ? smoothed_ - rttMicros : rttMicros - smoothed_;
            variance_ = (3 * variance_ + error) / 4;
            smoothed_ = (7 * smoothed_ + rttMicros) / 8;
            min_ = std::min(min_, rttMicros);
        }

        [[nodiscard]] bool HasSample() const noexcept { return hasSample_; }
        [[nodiscard]] std::uint64_t SmoothedMicros() const noexcept { return smoothed_; }
        [[nodiscard]] std::uint64_t VarianceMicros() const noexcept { return variance_; }
        [[nodiscard]] std::uint64_t LatestMicros() const noexcept { return latest_; }
        [[nodiscard]] std::uint64_t MinMicros() const noexcept { return min_; }

        [[nodiscard]] std::uint64_t RtoMicros() const noexcept
        {
            if (!hasSample_)
            {
                return std::clamp(initialRto_, minRto_, maxRto_);
            }
            // The clock granularity term of RFC 6298 is 1 ms here
            const std::uint64_t rto = smoothed_ + std::max<std::uint64_t>(1000, 4 * variance_);
            return std::clamp(rto, minRto_, maxRto_);
        }

    private:
        std::uint64_t initialRto_;
        std::uint64_t minRto_;
        std::uint64_t maxRto_;
        std::uint64_t smoothed_ = 0;
        std::uint64_t variance_ = 0;
        std::uint64_t latest_ = 0;
        std::uint64_t min_ = 0;
        bool hasSample_ = false;
    };
} // namespace N2Engine::N2Net
