#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/SimulationClock.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/LockstepProtocol.hpp"
#include "engine/network/n2net/BisectProtocol.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/HashTree.hpp"
#include "engine/network/n2net/Lockstep.hpp"
#include "engine/network/n2net/Transport.hpp"
#include "engine/replay/LockstepReplay.hpp"
#include "engine/replay/ReplayLog.hpp"

namespace N2Engine::Network
{
    /// The input layout of a JSON list of controls: [{"name": "move", "kind": "vector2", "bits": 8, "range": 1,
    /// "signed": true}, {"name": "jump", "kind": "button"}], the fields of Network.DeclareInputLayout. The error names the
    /// first thing wrong. Used by N2Server's --relay-layout and the test client.
    [[nodiscard]] std::expected<InputLayout, std::string> ParseInputLayoutJson(std::string_view text);

    enum class LockstepState : std::uint8_t
    {
        /// No relay peer yet (over the secure layer: the handshake is running)
        Connecting,
        /// Linked; the join is sent and the welcome is awaited
        Joining,
        /// Seated in a slot; waiting for the other players
        Lobby,
        /// Every slot joined: the simulation runs confirmed ticks
        Running,
        /// The relay ended the session (a desync, a stall, a leaver, a shutdown) and this client has finished with it:
        /// the report is written, the connection left. See StopInfo().
        Stopped,
        /// The relay refused the join; see Refusal()
        Refused,
        /// The relay went away, broke the protocol, or did not answer; see Error()
        Failed,
    };

    [[nodiscard]] std::string_view ToString(LockstepState state) noexcept;

    /// What the runtime tells its host
    struct LockstepEvent
    {
        enum class Type : std::uint8_t
        {
            Welcomed,
            Started,
            /// A slot was dropped from `tick` on (every peer applies it at that tick)
            SlotDropped,
            /// The relay stopped the session; `text` is the reason
            Stopped,
            /// The desync report was written to `text`
            ReportWritten,
            Refused,
            Failed,
        };

        Type type = Type::Welcomed;
        std::uint32_t slot = 0;
        std::uint32_t tick = 0;
        std::string text;
    };

    struct LockstepRuntimeOptions
    {
        std::uint32_t protocolId = 0;
        /// The input layout of the session: the same as the relay's, or the join is refused
        InputLayout layout;
        /// Names this peer in the desync report
        std::string peerName = "peer";
        /// The scene and seed written into the recorded replay (a scene name the golden runner knows makes the log
        /// replayable by RunReplay)
        std::string scene = "lockstep";
        std::uint32_t seed = 0;
        /// Where the n2desync report goes when the relay reports a mismatch; empty: not written
        std::string reportPath;
        /// A JSON object recorded in the report (engine version, scene hash, registry hash: whatever the host knows)
        std::string determinismFingerprintJson;
        /// No welcome this long after the runtime started: Failed
        std::uint64_t joinTimeoutMicros = 15'000'000;
        /// How often the client's own frames and its ack go to the relay
        std::uint64_t sendIntervalMicros = 4'000;
        /// Checkpoint trees kept for the report and for answering the other peer's bisection
        std::uint32_t historyDepth = 8;
        /// After its bisection a participant keeps answering the other peer's questions this long
        std::uint64_t desyncLingerMicros = 3'000'000;
        N2Net::BisectClientConfig bisect;
        N2Net::BisectResponderConfig responder;
        /// true: the runtime calls SimulationClock::BeginTick for every tick it runs (it then owns Time::GetTick());
        /// false (the default): the host's own loop owns the counter
        bool ownsTickCounter = false;

        /// The local player's values for the input of `tick`, in the layout's order (InputLayout::ValueCount of them): the
        /// same data a B4 InputFrame carries. Called once per tick the local slot needs an input for.
        std::function<std::vector<float>(std::uint32_t tick)> sampleInput;
        /// Runs one confirmed tick: every slot's input, identical on every peer. False ends the session as a failure.
        std::function<bool(const N2Net::TickInputs &inputs, std::uint32_t slotCount)> applyTick;
        /// The state-hash tree after `tick` ticks have run (BuildStateHashTree for a NetworkWorld, or the host's own)
        std::function<std::optional<N2Net::HashTree>(std::uint32_t tick)> buildTree;
        /// A readable name for a leaf the bisection reported (DescribeHashedField); optional
        std::function<std::string(std::uint32_t objectId, std::uint16_t fieldId)> describeField;
        std::function<void(const LockstepEvent &)> observer;
    };

    struct LockstepRuntimeStats
    {
        std::uint64_t ticksRun = 0;
        /// Passes of Update where a tick was due but a slot's input was missing: the simulation waited
        std::uint64_t stalls = 0;
        std::uint64_t bundlesSent = 0;
        std::uint64_t bundlesReceived = 0;
        std::uint64_t bundlesRejected = 0;
        std::uint64_t rootsSent = 0;
        std::uint64_t bisectRequestsServed = 0;
        std::uint64_t framesBehind = 0;
    };

    /// What the relay's Stop said, once the runtime has seen it
    struct LockstepStopInfo
    {
        StopReason reason = StopReason::Empty;
        std::uint32_t tick = 0;
        std::uint32_t slotA = 0;
        std::uint32_t slotB = 0;
        std::uint64_t rootA = 0;
        std::uint64_t rootB = 0;
        /// Desync only: what the bisection found (Kind::None when this peer was not in the pair or had no tree)
        N2Net::Divergence divergence;
        /// Desync only: why the bisection ended in Malformed (None when it did not)
        N2Net::BisectFailure bisectFailure = N2Net::BisectFailure::None;
        bool reportWritten = false;
        std::string reportError;
    };

    /**
     * The client side of the lockstep preset (netcode C3): it joins a LockstepRelay over a transport, and from then on
     * drives the simulation ONLY from confirmed ticks. Each Update:
     *   - pumps the connection to the relay (inputs of the other slots arrive as InputBundles, the relay acknowledges
     *     this client's own frames, DropSlot and Stop arrive on the reliable channel);
     *   - runs the SimulationClock (the A2 network-mode accumulator, SimulationMode::Deterministic) for the ticks that
     *     are due: for each, samples the local input (a B4 InputLayout payload, once per tick, never more than inputDelay
     *     ticks ahead of the simulation), submits it to the LockstepScheduler, and runs a tick only if the scheduler says
     *     every slot's input is known (TryAdvance). When it is not, the tick waits and the accumulator is reset, so a
     *     stall is not caught up in a burst;
     *   - after a tick whose number is a multiple of hashInterval, builds the state-hash tree, keeps it in a
     *     HashTreeHistory and sends its root to the relay;
     *   - records every confirmed tick as an n2replay version 2 row (LockstepRecorder), so the session can be replayed by
     *     the C2 golden runner.
     *
     * The desync flow (N14: stop, never try to continue): when the relay's Stop names a mismatch, the simulation stops at
     * once (no further tick runs), the two peers it names bisect against each other through the relay
     * (NetworkedHashTreePeer over a BisectClient whose messages ride To/From envelopes, and a BisectResponder answers the
     * other's questions), each writes the n2desync report with the input log, keeps answering for desyncLingerMicros, and
     * leaves. Any other peer writes the report without a bisection. Bisect is a synchronous loop (the IHashTreePeer
     * seam), so it runs on a helper thread that is handed control one slice per Update and never runs at the same time as
     * the caller: Update never blocks. The web build has no threads, so there the bisection is skipped and the report is
     * written with divergence none.
     *
     * Single-threaded. The SimulationClock's tick counter is Time::GetTick(), process-wide; the runtime touches it only
     * with ownsTickCounter.
     */
    class LockstepRuntime
    {
    public:
        LockstepRuntime(N2Net::ITransport &transport, LockstepRuntimeOptions options);
        ~LockstepRuntime();
        LockstepRuntime(const LockstepRuntime &) = delete;
        LockstepRuntime &operator=(const LockstepRuntime &) = delete;

        /// One pass. `unscaledDeltaSeconds` is the real time since the last pass (the clock turns it into ticks).
        void Update(std::uint64_t nowMicros, double unscaledDeltaSeconds);

        /// Tells the relay this client is hidden (a browser tab in the background) or visible again, so a stall is not a drop
        /// yet. Only while running.
        void SetPaused(bool paused);

        /// Leaves the session (a disconnect; the relay then drops or stops per its policy)
        void Leave();

        [[nodiscard]] LockstepState State() const noexcept { return state_; }
        [[nodiscard]] bool Finished() const noexcept
        {
            return state_ == LockstepState::Stopped || state_ == LockstepState::Refused || state_ == LockstepState::Failed;
        }
        [[nodiscard]] std::uint32_t Slot() const noexcept { return slot_; }
        [[nodiscard]] std::uint32_t SlotCount() const noexcept { return slotCount_; }
        /// Ticks run so far (the scheduler's CurrentTick)
        [[nodiscard]] std::uint32_t TicksRun() const noexcept;
        [[nodiscard]] std::uint32_t HashInterval() const noexcept { return hashInterval_; }
        [[nodiscard]] std::uint32_t TickRate() const noexcept { return tickRate_; }
        [[nodiscard]] const std::string &Error() const noexcept { return error_; }
        [[nodiscard]] RefuseReason Refusal() const noexcept { return refusal_; }
        [[nodiscard]] const LockstepRuntimeStats &Stats() const noexcept { return stats_; }
        [[nodiscard]] const std::optional<LockstepStopInfo> &StopInfo() const noexcept { return stop_; }
        [[nodiscard]] const N2Net::HashTreeHistory &History() const noexcept { return history_; }
        /// The confirmed inputs so far as an n2replay version 2 log; nullptr before the welcome
        [[nodiscard]] const Replay::ReplayLog *Log() const noexcept { return recorder_ ? &recorder_->Log() : nullptr; }
        [[nodiscard]] const N2Net::LockstepScheduler *Scheduler() const noexcept { return scheduler_.get(); }

    private:
        struct DriverImpl;
        struct Coroutine;

        void Pump(std::uint64_t now);
        void HandleEvent(const N2Net::TransportEvent &event, std::uint64_t now);
        void OnMessage(N2Net::MessageChannel channel, std::span<const std::uint8_t> message, std::uint64_t now);
        void OnWelcome(const WelcomeMessage &welcome);
        void OnBundle(std::span<const std::uint8_t> message);
        void OnForwardFrom(std::span<const std::uint8_t> message, std::uint64_t now);
        void RunTicks(double unscaledDelta);
        bool RunOneTick(const N2Net::TickInputs &inputs);
        void SendBundle(std::uint64_t now);
        void BeginStop(const StopMessage &stop);
        void StepStop(std::uint64_t now);
        void FinishStop();
        void WriteReport(LockstepStopInfo &info, const N2Net::HashTree *local);
        void Fail(std::string text);
        void Notify(LockstepEvent::Type type, std::uint32_t slot, std::uint32_t tick, std::string text);
        void SendReliable(const std::vector<std::uint8_t> &message);

        N2Net::ITransport &transport_;
        LockstepRuntimeOptions options_;
        LockstepState state_ = LockstepState::Connecting;
        N2Net::PeerId server_ = N2Net::kInvalidPeer;
        std::unique_ptr<N2Net::Connection> connection_;
        std::unique_ptr<N2Net::LockstepScheduler> scheduler_;
        std::unique_ptr<Replay::LockstepRecorder> recorder_;
        N2Net::HashTreeHistory history_;
        std::unique_ptr<N2Net::BisectResponder> responder_;
        std::unique_ptr<N2Net::BisectClient> bisectClient_;
        SimulationClock clock_;
        std::uint32_t slot_ = 0;
        std::uint32_t slotCount_ = 0;
        std::uint32_t hashInterval_ = 30;
        std::uint32_t tickRate_ = 60;
        double step_ = 1.0 / 60.0;
        std::uint32_t relayAck_ = 0;
        std::uint64_t startedAt_ = 0;
        bool haveStartedAt_ = false;
        std::uint64_t lastSendAt_ = 0;
        std::uint64_t lastAckAt_ = 0;
        enum class StopPhase : std::uint8_t
        {
            None,
            Bisecting,
            Lingering,
        };
        StopPhase stopPhase_ = StopPhase::None;
        std::uint64_t lingerUntil_ = 0;
        std::unique_ptr<Coroutine> bisection_;
        bool recorderFull_ = false;
        std::uint64_t now_ = 0;
        std::optional<StopMessage> pendingStop_;
        std::optional<LockstepStopInfo> stop_;
        std::string error_;
        RefuseReason refusal_ = RefuseReason::None;
        LockstepRuntimeStats stats_;
    };
}
