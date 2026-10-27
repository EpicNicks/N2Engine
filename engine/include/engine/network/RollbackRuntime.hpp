#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "engine/network/LockstepRuntime.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/network/RollbackWorld.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::Physics
{
    class IPhysicsBackend;
}

namespace N2Engine::Network
{
    class NetworkWorld;

    struct RollbackRuntimeOptions
    {
        /// The session's options as for a LockstepRuntime: protocolId, layout, peerName, scene, seed, reportPath,
        /// sampleInput, observer, spectate, eventPolicy, predict and the timeouts are the host's. applyTick, if set, is
        /// the host's hook that runs first in every tick (confirmed, predicted or resimulated) with that tick's inputs: it
        /// hands them to the game (where its FixedUpdate reads them). The runtime sets rollbackWindow (from the settings),
        /// save, restore, confirmed, resimulated, join, buildTree, describeField and ownsTickCounter itself; a confirmed or
        /// resimulated hook given here still runs, after the runtime's own.
        LockstepRuntimeOptions session;
        /// Hand Application's fixed-step loop to the session (Application::SetFixedTickDriver) from construction to
        /// destruction. Off: the host calls Update itself (a headless host or a test) and ticks still run through
        /// Application::StepTicks.
        bool driveApplication = true;
        /// The microsecond clock Update is called with when the Application drives it (steady_clock by default; a test
        /// gives the simulator's virtual clock)
        std::function<std::uint64_t()> clock;
    };

    /**
     * The relay-connected rollback runtime (netcode C5) and what wires it into the engine: a LockstepRuntime (the
     * connection to the LockstepRelay, the bundles, the drop and stop, the desync flow) in the mode the project's
     * network settings select (RollbackWindowFor: network.deterministic.mode rollback runs a RollbackSession with
     * deterministic.rollbackWindow, lockstep runs the lockstep scheduler), a RollbackWorld over the scene's NetworkWorld
     * and its physics backend, and Application's own fixed loop:
     *   - each tick (confirmed, predicted, or resimulated) sets the tick counter to the tick (so Time::GetTick() inside a
     *     resimulated tick is what it was the first time: the tick in the host's hook, tick + 1 in FixedUpdate), runs the
     *     host's applyTick, then Application::StepTicks(1), then the end of the tick a snapshot needs: the attach queue,
     *     RollbackWorld::CommitSpawns, the destroy queue;
     *   - save and restore are RollbackWorld::Save and Restore, the confirmed hook releases despawned objects no snapshot
     *     needs (ReleaseConfirmed), the resimulated hook the unclaimed pool (ReleasePool);
     *   - the state-hash tree is BuildStateHashTree of the NetworkWorld WITH the physics leaf (HashSimulatedState, which
     *     skips parked bodies), so a rollback peer and a lockstep peer of this runtime hash a confirmed tick alike;
     *   - a spectator (session.spectate) starts from a player's snapshot through RollbackWorld::RestoreForJoin;
     *   - with driveApplication, Application::Tick calls Update with the frame's delta in place of its fixed steps.
     *
     * Rollback needs a backend that can snapshot: with a physics backend whose SupportsSnapshot is false the runtime
     * runs lockstep and logs why. A network preset's simulation mode must be on (Application::Init sets it from
     * network.preset; with SimulationMode::None the constructor switches to Deterministic). The NetworkWorld must be bound
     * to the current scene and stay so. Single-threaded, like the scene.
     */
    class RollbackRuntime
    {
    public:
        RollbackRuntime(N2Net::ITransport &transport, NetworkWorld &world, Physics::IPhysicsBackend *physics,
                        const NetworkSettings &settings, RollbackRuntimeOptions options);
        ~RollbackRuntime();
        RollbackRuntime(const RollbackRuntime &) = delete;
        RollbackRuntime &operator=(const RollbackRuntime &) = delete;

        /// One pass of the session (what the Application's frame calls with driveApplication)
        void Update(std::uint64_t nowMicros, double unscaledDeltaSeconds);

        [[nodiscard]] LockstepRuntime &Session() noexcept { return *_runtime; }
        [[nodiscard]] const LockstepRuntime &Session() const noexcept { return *_runtime; }
        [[nodiscard]] RollbackWorld &World() noexcept { return _rollback; }
        /// The window the session runs with: 0 is lockstep
        [[nodiscard]] std::uint32_t RollbackWindow() const noexcept { return _window; }
        /// A spectator's join, once its snapshot was applied
        [[nodiscard]] const std::optional<JoinRestoreResult> &JoinResult() const noexcept { return _join; }
        /// Ticks the Application ran for the session (resimulated ones included)
        [[nodiscard]] std::uint64_t TicksStepped() const noexcept { return _ticksStepped; }

    private:
        bool RunTick(const N2Net::TickInputs &inputs, std::uint32_t slotCount);

        NetworkWorld &_world;
        Physics::IPhysicsBackend *_physics;
        RollbackWorld _rollback;
        std::uint32_t _window = 0;
        bool _drives = false;
        std::function<bool(const N2Net::TickInputs &, std::uint32_t)> _hostApply;
        std::function<std::uint64_t()> _clock;
        std::optional<JoinRestoreResult> _join;
        std::uint64_t _ticksStepped = 0;
        std::unique_ptr<LockstepRuntime> _runtime;
    };
}
