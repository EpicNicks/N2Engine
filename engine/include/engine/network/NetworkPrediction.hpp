#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>

#include <math/Vector3.hpp>

#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/physics/CharacterController.hpp"

namespace N2Engine::Network
{
    class NetworkObject;
    class NetworkServer;

    // Client prediction, reconciliation and smoothing for the engine-owned character controller (netcode stage B6).
    //
    // What is predicted: the movement of the objects the client owns that have a CharacterController, and nothing else
    // (design 3.3). The step is CharacterMotor::Step, a pure function of (state, input, dt, scene queries), run by the
    // server with the owner's input and by the client with the same input a moment earlier; reconciling is "set the state
    // to the server's, run the same step over the inputs the server has not applied yet". The server is the authority: the
    // client's state is a guess that the next state update overwrites, and nothing the client computes is ever sent.
    //
    // Everything here is bounded: a history is a fixed array of kMaxPredictionHistory ticks, at most kMaxPredictedObjects
    // objects are predicted, and one reconciliation replays at most kMaxPredictionHistory steps, once per object per
    // NetworkClient::Update.

    /// The most ticks of input and predicted state one object remembers (the rollback window): 128 ticks is a little over
    /// two seconds at 60 Hz. A server state older than that cannot be replayed from and the client snaps to it.
    inline constexpr std::size_t kMaxPredictionHistory = 128;
    /// The most objects one client predicts
    inline constexpr std::size_t kMaxPredictedObjects = 8;

    /// How the networked input layout becomes a CharacterInput. Both ends use the same mapping, so the server's step and
    /// the client's prediction see the same input. The controls are looked up by name in the layout.
    struct CharacterInputMapping
    {
        /// A Vector2 control: x is the world x velocity and y the world z, each as a fraction of moveSpeed. A vector
        /// longer than 1 is cut to length 1, so a hostile client cannot move faster than moveSpeed whatever the layout's
        /// range allows. Empty: the character cannot move by input.
        std::string moveControl = "move";
        /// A Button control: a jump on the tick it is held. Empty: no jump.
        std::string jumpControl = "jump";
        /// World units per second at full deflection, 0 to CharacterMotor::kMaxMoveSpeed (clamped)
        float moveSpeed = 5.0f;
    };

    /// A CharacterInputMapping resolved against a layout: indices, no names. Cheap to copy and use every tick.
    class CharacterInputMap
    {
    public:
        CharacterInputMap() = default;

        /// Resolves `mapping` in `layout`. The error names the control that is missing or of the wrong kind.
        [[nodiscard]] static std::expected<CharacterInputMap, std::string> Create(const CharacterInputMapping &mapping,
                                                                                  const InputLayout &layout);

        /// The input for `values` (InputLayout::ValueCount() of them; fewer, or none, give no input). Non-finite values
        /// count as 0. Pure: the same values give the same input on every machine.
        [[nodiscard]] Physics::CharacterInput Map(std::span<const float> values) const;

        [[nodiscard]] float MoveSpeed() const noexcept { return _moveSpeed; }

    private:
        std::size_t _valueCount = 0;
        bool _hasMove = false;
        std::size_t _moveOffset = 0;
        bool _hasJump = false;
        std::size_t _jumpOffset = 0;
        float _moveSpeed = 0.0f;
    };

    /// The knobs of prediction (a field of NetworkClientOptions)
    struct PredictionOptions
    {
        /// Off by default: QueueInput only sends. On: it also steps the client's own CharacterControllers.
        bool enabled = false;
        CharacterInputMapping mapping;
        /// The step's dt in seconds: one fixed tick (1 / the server's tick rate). The server steps with the same value.
        float stepSeconds = 1.0f / 60.0f;
        /// The server's state and the predicted state at the same tick agree when their positions are within this many
        /// units, their vertical speeds within velocityEpsilon and they are both grounded or both not. The replicated
        /// state is quantised to a millimetre, so this must stay above that.
        float positionEpsilon = 0.01f;
        float velocityEpsilon = 0.05f;
        /// A correction larger than this many units (a teleport, a long rubber band) is not smoothed: the object snaps
        float snapDistance = 1.0f;
        /// A smaller correction is shown as an offset that falls to zero linearly over this long
        std::uint64_t smoothingMicros = 100'000;
        /// Ticks of history, 8 to kMaxPredictionHistory (clamped)
        std::uint32_t historyTicks = static_cast<std::uint32_t>(kMaxPredictionHistory);
        /// true: NetworkClient::Update moves the object to (state + the decaying offset) every update, so the object
        /// shows the smoothed position. false: the object shows the state and the game reads PredictionOffset itself.
        bool applySmoothing = true;

        [[nodiscard]] PredictionOptions Clamped() const noexcept;

        /// The options the project's network settings give: stepSeconds from tickRate, smoothingMicros from
        /// authoritative.smoothingMs and snapDistance from authoritative.snapThreshold. The rest (mapping, epsilons,
        /// history, enabled) keep the values of a default PredictionOptions: set them after.
        [[nodiscard]] static PredictionOptions FromSettings(const NetworkSettings &settings) noexcept;
    };

    /// The part of PredictionOptions that is safe to change while a client runs, because it only decides how an error is
    /// judged and shown, never what the step computes (that, the step size, the mapping and the history length, must stay
    /// what the server uses). NetworkClient::SetPredictionTuning applies it between two Updates; the values are clamped as
    /// PredictionOptions::Clamped does.
    struct PredictionTuning
    {
        float positionEpsilon = 0.01f;
        float velocityEpsilon = 0.05f;
        float snapDistance = 1.0f;
        std::uint64_t smoothingMicros = 100'000;
        bool applySmoothing = true;

        [[nodiscard]] static PredictionTuning Of(const PredictionOptions &options) noexcept;
        /// `options` with the tuning fields replaced by these (clamped)
        [[nodiscard]] PredictionOptions AppliedTo(const PredictionOptions &options) const noexcept;
    };

    struct PredictionStats
    {
        /// Frames of input stepped locally
        std::uint64_t inputsPredicted = 0;
        /// Server states compared with the history
        std::uint64_t comparisons = 0;
        /// ... that agreed
        std::uint64_t matches = 0;
        /// ... that did not: the state was set to the server's and the inputs after it replayed
        std::uint64_t corrections = 0;
        /// Corrections shown as a jump (a teleport, an error over snapDistance, or an overflow)
        std::uint64_t snaps = 0;
        /// The server's state was older than the history held: the object snapped to it and the history was dropped
        std::uint64_t windowOverflows = 0;
        /// Steps run again by corrections
        std::uint64_t replayedSteps = 0;
        /// A server state that could not be used: not finite, a tick this client never predicted (it names a tick after
        /// the newest, or none the history holds), or older than what was already confirmed
        std::uint64_t statesIgnored = 0;
        /// The largest position error a comparison found, in units
        float maxError = 0.0f;
    };

    enum class ReconcileOutcome : std::uint8_t
    {
        /// The server's state agreed with the prediction at its tick: history up to it was dropped
        Match,
        /// It did not: the state is the server's with the later inputs replayed on it
        Corrected,
        /// The tick is older than the history holds: the state is the server's, nothing replayed, history dropped
        Overflow,
        /// Not used (see PredictionStats::statesIgnored); nothing changed
        Ignored
    };

    struct ReconcileResult
    {
        ReconcileOutcome outcome = ReconcileOutcome::Ignored;
        /// The distance between the server's position and the predicted one at the tick (0 when not compared)
        float error = 0.0f;
        /// False for Corrected when the history held no sample at the tick (the same input applied twice by the server:
        /// the state was replayed from without a comparison)
        bool compared = false;
        std::uint32_t replayed = 0;
        /// Match: the predicted state at the tick. Corrected and Overflow: the new current state. Ignored: unused.
        Physics::CharacterState state;
    };

    /// The step a reconciliation replays: (state, input, dt) -> state
    using PredictionStepper =
        std::function<Physics::CharacterState(const Physics::CharacterState &, const Physics::CharacterInput &, float)>;

    /**
     * The ring buffer of one predicted object: for each local tick, the input and the state the step gave. No I/O, no
     * engine objects: the comparison and the replay are pure, so they are testable with a scripted stepper.
     *
     * Ticks are the stamps of the InputFrames (what the server echoes back) and are compared modulo 2^32.
     */
    class PredictionHistory
    {
    public:
        /// `capacity` is clamped to 8..kMaxPredictionHistory
        explicit PredictionHistory(std::size_t capacity = kMaxPredictionHistory);

        void Clear() noexcept;

        /// Records the input of `tick` and the state after its step. False (nothing recorded) when `tick` does not rise
        /// from the newest. The oldest sample is dropped when the history is full.
        bool Record(std::uint32_t tick, const Physics::CharacterInput &input, float dt, const Physics::CharacterState &after);

        /// The server says its state after the input stamped `ackTick` is `server`. See ReconcileOutcome. `step` is
        /// called once per sample after `ackTick` (at most Capacity() times). A server state that is not finite is Ignored.
        [[nodiscard]] ReconcileResult Reconcile(std::uint32_t ackTick, const Physics::CharacterState &server,
                                                const PredictionStepper &step, const PredictionOptions &options);

        [[nodiscard]] std::size_t Size() const noexcept { return _count; }
        [[nodiscard]] std::size_t Capacity() const noexcept { return _capacity; }
        [[nodiscard]] bool Empty() const noexcept { return _count == 0; }
        /// The newest sample's tick and state; nullptr when empty
        [[nodiscard]] const Physics::CharacterState *NewestState() const noexcept;
        [[nodiscard]] bool NewestTick(std::uint32_t &tick) const noexcept;
        /// The predicted state at `tick`, if the history holds it
        [[nodiscard]] const Physics::CharacterState *StateAt(std::uint32_t tick) const noexcept;

    private:
        struct Sample
        {
            std::uint32_t tick = 0;
            float dt = 0.0f;
            Physics::CharacterInput input;
            Physics::CharacterState state;
        };

        [[nodiscard]] Sample &At(const std::size_t index) noexcept { return _samples[(_head + index) % _capacity]; }
        [[nodiscard]] const Sample &At(const std::size_t index) const noexcept
        {
            return _samples[(_head + index) % _capacity];
        }
        void DropFront(std::size_t count) noexcept;

        std::array<Sample, kMaxPredictionHistory> _samples{};
        std::size_t _capacity;
        std::size_t _head = 0;
        std::size_t _count = 0;
        /// The newest tick of a sample dropped because the history was full (or by an overflow): states older than it
        /// cannot be replayed from
        bool _hasEvicted = false;
        std::uint32_t _evictedThrough = 0;
        /// The newest tick of a sample dropped because the server confirmed it
        bool _hasConfirmed = false;
        std::uint32_t _confirmedThrough = 0;
    };

    // ---- The server's side ------------------------------------------------------------------------------------------

    enum class ServerStepResult : std::uint8_t
    {
        /// The object's CharacterController took one step
        Stepped,
        /// The object has no CharacterController
        NoController,
        /// The step did not run: no physics backend, settings that are not valid or a dt that is not above 0
        NotRun
    };

    /**
     * One server tick of the CharacterController on `object`: the owner's input for `tick` (NetworkServer::InputFor, so a
     * missing frame is repeated or neutral by the server's policy) mapped by `map`, then the same step the client
     * predicts with. An object the server owns, or whose owner has left, steps with no input (gravity still acts). Marks
     * the controller as externally driven. Call it once per fixed tick, after NetworkServer::SetCurrentTick(tick) and
     * before NetworkServer::Update sends the state of that tick: the stamp of the input it applied goes into the state
     * updates, which is what clients reconcile at.
     */
    ServerStepResult StepServerCharacter(NetworkServer &server, NetworkObject &object, const CharacterInputMap &map,
                                         std::uint32_t tick, float dt);
}
