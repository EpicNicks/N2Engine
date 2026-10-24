#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

namespace N2Engine::Network
{
    /// The most samples one object keeps, whatever the options say
    inline constexpr std::size_t kMaxInterpolationSamples = 64;
    /// A sample older than the newest of its object by more than this many ticks is refused as stale
    inline constexpr std::int32_t kMaxSampleAgeTicks = 256;
    /// A message tick further ahead of the newest by more than this many ticks, or further behind by more than
    /// kMaxSampleAgeTicks, is a break in the server's tick count: the buffers are dropped and start again (a resync)
    inline constexpr std::int32_t kMaxMessageTickJump = 3600;

    struct InterpolationOptions
    {
        /// How far behind the estimated server time remote objects are shown (the setting
        /// network.authoritative.interpolationDelayMs). Clamped to 0 to 2 seconds.
        std::uint32_t delayMicros = 100'000;
        /// How long past its newest sample an object is extrapolated when the server's snapshots are late; beyond it the
        /// object stays where the clamp put it. Clamped to 0 to 1 second.
        std::uint32_t maxExtrapolationMicros = 250'000;
        /// Samples kept per object (the oldest are dropped). Clamped to 4 to kMaxInterpolationSamples.
        std::size_t maxSamplesPerObject = 32;

        [[nodiscard]] InterpolationOptions Clamped() const noexcept;
    };

    /// One server state of an object: where its NetworkTransform was at `tick`
    struct SnapshotSample
    {
        std::uint32_t tick = 0;
        Math::Vector3 position{0.0f, 0.0f, 0.0f};
        Math::Quaternion rotation = Math::Quaternion::Identity;
        Math::Vector3 scale{1.0f, 1.0f, 1.0f};
        /// NetworkTransform's teleport serial at `tick`: a sample whose serial differs from the one before it is a jump
        int teleportSerial = 0;
    };

    enum class SnapshotAdd : std::uint8_t
    {
        /// Put in the buffer
        Added,
        /// The buffer held a sample for this tick: the new one replaced it (the newer message wins)
        Replaced,
        /// Older than the newest by more than kMaxSampleAgeTicks
        Stale,
        /// A position, scale or rotation that is not a finite number (or a rotation of length 0)
        Invalid,
    };

    enum class PoseMode : std::uint8_t
    {
        /// Between two samples
        Interpolated,
        /// Before the first sample: shown as the first
        BeforeFirst,
        /// At or after the newest sample, and the server's later messages say nothing changed: shown as the newest
        Held,
        /// Held at the older sample of a jump, until the time of the jump
        Teleport,
        /// Past the newest sample with the server's messages late: moved on by its last velocity, at most the limit
        Extrapolated,
    };

    struct InterpolatedPose
    {
        Math::Vector3 position{0.0f, 0.0f, 0.0f};
        Math::Quaternion rotation = Math::Quaternion::Identity;
        Math::Vector3 scale{1.0f, 1.0f, 1.0f};
        PoseMode mode = PoseMode::Held;
    };

    struct InterpolationStats
    {
        std::uint64_t samplesAdded = 0;
        std::uint64_t samplesReplaced = 0;
        std::uint64_t samplesStale = 0;
        std::uint64_t samplesInvalid = 0;
        /// A sample that repeated its object's last state and moved the end of the static run instead of being kept
        std::uint64_t samplesCoalesced = 0;
        /// Dropped to stay within maxSamplesPerObject
        std::uint64_t samplesDropped = 0;
        /// A "still here" sample put before a move that follows messages without the object (so it does not start early)
        std::uint64_t holdsInserted = 0;
        std::uint64_t resyncs = 0;
        std::uint64_t evaluations = 0;
        std::uint64_t extrapolations = 0;
        /// An extrapolation that reached its limit
        std::uint64_t extrapolationsClamped = 0;
        std::uint64_t teleportsHeld = 0;
    };

    /**
     * The client's buffer of remote objects' states, and the pose to show at a render time (netcode stage B3). No I/O,
     * no clock, no engine objects: NetworkClient feeds it the transform of each object a StateUpdate changed, stamped
     * with the update's server tick, and asks for the pose of each object at (estimated server time - interpolation
     * delay).
     *
     * Per object a short list of samples in tick order (at most maxSamplesPerObject, the oldest dropped). A sample for a
     * tick already held replaces it (the newer message wins); one out of order is put in its place; one older than the newest by
     * more than kMaxSampleAgeTicks is refused. The ticks are 32-bit and compared modulo 2^32 (the same signed difference
     * as input), so a counter that wraps changes nothing; the render time is given as a number of ticks and reduced the
     * same way.
     *
     * Pose: between two samples position and scale are interpolated linearly and the rotation by slerp (the shorter way,
     * normalised; a rotation that is not finite or has no length is refused at the door). A sample whose teleport serial
     * differs from its predecessor's is a jump: the object stays at the predecessor until the time of the jump, then is at the
     * jump, never interpolated across. The server sends an object only when it changed, so no later sample means "still
     * there", not "late": past the newest sample the object is held, unless the server's newest message (any object) is
     * older than the render time, which is the only sign that snapshots are late. Then it is extrapolated by the velocity
     * of its last two samples, for the time since the newest message and no more than maxExtrapolationMicros, and then it
     * stays where the clamp put it. An extrapolation is a guess: a message that arrives with the object's real state
     * corrects it.
     *
     * Memory is bounded by the number of objects (as many as were given: NetworkClient gives only objects of its world,
     * so at most kMaxNetworkObjects) times maxSamplesPerObject, whatever the server sends.
     */
    class SnapshotInterpolator
    {
    public:
        explicit SnapshotInterpolator(InterpolationOptions options = {});

        /// A StateUpdate stamped `tick` is being applied: call once per message, before the samples it carries. Its tick
        /// is what tells the interpolator the stream has reached a time (so an object with no later sample is still, not
        /// late), and a tick that breaks the sequence (see kMaxMessageTickJump) drops every buffer.
        void NoteMessage(std::uint32_t tick);

        /// Puts the sample into object `id`'s buffer; sample.tick must be the tick of the message just noted
        SnapshotAdd Add(std::uint32_t id, const SnapshotSample &sample);

        /// Forgets the object (it was despawned)
        void Forget(std::uint32_t id);
        /// Forgets every object
        void Clear();

        /// The pose of object `id` at `renderTick` (server ticks, fractional, as ClockSync::ServerTick less the delay),
        /// `tickRate` ticks to the second; nullopt when the object has no sample. Also drops the samples no later time can
        /// need.
        [[nodiscard]] std::optional<InterpolatedPose> Evaluate(std::uint32_t id, double renderTick, double tickRate);

        [[nodiscard]] bool Has(std::uint32_t id) const { return _objects.contains(id); }
        [[nodiscard]] std::size_t ObjectCount() const noexcept { return _objects.size(); }
        [[nodiscard]] std::size_t SampleCount(std::uint32_t id) const;
        /// The ids that have samples, ascending
        [[nodiscard]] std::vector<std::uint32_t> Ids() const;
        [[nodiscard]] const InterpolationOptions &Options() const noexcept { return _options; }
        [[nodiscard]] const InterpolationStats &Stats() const noexcept { return _stats; }

    private:
        struct Entry
        {
            SnapshotSample sample;
            /// The sample is a jump from the one before it
            bool teleport = false;
        };

        struct Buffer
        {
            std::deque<Entry> entries;
        };

        InterpolationOptions _options;
        InterpolationStats _stats;
        std::map<std::uint32_t, Buffer> _objects;
        bool _haveMessage = false;
        /// The newest message tick, and the distinct one before it
        std::uint32_t _messageTick = 0;
        bool _havePrevious = false;
        std::uint32_t _previousTick = 0;
    };
}
