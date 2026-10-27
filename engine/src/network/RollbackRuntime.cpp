#include "engine/network/RollbackRuntime.hpp"

#include <chrono>
#include <format>
#include <utility>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

namespace N2Engine::Network
{
    static_assert(kMaxRollbackWindow == 15, "RollbackWindowFor clamps to the session's window bound");

    RollbackRuntime::RollbackRuntime(N2Net::ITransport &transport, NetworkWorld &world, Physics::IPhysicsBackend *physics,
                                     const NetworkSettings &settings, RollbackRuntimeOptions options)
        : _world(world), _physics(physics), _rollback(world, physics)
    {
        _window = RollbackWindowFor(settings);
        if (_window > 0 && _physics != nullptr && !_physics->SupportsSnapshot())
        {
            Logger::Warn(std::format("network.deterministic.mode is rollback but the physics backend cannot snapshot its "
                                     "world: the session runs lockstep (rollback window {} ignored)",
                                     _window));
            _window = 0;
        }

        Application &app = Application::GetInstance();
        if (!app.GetSimulationClock().IsNetworked())
        {
            // A tick of the session is a network tick: the attach queue at its boundary, the fixed step unscaled
            app.SetSimulationMode(SimulationMode::Deterministic);
        }

        LockstepRuntimeOptions session = std::move(options.session);
        _hostApply = std::move(session.applyTick);
        _clock = options.clock ? std::move(options.clock) : std::function<std::uint64_t()>(
                                                                []
                                                                {
                                                                    return static_cast<std::uint64_t>(
                                                                        std::chrono::duration_cast<std::chrono::microseconds>(
                                                                            std::chrono::steady_clock::now().time_since_epoch())
                                                                            .count());
                                                                });
        session.rollbackWindow = _window;
        // The runtime sets the counter itself, around Application's own BeginTick
        session.ownsTickCounter = false;
        session.applyTick = [this](const N2Net::TickInputs &inputs, const std::uint32_t slots) { return RunTick(inputs, slots); };
        session.save = [this](const std::uint32_t tick, WorldSnapshot &out) { return _rollback.Save(tick, out); };
        session.restore = [this](const WorldSnapshot &snapshot) { return _rollback.Restore(snapshot); };
        session.confirmed = [this, hook = std::move(session.confirmed)](const std::uint32_t first)
        {
            (void)_rollback.ReleaseConfirmed(first);
            if (hook)
            {
                hook(first);
            }
        };
        session.resimulated = [this, hook = std::move(session.resimulated)]
        {
            (void)_rollback.ReleasePool();
            if (hook)
            {
                hook();
            }
        };
        session.join = [this](const WorldSnapshot &snapshot) -> std::expected<void, std::string>
        {
            auto joined = _rollback.RestoreForJoin(snapshot);
            if (!joined)
            {
                return std::unexpected(joined.error());
            }
            _join = *joined;
            if (!joined->exactPhysics && snapshot.hasPhysics)
            {
                Logger::Warn("Spectating: the physics backend refused the snapshot's physics part (its bodies are not the "
                             "ones of the snapshot); the bodies were set from the objects' body states, so this view may "
                             "drift from the players' simulation");
            }
            return {};
        };
        session.buildTree = [this](const std::uint32_t tick)
        { return BuildStateHashTree(_world, _rollback.Layouts(), tick, _physics); };
        session.describeField = [this](const std::uint32_t objectId, const std::uint16_t fieldId)
        { return DescribeHashedField(_world, _rollback.Layouts(), objectId, fieldId); };

        _runtime = std::make_unique<LockstepRuntime>(transport, std::move(session));

        if (options.driveApplication)
        {
            _drives = true;
            app.SetFixedTickDriver([this](const double delta) { Update(_clock(), delta); });
        }
    }

    RollbackRuntime::~RollbackRuntime()
    {
        if (_drives)
        {
            Application::GetInstance().SetFixedTickDriver(nullptr);
        }
    }

    void RollbackRuntime::Update(const std::uint64_t nowMicros, const double unscaledDeltaSeconds)
    {
        _runtime->Update(nowMicros, unscaledDeltaSeconds);
    }

    bool RollbackRuntime::RunTick(const N2Net::TickInputs &inputs, const std::uint32_t slotCount)
    {
        Scene *const scene = _world.GetScene();
        if (scene == nullptr || SceneManager::GetCurSceneIndex() == -1 || &SceneManager::GetCurSceneRef() != scene)
        {
            Logger::Error("The network session's scene is not the current scene: the session stops");
            return false;
        }
        Application &app = Application::GetInstance();
        // The counter is the ticks run before this one (the host's hook sees `tick`); StepTicks' BeginTick makes it
        // tick + 1 inside the tick's FixedUpdate, as the first run saw it
        app.GetSimulationClock().SetTick(inputs.tick);
        if (_hostApply && !_hostApply(inputs, slotCount))
        {
            return false;
        }
        if (app.StepTicks(1) != 1)
        {
            return false;
        }
        // The tick boundary a snapshot needs: what the tick created is attached (fresh bodies exist), pooled spawns are
        // active, what it destroyed is gone
        scene->ProcessAttachQueue();
        _rollback.CommitSpawns();
        scene->ProcessDestroyed();
        ++_ticksStepped;
        return true;
    }
}
