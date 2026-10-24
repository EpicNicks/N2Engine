#include "engine/network/NetworkClock.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/network/NetworkInput.hpp"
#include "engine/network/n2net/ByteIO.hpp"

namespace N2Engine::Network
{
    namespace
    {
        using N2Net::ByteReader;
        using N2Net::ByteWriter;

        constexpr double kMicrosPerSecond = 1'000'000.0;

        /// The median of a sorted, non-empty list (the mean of the two middle values of an even one)
        double MedianOfSorted(const std::vector<double> &sorted) noexcept
        {
            const std::size_t count = sorted.size();
            if (count % 2 == 1)
            {
                return sorted[count / 2];
            }
            return (sorted[count / 2 - 1] + sorted[count / 2]) * 0.5;
        }
    }

    std::vector<std::uint8_t> Encode(const ClockPingMessage &message)
    {
        std::array<std::uint8_t, kClockPingBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kClockPingType);
        writer.U32(message.sequence);
        return std::vector<std::uint8_t>(buffer.data(), buffer.data() + writer.Size());
    }

    std::vector<std::uint8_t> Encode(const ClockPongMessage &message)
    {
        std::array<std::uint8_t, kClockPongBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kClockPongType);
        writer.U32(message.sequence);
        writer.U32(message.tick);
        writer.U32(message.tickMicros);
        writer.U16(message.tickRate);
        return std::vector<std::uint8_t>(buffer.data(), buffer.data() + writer.Size());
    }

    std::vector<std::uint8_t> Encode(const StreamTickMessage &message)
    {
        std::array<std::uint8_t, kStreamTickBytes> buffer{};
        ByteWriter writer(buffer.data(), buffer.size());
        writer.U8(kStreamTickType);
        writer.U32(message.tick);
        return std::vector<std::uint8_t>(buffer.data(), buffer.data() + writer.Size());
    }

    std::expected<StreamTickMessage, DecodeError> DecodeStreamTick(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        if (bytes[0] != kStreamTickType)
        {
            return std::unexpected(DecodeError::UnknownType);
        }
        if (bytes.size() < kStreamTickBytes)
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (bytes.size() > kStreamTickBytes)
        {
            return std::unexpected(DecodeError::TrailingBytes);
        }
        ByteReader reader(bytes);
        (void)reader.U8();
        StreamTickMessage message;
        message.tick = reader.U32();
        if (reader.Failed())
        {
            return std::unexpected(DecodeError::Truncated);
        }
        return message;
    }

    std::expected<ClockPingMessage, DecodeError> DecodeClockPing(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        if (bytes[0] != kClockPingType)
        {
            return std::unexpected(DecodeError::UnknownType);
        }
        if (bytes.size() < kClockPingBytes)
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (bytes.size() > kClockPingBytes)
        {
            return std::unexpected(DecodeError::TrailingBytes);
        }
        ByteReader reader(bytes);
        (void)reader.U8();
        ClockPingMessage message;
        message.sequence = reader.U32();
        if (reader.Failed())
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (message.sequence == 0)
        {
            return std::unexpected(DecodeError::BadValue);
        }
        return message;
    }

    std::expected<ClockPongMessage, DecodeError> DecodeClockPong(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        if (bytes[0] != kClockPongType)
        {
            return std::unexpected(DecodeError::UnknownType);
        }
        if (bytes.size() < kClockPongBytes)
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (bytes.size() > kClockPongBytes)
        {
            return std::unexpected(DecodeError::TrailingBytes);
        }
        ByteReader reader(bytes);
        (void)reader.U8();
        ClockPongMessage message;
        // One read per statement: the order of two reads in one expression is unspecified
        message.sequence = reader.U32();
        message.tick = reader.U32();
        message.tickMicros = reader.U32();
        message.tickRate = reader.U16();
        if (reader.Failed())
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (message.sequence == 0 || message.tickRate == 0 || message.tickRate > kMaxClockTickRate ||
            message.tickMicros > 1'000'000u / message.tickRate)
        {
            return std::unexpected(DecodeError::BadValue);
        }
        return message;
    }

    ClockSyncOptions ClockSyncOptions::Clamped() const noexcept
    {
        ClockSyncOptions clamped = *this;
        clamped.sampleCount = std::clamp<std::size_t>(clamped.sampleCount, 3, 64);
        // A NaN slew rate fails both comparisons of clamp and would stay NaN: take the default then
        clamped.maxSlewRate = std::isfinite(clamped.maxSlewRate) ? std::clamp(clamped.maxSlewRate, 0.001, 0.5) : 0.05;
        return clamped;
    }

    std::string_view ToString(const ClockSample result) noexcept
    {
        switch (result)
        {
        case ClockSample::Accepted:
            return "accepted";
        case ClockSample::BadTimes:
            return "bad times";
        case ClockSample::RttTooLarge:
            return "round trip too large";
        case ClockSample::BadTick:
            return "bad tick";
        case ClockSample::RateChanged:
            return "tick rate changed";
        }
        return "unknown";
    }

    ClockSync::ClockSync(ClockSyncOptions options) : _options(options.Clamped()) {}

    ClockSample ClockSync::AddSample(const std::uint64_t sendMicros, const std::uint64_t receiveMicros,
                                     const std::uint32_t serverTick, const std::uint32_t tickMicros,
                                     const std::uint16_t tickRate)
    {
        const auto reject = [this](const ClockSample why)
        {
            ++_stats.samplesRejected;
            return why;
        };
        if (receiveMicros < sendMicros)
        {
            return reject(ClockSample::BadTimes);
        }
        const std::uint64_t rtt = receiveMicros - sendMicros;
        if (rtt > _options.maxRttMicros)
        {
            return reject(ClockSample::RttTooLarge);
        }
        if (tickRate == 0 || tickRate > kMaxClockTickRate || tickMicros > 1'000'000u / tickRate)
        {
            return reject(ClockSample::BadTick);
        }
        if (_synced && tickRate != _tickRate)
        {
            return reject(ClockSample::RateChanged);
        }
        if (!_synced)
        {
            _tickRate = tickRate;
            _tickBase = serverTick;
        }

        // Unwrap the 32-bit tick against the newest unwrapped one: right across the wrap of the counter
        const std::int32_t relative = TickDifference(serverTick, static_cast<std::uint32_t>(_tickBase));
        const double extended = static_cast<double>(_tickBase) + static_cast<double>(relative);
        if (extended > static_cast<double>(_tickBase))
        {
            _tickBase = static_cast<std::uint64_t>(extended);
        }

        const double ticksPerMicro = static_cast<double>(tickRate) / kMicrosPerSecond;
        // The server's time when the pong arrived: its tick, the part of the tick that had passed, and half the round trip
        const double serverAtReceive =
            extended + (static_cast<double>(tickMicros) + static_cast<double>(rtt) * 0.5) * ticksPerMicro;
        const double clientAtReceive = static_cast<double>(receiveMicros) * ticksPerMicro;

        Sample sample;
        sample.rttMicros = rtt;
        sample.offsetTicks = serverAtReceive - clientAtReceive;
        _samples.push_back(sample);
        while (_samples.size() > _options.sampleCount)
        {
            _samples.pop_front();
        }
        _synced = true;
        ++_stats.samplesAccepted;
        Recompute();
        return ClockSample::Accepted;
    }

    void ClockSync::Recompute()
    {
        // Never empty here: called after a push
        std::vector<Sample> byRtt(_samples.begin(), _samples.end());
        std::stable_sort(byRtt.begin(), byRtt.end(),
                         [](const Sample &a, const Sample &b) { return a.rttMicros < b.rttMicros; });

        const std::size_t keep = (byRtt.size() + 1) / 2;
        std::vector<double> offsets;
        offsets.reserve(keep);
        for (std::size_t i = 0; i < keep; ++i)
        {
            offsets.push_back(byRtt[i].offsetTicks);
        }
        std::sort(offsets.begin(), offsets.end());
        _targetOffset = MedianOfSorted(offsets);

        std::vector<double> rtts;
        rtts.reserve(byRtt.size());
        for (const Sample &sample : byRtt)
        {
            rtts.push_back(static_cast<double>(sample.rttMicros));
        }
        // byRtt is sorted by round trip already
        const double median = MedianOfSorted(rtts);
        double deviation = 0.0;
        for (const double rtt : rtts)
        {
            deviation += std::fabs(rtt - median);
        }
        deviation /= static_cast<double>(rtts.size());
        _rttMicros = static_cast<std::uint64_t>(median + 0.5);
        _jitterMicros = static_cast<std::uint64_t>(deviation + 0.5);
    }

    double ClockSync::ServerTick(const std::uint64_t nowMicros)
    {
        if (!_synced)
        {
            return 0.0;
        }
        const double ticksPerMicro = static_cast<double>(_tickRate) / kMicrosPerSecond;
        if (!_haveOffset)
        {
            _offset = _targetOffset;
            _haveOffset = true;
            _lastQueryMicros = nowMicros;
        }
        else
        {
            const std::uint64_t elapsed = nowMicros > _lastQueryMicros ? nowMicros - _lastQueryMicros : 0;
            _lastQueryMicros = std::max(_lastQueryMicros, nowMicros);
            const double difference = _targetOffset - _offset;
            const double snapTicks = static_cast<double>(_options.snapThresholdMicros) * ticksPerMicro;
            if (std::fabs(difference) > snapTicks)
            {
                _offset = _targetOffset;
                ++_stats.snaps;
            }
            else
            {
                const double step = _options.maxSlewRate * static_cast<double>(elapsed) * ticksPerMicro;
                _offset += std::clamp(difference, -step, step);
            }
        }

        double value = static_cast<double>(nowMicros) * ticksPerMicro + _offset;
        if (_haveValue && value < _lastValue)
        {
            // Server time does not run backwards: it stands still until the estimate catches up
            value = _lastValue;
        }
        _lastValue = value;
        _haveValue = true;
        return value;
    }

    std::uint64_t ClockSync::ServerTimeMicros(const std::uint64_t nowMicros)
    {
        if (!_synced)
        {
            return 0;
        }
        const double ticks = ServerTick(nowMicros);
        if (!(ticks > 0.0))
        {
            return 0;
        }
        return static_cast<std::uint64_t>(ticks * kMicrosPerSecond / static_cast<double>(_tickRate));
    }
}
