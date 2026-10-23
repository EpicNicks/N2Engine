#include "engine/network/NetworkInterpolation.hpp"

#include <algorithm>
#include <cmath>

#include <math/DeterministicMath.hpp>

#include "engine/network/NetworkInput.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr double kMicrosPerSecond = 1'000'000.0;
        /// Render times beyond this are not a time any session reaches (and would not survive the cast to an integer)
        constexpr double kMaxRenderTick = 1.0e15;

        bool Finite(const Math::Vector3 &v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        bool SamePose(const SnapshotSample &a, const SnapshotSample &b) noexcept
        {
            return a.teleportSerial == b.teleportSerial && a.position.x == b.position.x && a.position.y == b.position.y &&
                   a.position.z == b.position.z && a.scale.x == b.scale.x && a.scale.y == b.scale.y &&
                   a.scale.z == b.scale.z && a.rotation.GetW() == b.rotation.GetW() &&
                   a.rotation.GetX() == b.rotation.GetX() && a.rotation.GetY() == b.rotation.GetY() &&
                   a.rotation.GetZ() == b.rotation.GetZ();
        }

        Math::Vector3 LerpVector(const Math::Vector3 &a, const Math::Vector3 &b, const float t) noexcept
        {
            return Math::Vector3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
        }

        /// Slerp along the shorter arc of two unit quaternions; falls back to a normalised lerp when they are nearly
        /// equal (where the sine of the angle between them is too small to divide by). The result is unit length.
        Math::Quaternion SlerpShortest(const Math::Quaternion &a, const Math::Quaternion &b, const float t) noexcept
        {
            const double aw = a.GetW();
            const double ax = a.GetX();
            const double ay = a.GetY();
            const double az = a.GetZ();
            const double bw = b.GetW();
            const double bx = b.GetX();
            const double by = b.GetY();
            const double bz = b.GetZ();
            double dot = aw * bw + ax * bx + ay * by + az * bz;
            const double sign = dot < 0.0 ? -1.0 : 1.0;
            dot = std::fabs(dot);

            double weightA = 1.0 - static_cast<double>(t);
            double weightB = static_cast<double>(t);
            if (dot < 0.9995)
            {
                const double theta = Math::Acos(std::min(dot, 1.0));
                const double sine = Math::Sin(theta);
                weightA = Math::Sin((1.0 - static_cast<double>(t)) * theta) / sine;
                weightB = Math::Sin(static_cast<double>(t) * theta) / sine;
            }
            weightB *= sign;

            const double w = aw * weightA + bw * weightB;
            const double x = ax * weightA + bx * weightB;
            const double y = ay * weightA + by * weightB;
            const double z = az * weightA + bz * weightB;
            const double lengthSquared = w * w + x * x + y * y + z * z;
            if (!(lengthSquared > 1.0e-18) || !std::isfinite(lengthSquared))
            {
                return Math::Quaternion::Identity;
            }
            const double inverse = 1.0 / std::sqrt(lengthSquared);
            return Math::Quaternion(static_cast<float>(w * inverse), static_cast<float>(x * inverse),
                                    static_cast<float>(y * inverse), static_cast<float>(z * inverse));
        }
    }

    InterpolationOptions InterpolationOptions::Clamped() const noexcept
    {
        InterpolationOptions clamped = *this;
        clamped.delayMicros = std::min<std::uint32_t>(clamped.delayMicros, 2'000'000u);
        clamped.maxExtrapolationMicros = std::min<std::uint32_t>(clamped.maxExtrapolationMicros, 1'000'000u);
        clamped.maxSamplesPerObject = std::clamp<std::size_t>(clamped.maxSamplesPerObject, 4, kMaxInterpolationSamples);
        return clamped;
    }

    SnapshotInterpolator::SnapshotInterpolator(InterpolationOptions options) : _options(options.Clamped()) {}

    void SnapshotInterpolator::NoteMessage(const std::uint32_t tick)
    {
        if (!_haveMessage)
        {
            _haveMessage = true;
            _messageTick = tick;
            return;
        }
        const std::int32_t difference = TickDifference(tick, _messageTick);
        if (difference == 0)
        {
            return;
        }
        if (difference > 0)
        {
            if (difference > kMaxMessageTickJump)
            {
                Clear();
                _messageTick = tick;
                ++_stats.resyncs;
                return;
            }
            _previousTick = _messageTick;
            _havePrevious = true;
            _messageTick = tick;
            return;
        }
        if (difference < -kMaxSampleAgeTicks)
        {
            Clear();
            _messageTick = tick;
            ++_stats.resyncs;
        }
        // Otherwise a message older than the newest (out of order): the stream's time stays where it is
    }

    void SnapshotInterpolator::Forget(const std::uint32_t id)
    {
        _objects.erase(id);
    }

    void SnapshotInterpolator::Clear()
    {
        _objects.clear();
        _havePrevious = false;
    }

    std::size_t SnapshotInterpolator::SampleCount(const std::uint32_t id) const
    {
        const auto found = _objects.find(id);
        return found == _objects.end() ? 0 : found->second.entries.size();
    }

    std::vector<std::uint32_t> SnapshotInterpolator::Ids() const
    {
        std::vector<std::uint32_t> ids;
        ids.reserve(_objects.size());
        for (const auto &entry : _objects)
        {
            ids.push_back(entry.first);
        }
        return ids;
    }

    SnapshotAdd SnapshotInterpolator::Add(const std::uint32_t id, const SnapshotSample &sample)
    {
        if (!Finite(sample.position) || !Finite(sample.scale))
        {
            ++_stats.samplesInvalid;
            return SnapshotAdd::Invalid;
        }
        const double w = sample.rotation.GetW();
        const double x = sample.rotation.GetX();
        const double y = sample.rotation.GetY();
        const double z = sample.rotation.GetZ();
        const double lengthSquared = w * w + x * x + y * y + z * z;
        if (!std::isfinite(lengthSquared) || !(lengthSquared > 1.0e-12))
        {
            ++_stats.samplesInvalid;
            return SnapshotAdd::Invalid;
        }
        if (!_haveMessage)
        {
            NoteMessage(sample.tick);
        }

        SnapshotSample incoming = sample;
        const double inverse = 1.0 / std::sqrt(lengthSquared);
        incoming.rotation = Math::Quaternion(static_cast<float>(w * inverse), static_cast<float>(x * inverse),
                                             static_cast<float>(y * inverse), static_cast<float>(z * inverse));

        const auto found = _objects.find(id);
        if (found == _objects.end() || found->second.entries.empty())
        {
            Buffer &buffer = _objects[id];
            buffer.entries.push_back(Entry{incoming, false});
            ++_stats.samplesAdded;
            return SnapshotAdd::Added;
        }

        std::deque<Entry> &entries = found->second.entries;
        const std::uint32_t newestTick = entries.back().sample.tick;
        if (TickDifference(incoming.tick, newestTick) < -kMaxSampleAgeTicks)
        {
            ++_stats.samplesStale;
            return SnapshotAdd::Stale;
        }
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            if (entries[index].sample.tick != incoming.tick)
            {
                continue;
            }
            // Another state for a tick already held: the message that brought it is newer than the one that brought the
            // first (state updates are applied in order), so it replaces it. A host that does not give the server its
            // tick sends tick 0 in every message, and then this is how the newest state is kept.
            entries[index].sample = incoming;
            entries[index].teleport =
                index > 0 && incoming.teleportSerial != entries[index - 1].sample.teleportSerial;
            if (index + 1 < entries.size())
            {
                entries[index + 1].teleport = entries[index + 1].sample.teleportSerial != incoming.teleportSerial;
            }
            ++_stats.samplesReplaced;
            return SnapshotAdd::Replaced;
        }

        // Where it goes: after the last sample that is not later than it
        std::size_t position = entries.size();
        while (position > 0 && TickBefore(incoming.tick, entries[position - 1].sample.tick))
        {
            --position;
        }

        if (position == entries.size())
        {
            const std::size_t count = entries.size();
            if (count >= 2 && SamePose(incoming, entries[count - 1].sample) &&
                SamePose(entries[count - 1].sample, entries[count - 2].sample))
            {
                // Still where it was: the static run ends later, it does not grow
                entries[count - 1].sample.tick = incoming.tick;
                ++_stats.samplesCoalesced;
                return SnapshotAdd::Added;
            }
            const SnapshotSample last = entries.back().sample;
            if (_havePrevious && TickBefore(last.tick, _previousTick) && TickBefore(_previousTick, incoming.tick))
            {
                // A message came after the last sample and before this one without the object in it: the object was where
                // the last sample says until at least then, so the move does not start before that
                SnapshotSample hold = last;
                hold.tick = _previousTick;
                entries.push_back(Entry{hold, false});
                ++_stats.holdsInserted;
            }
            const bool jump = incoming.teleportSerial != entries.back().sample.teleportSerial;
            entries.push_back(Entry{incoming, jump});
        }
        else
        {
            Entry entry{incoming, false};
            if (position > 0)
            {
                entry.teleport = incoming.teleportSerial != entries[position - 1].sample.teleportSerial;
            }
            entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(position), entry);
            // The sample after it is a jump from this one now, not from the one before
            const std::size_t after = position + 1;
            if (after < entries.size())
            {
                entries[after].teleport = entries[after].sample.teleportSerial != incoming.teleportSerial;
            }
        }
        ++_stats.samplesAdded;

        while (entries.size() > _options.maxSamplesPerObject)
        {
            entries.pop_front();
            ++_stats.samplesDropped;
        }
        return SnapshotAdd::Added;
    }

    std::optional<InterpolatedPose> SnapshotInterpolator::Evaluate(const std::uint32_t id, const double renderTick,
                                                                   const double tickRate)
    {
        const auto found = _objects.find(id);
        if (found == _objects.end() || found->second.entries.empty())
        {
            return std::nullopt;
        }
        std::deque<Entry> &entries = found->second.entries;
        ++_stats.evaluations;

        const std::size_t count = entries.size();
        const SnapshotSample &newest = entries.back().sample;
        const std::uint32_t newestTick = newest.tick;

        InterpolatedPose pose;
        pose.position = newest.position;
        pose.rotation = newest.rotation;
        pose.scale = newest.scale;
        pose.mode = PoseMode::Held;

        if (!std::isfinite(renderTick) || std::fabs(renderTick) > kMaxRenderTick || !std::isfinite(tickRate) ||
            !(tickRate > 0.0))
        {
            return pose;
        }

        // The render time relative to the newest sample, in ticks (negative: before it). The integer part is reduced
        // modulo 2^32 like every tick, so the render time and the samples need not share an origin.
        const double whole = std::floor(renderTick);
        const auto rawTick = static_cast<std::uint32_t>(static_cast<std::int64_t>(whole));
        const double relative = static_cast<double>(TickDifference(rawTick, newestTick)) + (renderTick - whole);
        const auto timeOf = [&](const std::size_t index)
        { return static_cast<double>(TickDifference(entries[index].sample.tick, newestTick)); };

        if (relative < timeOf(0))
        {
            const SnapshotSample &first = entries.front().sample;
            pose.position = first.position;
            pose.rotation = first.rotation;
            pose.scale = first.scale;
            pose.mode = PoseMode::BeforeFirst;
        }
        else if (relative >= 0.0)
        {
            // At or after the newest sample. The server sends an object only when it changed, so nothing newer means the
            // object is still there, unless the stream itself has not reached the render time.
            const double streamTime =
                _haveMessage ? std::max(0.0, static_cast<double>(TickDifference(_messageTick, newestTick))) : 0.0;
            if (relative > streamTime && count >= 2 && !entries[count - 1].teleport)
            {
                const double overTicks = relative - streamTime;
                const double limitTicks = static_cast<double>(_options.maxExtrapolationMicros) * tickRate / kMicrosPerSecond;
                const double used = std::min(overTicks, limitTicks);
                if (overTicks > limitTicks)
                {
                    ++_stats.extrapolationsClamped;
                }
                const SnapshotSample &before = entries[count - 2].sample;
                const double span = static_cast<double>(TickDifference(newestTick, before.tick));
                if (span > 0.0 && used > 0.0)
                {
                    const double scaleBy = used / span;
                    pose.position = Math::Vector3(
                        static_cast<float>(newest.position.x + (newest.position.x - before.position.x) * scaleBy),
                        static_cast<float>(newest.position.y + (newest.position.y - before.position.y) * scaleBy),
                        static_cast<float>(newest.position.z + (newest.position.z - before.position.z) * scaleBy));
                    pose.mode = PoseMode::Extrapolated;
                    ++_stats.extrapolations;
                }
            }
        }
        else
        {
            // Strictly between the first sample and the newest: the segment [index, index + 1] holds it
            std::size_t index = count - 2;
            while (index > 0 && timeOf(index) > relative)
            {
                --index;
            }
            const SnapshotSample &from = entries[index].sample;
            const SnapshotSample &to = entries[index + 1].sample;
            if (entries[index + 1].teleport)
            {
                pose.position = from.position;
                pose.rotation = from.rotation;
                pose.scale = from.scale;
                pose.mode = PoseMode::Teleport;
                ++_stats.teleportsHeld;
            }
            else
            {
                const double start = timeOf(index);
                const double span = timeOf(index + 1) - start;
                double alpha = span > 0.0 ? (relative - start) / span : 1.0;
                alpha = std::clamp(alpha, 0.0, 1.0);
                const float t = static_cast<float>(alpha);
                pose.position = LerpVector(from.position, to.position, t);
                pose.scale = LerpVector(from.scale, to.scale, t);
                pose.rotation = SlerpShortest(from.rotation, to.rotation, t);
                pose.mode = PoseMode::Interpolated;
            }
        }

        // Samples no later render time needs: everything before the segment the render time is in, keeping two samples
        // so the velocity of the last segment survives
        while (entries.size() > 2 && timeOf(1) <= relative)
        {
            entries.pop_front();
        }
        return pose;
    }
}
