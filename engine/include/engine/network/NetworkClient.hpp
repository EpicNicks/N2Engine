#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkClock.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkId.hpp"
#include "engine/network/NetworkInput.hpp"
#include "engine/network/NetworkInterpolation.hpp"
#include "engine/network/NetworkPrediction.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkReplication.hpp"
#include "engine/network/NetworkRpc.hpp"
#include "engine/network/NetworkTimeDilation.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/PrefabTable.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::Network
{
    enum class NetworkClientState : std::uint8_t
    {
        /// No server peer yet: the transport has not reported a link (over the secure layer: its handshake is running)
        Connecting,
        /// Linked; waiting for the server's hello
        AwaitingHello,
        /// The hello was accepted; the join state (the scene objects destroyed so far, the live spawned objects) is
        /// arriving
        Joining,
        /// The world matches the server's and follows its spawns and despawns
        Joined,
        /// The client refused to join (the fingerprint differs); see Error() and Refusal()
        Refused,
        /// The server closed the connection, or Leave was called
        Closed,
        /// A protocol violation by the server, or no hello in time; see Error()
        Failed,
    };

    [[nodiscard]] std::string_view ToString(NetworkClientState state) noexcept;

    struct NetworkClientOptions
    {
        /// The same protocol id the server's Connections use
        std::uint32_t protocolId = 0;
        /// This client's scene (an informational text) and the hash of its parsed content (HashSceneJson)
        std::string scenePath;
        std::uint64_t sceneHash = 0;
        /// The prefabs of this process, frozen (PrefabTable::Capture); the server's spawn names one by index
        PrefabTable prefabs;
        /// Linked, but no hello this long: the client fails
        std::uint64_t helloTimeoutMicros = 10'000'000;
        /// After refusing, the client keeps its connection up this long at most, to get its reject out (it leaves as soon
        /// as the reject was acknowledged)
        std::uint64_t refuseLingerMicros = 1'000'000;
        /// The networked input layout (netcode stage B4), part of the fingerprint: the same as the server's, or the
        /// client refuses to join. Empty: this client sends no input.
        InputLayout inputLayout;
        /// Frames in every input message: the newest this many unacknowledged ones (the network setting
        /// authoritative.inputRedundancy, 1 to 8; a host copies it here). Clamped to 1 to kMaxInputFramesPerMessage.
        std::uint32_t inputRedundancy = 4;
        /// Remote objects are shown interpolated (netcode stage B3). false: they show the newest state received, as
        /// before. The delay is the network setting authoritative.interpolationDelayMs (a host copies it into
        /// interpolation.delayMicros).
        bool interpolate = true;
        InterpolationOptions interpolation;
        /// The clock estimate's filter (see ClockSyncOptions)
        ClockSyncOptions clock;
        /// How often the client pings the server for its clock: this often until the estimate has eight samples, then the
        /// second interval. At most one ping per Update. Each at least 20 ms (clamped), so a ping storm cannot come from here.
        std::uint64_t clockBurstIntervalMicros = 100'000;
        std::uint64_t clockIntervalMicros = 500'000;
        /// The limits on the RPCs the server sends this client (a ceiling that bounds the queue, not a rate anyone needs)
        /// and on the RPCs this client sends (maxSendPerSecond); clamped
        RpcLimits rpc;
        /// True in the Deterministic preset: only RPCs declared cosmetic are sent or run
        bool cosmeticRpcsOnly = false;
        /// Client prediction of the objects this client owns that have a CharacterController (netcode stage B6). Off by
        /// default. Needs an input layout with the controls the mapping names, or the client logs the error and does not
        /// predict (PredictionError).
        PredictionOptions prediction;
        /// The server's input delay in ticks (ServerInputOptions::delayTicks: a frame stamped T is applied on tick
        /// T + delay), which SuggestedInputTick needs: the hello does not carry it, so a host copies the value the server
        /// runs with. Clamped to 0..kMaxInputDelayTicks.
        std::uint32_t serverInputDelayTicks = 2;
        /// SuggestedInputTick allows this many times the measured jitter on top of half the round trip (0 to 8, clamped)
        double jitterAllowance = 1.0;
        /// Time dilation of the fixed-step clock (opt-in, off by default; see NetworkTimeDilation.hpp)
        TimeDilationOptions timeDilation;
    };

    /// What the clock sync did on the client (the estimator's own counters are ClockSync::Stats)
    struct ClientClockStats
    {
        std::uint64_t pingsSent = 0;
        std::uint64_t pongsReceived = 0;
        /// A pong for a ping this client has no record of (answered twice, too late, or never sent)
        std::uint64_t pongsUnmatched = 0;
        /// A pong the estimator refused (a round trip over its limit, a tick rate that changed)
        std::uint64_t pongsRejected = 0;
    };

    struct NetworkClientStats
    {
        std::uint64_t spawnsApplied = 0;
        std::uint64_t despawnsApplied = 0;
        /// A despawn for an object this client does not have (it destroyed it itself, or it never had it)
        std::uint64_t despawnsUnknown = 0;
    };

    /// What NetworkClient tells its observer
    struct NetworkSessionEvent
    {
        enum class Type : std::uint8_t
        {
            /// The join state arrived and matched: the world is in step with the server
            Joined,
            Spawned,
            /// An object the server despawned was found and is being destroyed
            Despawned,
            /// The client refused to join; `text` is the error
            Refused,
            /// The connection ended without a violation
            Closed,
            Failed,
        };

        Type type = Type::Joined;
        NetworkId id;
        /// Spawned: the prefab's name
        std::string prefab;
        /// Spawned: the owner
        ClientId owner = kServerClientId;
        /// Refused, Closed and Failed: the error text
        std::string text;
    };

    /**
     * The client side of the world protocol (netcode stage B1): over one transport link to the server it runs an n2net
     * Connection, checks the server's fingerprint against its own and refuses to join, with an error that names the
     * difference, when they differ; otherwise it accepts, applies the join state and then every spawn and despawn the
     * server sends, into the world it was given (a NetworkWorld bound to the scene the client loaded). Headless: it
     * needs no window, renderer or engine tick, only a scene to put objects into.
     *
     * What it trusts: nothing it does not check. Every message is decoded with bounded lengths; a spawn must carry an id
     * above every scene id and above the last spawn's, name a prefab of the client's own table that has exactly one
     * NetworkObject on its root, and not exceed kMaxNetworkObjects; the join state must be as long as the server said.
     * Anything else fails the client (Failed, Error() says why) and it disconnects. A client never sends a spawn or a
     * despawn: it sends its accept or its reject, and the acknowledgements of the state it applied, nothing else.
     *
     * Field replication (netcode stage B2): once Joined, the client applies the server's StateUpdates (the unreliable
     * channel) to the replicated fields of the objects it has, through Component::ApplyReplicated, and acknowledges what
     * it applied. It writes nothing but replicated fields and takes values from nothing but the server's connection.
     *
     * Player input (netcode stage B4): once Joined, the game queues the local input once per fixed tick (QueueInput); each
     * Update sends the newest few unacknowledged frames in one unreliable message, so a lost or reordered datagram loses
     * no input as long as one of the next few arrives; the server's acknowledgements trim the ring of unacknowledged frames,
     * which never holds more than kMaxUnackedInputFrames.
     *
     * Prediction (netcode stage B6, NetworkPrediction.hpp), when options.prediction.enabled: QueueInput also steps every
     * CharacterController of an object this client owns, with the input it queues (mapped to a CharacterInput, after the
     * layout's quantisation, so it is the input the server will read), and keeps the input and the state it got in a
     * bounded history. The server's state updates carry the stamp of the newest input it applied; when an update for such
     * an object arrives, Update compares the server's state with the history at that stamp, and on a difference sets the
     * controller to the server's state and runs the inputs after the stamp through the same step again, showing the
     * correction as an offset that decays (or as a jump, when it is large). The server stays the authority: the client's
     * state is a guess, and nothing it computes is sent.
     *
     * Clock and interpolation (netcode stage B3): once Joined, the client pings the server a few times a second and
     * estimates the server's tick from the answers (ClockSync: server time, round trip, jitter), and every Update shows
     * the objects it has no authority over at (estimated server time - interpolation delay), interpolated between the
     * states the server sent (SnapshotInterpolator). An object this client owns is NOT interpolated (the client controls
     * it; prediction is a later stage), and neither is one the interpolation filter (SetInterpolationFilter) refuses.
     *
     * Single-threaded; call Update every frame with a monotonic microsecond clock. It must not outlive the transport
     * or the world.
     */
    class NetworkClient
    {
    public:
        /// `world` must be bound to the client's scene already (NetworkWorld::BindScene): the fingerprint it sends and
        /// compares includes the number of scene objects
        NetworkClient(N2Net::ITransport &transport, NetworkWorld &world, NetworkClientOptions options);
        ~NetworkClient();
        NetworkClient(const NetworkClient &) = delete;
        NetworkClient &operator=(const NetworkClient &) = delete;

        /// The client most recently constructed that is still alive, or nullptr: how the Lua binding finds the client
        /// (Network.GetServerTime and the rest). A process runs one.
        [[nodiscard]] static NetworkClient *Active() noexcept;

        /// Called when something happens (see NetworkSessionEvent). Runs inside Update; it may not call Update.
        void SetEventObserver(std::function<void(const NetworkSessionEvent &)> observer)
        {
            _observer = std::move(observer);
        }

        /// One step: polls the transport, handles what arrived, checks the timers, lets the connection send what is due
        void Update(std::uint64_t nowMicros);

        /// Disconnects from the server (the next Update does it) and ends the session as Closed
        void Leave() noexcept { _leaveRequested = true; }

        [[nodiscard]] NetworkClientState State() const noexcept { return _state; }
        [[nodiscard]] bool IsJoined() const noexcept { return _state == NetworkClientState::Joined; }
        /// True once nothing more will happen: the session ended (Closed, Failed) or the refusal has been sent
        [[nodiscard]] bool IsFinished() const noexcept { return _finished; }
        /// What went wrong (Refused, Failed, Closed by the server); empty otherwise
        [[nodiscard]] const std::string &Error() const noexcept { return _error; }
        /// The reason of a refusal; None unless the state is Refused
        [[nodiscard]] JoinRefusal Refusal() const noexcept { return _refusal; }
        /// The id the server gave this client (0 until its hello)
        [[nodiscard]] ClientId LocalClientId() const noexcept { return _localClientId; }
        [[nodiscard]] const Fingerprint &LocalFingerprint() const noexcept { return _localFingerprint; }
        /// The server's, once its hello arrived
        [[nodiscard]] const std::optional<Fingerprint> &ServerFingerprint() const noexcept { return _serverFingerprint; }
        [[nodiscard]] const NetworkClientStats &Stats() const noexcept { return _stats; }
        /// What the client did with the server's state updates (netcode stage B2)
        [[nodiscard]] const ClientReplicationStats &ReplicationStats() const noexcept { return _replicator.Stats(); }
        [[nodiscard]] NetworkWorld &World() noexcept { return _world; }

        /// Calls RPC `name` of `component` (a ClientToServer RPC on an object this client owns) with `args`: one JSON
        /// value per parameter. The server checks everything again; a call it refuses has no effect.
        RpcSendResult CallRpc(Component &component, std::string_view name, const std::vector<nlohmann::json> &args);
        [[nodiscard]] const RpcStats &RpcStatistics() const noexcept { return _rpcStats; }

        /// Packs one tick of local input (`values`: InputLayout::ValueCount() of them, in the layout's order) into an
        /// InputFrame and queues it for the next Update to send. `tick` is the tick the server should apply it on (the
        /// server's own count: matching it to the client's is clock sync, a later stage) and must rise from one call to
        /// the next. Returns the frame's sequence. Fails when the client has not joined, has no layout, the count is
        /// wrong or the tick does not rise.
        [[nodiscard]] std::expected<std::uint32_t, std::string> QueueInput(std::uint32_t tick, std::span<const float> values);
        [[nodiscard]] const InputLayout &GetInputLayout() const noexcept { return _options.inputLayout; }
        /// What the input sender did; Unacked is the frames still held for the server to acknowledge
        [[nodiscard]] const ClientInputStats &InputStats() const noexcept { return _input.Stats(); }
        [[nodiscard]] std::size_t UnackedInputFrames() const noexcept { return _input.Unacked(); }

        // ---- Clock and interpolation (netcode stage B3) ----------------------------------------------------------------

        /// True once a pong was accepted: the server's time is known (as of the last Update's clock)
        [[nodiscard]] bool ClockSynced() const noexcept { return _clock.Synced(); }
        /// The server's tick, fractional, at the clock of the last Update; 0 until ClockSynced. Never decreases. The tick
        /// is the 64-bit count the 32 bits on the wire were unwrapped to.
        [[nodiscard]] double ServerTick() { return _clock.ServerTick(_now); }
        /// The same in microseconds of server time
        [[nodiscard]] std::uint64_t ServerTimeMicros() { return _clock.ServerTimeMicros(_now); }
        /// The server's tick rate as its pongs say (0 until ClockSynced)
        [[nodiscard]] std::uint32_t ServerTickRate() const noexcept { return _clock.TickRate(); }
        /// The round trip to the server (median of the recent pings) and its jitter, in microseconds; 0 until ClockSynced
        [[nodiscard]] std::uint64_t RttMicros() const noexcept { return _clock.RttMicros(); }
        [[nodiscard]] std::uint64_t JitterMicros() const noexcept { return _clock.JitterMicros(); }
        [[nodiscard]] const ClockSync &Clock() const noexcept { return _clock; }
        [[nodiscard]] const ClientClockStats &ClockStats() const noexcept { return _clockStats; }

        /// The tick remote objects were shown at in the last Update (the server tick less the interpolation delay); 0
        /// until the first one
        [[nodiscard]] double RenderTick() const noexcept { return _renderTick; }
        [[nodiscard]] const SnapshotInterpolator &Interpolator() const noexcept { return _interpolator; }

        // ---- Input lead and time dilation (netcode stage B6 follow-up) ----------------------------------------------------

        /// The tick the game should stamp its input with now so that it reaches the server just in time, none until
        /// ClockSynced. The server applies a frame stamped T on tick T + delay (ServerInputOptions::delayTicks), so a frame
        /// is in time when it arrives by tick T + delay; with the one-way trip of RTT / 2 and a jitter allowance:
        ///   floor(ServerTick()) + ceil((RTT / 2 + jitterAllowance * jitter) * tickRate) - serverInputDelayTicks
        /// (at least 0; the 32-bit wire tick, wrapping). The server's delay is SUBTRACTED: it is the slack the server adds
        /// after the stamp, so the stamp can be that much earlier. Uses the clock as of the last Update.
        [[nodiscard]] std::optional<std::uint32_t> SuggestedInputTick();
        /// The same without rounding (ServerTick() + (RTT / 2 + allowance) * tickRate - delay, at least 0), in ticks; none
        /// until ClockSynced
        [[nodiscard]] std::optional<double> SuggestedInputTickExact();
        /// How far ahead of the target the game is stamping, in ticks (positive: too far ahead): the tick of the newest
        /// QueueInput plus half a tick, less SuggestedInputTickExact. None until a frame was queued and the clock synced.
        [[nodiscard]] std::optional<double> InputLeadTicks();

        /// The time dilation in force (clamped). Changing it takes effect at the next Update; turning it on or off, or
        /// changing it while off, restarts the controller. Always allowed; it only acts when enabled.
        [[nodiscard]] const TimeDilationOptions &GetTimeDilation() const noexcept { return _dilation.Options(); }
        void SetTimeDilation(const TimeDilationOptions &options) noexcept { _dilation.SetOptions(options); }
        [[nodiscard]] const TimeDilationStats &TimeDilationStatistics() const noexcept { return _dilation.Stats(); }
        /// The factor for the client's fixed-step clock this frame: 1 unless dilation is enabled and the client is joined.
        /// Application::Tick applies it in the Authoritative simulation mode.
        [[nodiscard]] double FixedStepScale() const noexcept;
        /// The whole ticks the fixed-step clock should hold (negative) or run extra (positive), once; 0 usually
        [[nodiscard]] std::int32_t TakeFixedStepAdjustTicks() noexcept;

        // ---- Prediction (netcode stage B6) -------------------------------------------------------------------------------

        /// True when prediction is on and set up (the mapping resolved against the layout)
        [[nodiscard]] bool PredictionActive() const noexcept { return _predictionMap.has_value(); }
        /// Why prediction is off although options.prediction.enabled; empty otherwise
        [[nodiscard]] const std::string &PredictionError() const noexcept { return _predictionError; }
        [[nodiscard]] const PredictionStats &PredictionStatistics() const noexcept { return _predictionStats; }
        /// Zeroes the counters (not the history or the corrections being shown)
        void ResetPredictionStatistics() noexcept { _predictionStats = PredictionStats{}; }
        /// Whether the object is predicted now
        [[nodiscard]] bool IsPredicted(NetworkId id) const { return _predicted.contains(id.value); }
        /// The ticks of history the object holds (0 if it is not predicted)
        [[nodiscard]] std::size_t PredictionHistorySize(NetworkId id) const;
        /// The correction still to show for the object, at the clock of the last Update: the displayed position is the
        /// state's plus this. Zero when none or the object is not predicted.
        [[nodiscard]] Math::Vector3 PredictionOffset(NetworkId id) const;

        /// The tuning in force (the clamped options.prediction values; defaults while prediction is off)
        [[nodiscard]] PredictionTuning GetPredictionTuning() const noexcept { return PredictionTuning::Of(_predictionOptions); }
        /// Changes how errors are judged and shown while the client runs: false (nothing changed) when prediction is not
        /// active. Values are clamped. Takes effect at the next Update; a correction already being shown keeps the
        /// duration it was measured against only until the next frame (the offset is recomputed from the new duration).
        bool SetPredictionTuning(const PredictionTuning &tuning) noexcept;

        /// The hook for an object the game moves itself (a predicted, locally controlled one: netcode stage B6): the
        /// filter is asked for each object before it is interpolated and returns false to leave the object alone. An
        /// object this client owns is never interpolated, filter or not. Runs inside Update.
        void SetInterpolationFilter(std::function<bool(const NetworkObject &)> filter)
        {
            _interpolationFilter = std::move(filter);
        }

    private:
        void HandleEvent(const N2Net::TransportEvent &event);
        void OnDatagram(std::span<const std::uint8_t> data);
        void OnMessage(N2Net::MessageChannel channel, std::span<const std::uint8_t> message);
        void OnHello(const ServerHelloMessage &hello);
        void OnSpawn(const SpawnMessage &spawn);
        void OnDespawn(const DespawnMessage &despawn);
        void OnWorldReady(const WorldReadyMessage &ready);
        void OnInputAck(std::span<const std::uint8_t> message);
        void OnClockPong(std::span<const std::uint8_t> message);
        void OnRpc(std::span<const std::uint8_t> message);
        void DispatchRpcs();
        void OnStreamTick(std::span<const std::uint8_t> message);
        void SendClockPing();
        void OnStateMessage(std::uint32_t tick);
        void OnEntryApplied(NetworkObject &object, std::uint32_t tick);
        void ApplyInterpolation();
        void UpdateDilation();
        struct PredictedObject;
        void Predict(const std::vector<float> &values, std::uint32_t tick);
        void ScanPredicted();
        void NotePredictedEntry(NetworkObject &object);
        void ReconcilePredicted();
        void ApplyPredictedPositions();
        [[nodiscard]] Math::Vector3 OffsetOf(const PredictedObject &object) const;
        [[nodiscard]] bool IsLocallyOwned(const NetworkObject &object) const noexcept;
        void RefuseJoin(JoinRefusal reason, std::string text);
        void Fail(std::string text);
        void Notify(const NetworkSessionEvent &event);
        [[nodiscard]] bool Live() const noexcept;

        N2Net::ITransport &_transport;
        NetworkWorld &_world;
        NetworkClientOptions _options;
        Fingerprint _localFingerprint;
        std::optional<Fingerprint> _serverFingerprint;
        NetworkClientStats _stats;
        ClientReplicator _replicator;
        ClientInputSender _input;
        ClockSync _clock;
        SnapshotInterpolator _interpolator;
        ClientClockStats _clockStats;
        RpcStats _rpcStats;
        RpcBudget _rpcBudget;
        ThrottledLog _rpcLog;
        std::vector<PendingRpc> _pendingRpcs;
        struct PendingPing
        {
            std::uint32_t sequence = 0;
            std::uint64_t sentAt = 0;
        };
        static constexpr std::size_t kPendingPings = 8;
        std::array<PendingPing, kPendingPings> _pendingPings{};
        std::uint32_t _nextPingSequence = 1;
        std::uint64_t _nextPingAt = 0;
        double _renderTick = 0.0;
        TimeDilationController _dilation;
        bool _hasInputTick = false;
        std::uint32_t _lastInputTick = 0;
        std::uint64_t _lastInputAt = 0;
        struct PredictedObject
        {
            explicit PredictedObject(std::size_t capacity) : history(capacity) {}
            PredictionHistory history;
            /// Whether the server has sent this object's state yet: until it has, the controller is not where the server's
            /// is (the object is not placed), so it is not predicted; the first state sets it (netcode stage B6)
            bool synced = false;
            /// A server state waiting for the next reconciliation: the newest by input tick
            bool hasPending = false;
            std::uint32_t pendingTick = 0;
            Physics::CharacterState pendingState;
            /// The correction being shown: starts at offsetStart at offsetSince and falls linearly to 0
            Math::Vector3 offsetStart{0.0f, 0.0f, 0.0f};
            std::uint64_t offsetSince = 0;
        };
        std::optional<CharacterInputMap> _predictionMap;
        PredictionOptions _predictionOptions;
        std::string _predictionError;
        PredictionStats _predictionStats;
        std::map<std::uint32_t, PredictedObject> _predicted;
        bool _predictRescan = true;
        std::function<bool(const NetworkObject &)> _interpolationFilter;
        NetworkClientState _state = NetworkClientState::Connecting;
        std::string _error;
        JoinRefusal _refusal = JoinRefusal::None;
        ClientId _localClientId = kServerClientId;

        std::uint64_t _now = 0;
        N2Net::PeerId _serverPeer = N2Net::kInvalidPeer;
        std::unique_ptr<N2Net::Connection> _connection;
        std::uint64_t _helloWaitSince = 0;
        std::uint64_t _refusedAt = 0;
        std::uint32_t _lastSpawnId = 0;
        std::uint32_t _joinSpawns = 0;
        std::uint32_t _joinDespawns = 0;
        bool _leaveRequested = false;
        bool _disconnectPending = false;
        bool _finished = false;
        std::function<void(const NetworkSessionEvent &)> _observer;
    };
}
