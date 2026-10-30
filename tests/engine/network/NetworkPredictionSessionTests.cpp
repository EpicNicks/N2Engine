#include "../physics/PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "NetworkTestSupport.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkInput.hpp"
#include "engine/network/NetworkPrediction.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/network/InterestManagement.hpp"
#include "engine/network/NetworkTransform.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Client prediction end to end (netcode stage B6): a NetworkServer that steps the owner's CharacterController with the
// owner's input, and NetworkClients that predict the same step, over seeded virtual links with no sleeps, on a real
// physics backend (a ground box in the loaded scene; the pawns are not colliders, so server and client pawns, which share
// the one backend of this process, do not see each other). Everything is deterministic: the same seed gives the same run.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using N2Engine::Physics::CharacterController;
using N2Engine::Physics::CharacterInput;
using N2Engine::Physics::CharacterState;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'4E50;
    constexpr std::uint64_t kSceneHash = 0x50EDC7ull;
    constexpr const char *kScenePath = "res://prediction.scene";
    constexpr const char *kPawn = "PredictionTest_Pawn";
    constexpr float kDt = 1.0f / 60.0f;
    constexpr std::uint64_t kTickMicros = 16'667;
    constexpr std::uint32_t kStartTick = 1000;
    constexpr std::uint32_t kInputDelay = 2;

    InputControlSpec Spec(const std::string &name, const InputControlKind kind, const std::uint8_t bits, const bool isSigned)
    {
        InputControlSpec spec;
        spec.name = name;
        spec.kind = kind;
        spec.bits = bits;
        spec.range = 1.0f;
        spec.isSigned = isSigned;
        return spec;
    }

    InputLayout GameLayout()
    {
        return InputLayout::Create({Spec("move", InputControlKind::Vector2, 8, true), Spec("jump", InputControlKind::Button, 1, false)})
            .value();
    }

    /// The values in the layout's order (by name): jump, move.x, move.y
    std::vector<float> Values(const float jump, const float x, const float y) { return {jump, x, y}; }

    /// What a walking, turning, jumping player does on stamp `s`
    std::vector<float> Script(const std::uint32_t s)
    {
        const std::uint32_t n = s - kStartTick;
        const float angle = static_cast<float>(n) * 0.045f;
        return Values((n % 47) == 20 ? 1.0f : 0.0f, std::cos(angle), std::sin(angle));
    }

    std::vector<float> Idle(std::uint32_t) { return Values(0.0f, 0.0f, 0.0f); }

    struct Peer
    {
        explicit Peer(const std::string &name) : scene(MakeScene(name, {1, 2, 3}))
        {
            bound = world.BindScene(*scene);
        }

        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        std::expected<std::uint32_t, std::string> bound{std::unexpected(std::string("not bound"))};
    };

    struct ClientRig
    {
        ClientRig() : peer("client") {}

        Peer peer;
        N2Net::SimTransport *endpoint = nullptr;
        std::unique_ptr<NetworkClient> client;
        std::optional<NetworkId> pawnId;

        [[nodiscard]] NetworkObject *Pawn() const { return pawnId.has_value() ? peer.world.Find(*pawnId) : nullptr; }
        [[nodiscard]] CharacterController *Controller() const
        {
            NetworkObject *const pawn = Pawn();
            return pawn != nullptr ? pawn->GetGameObject().GetComponent<CharacterController>() : nullptr;
        }
    };

    class PredictionSessionTest : public ::testing::Test
    {
    protected:
        using Backend = N2PhysicsTests::
#ifdef N2ENGINE_PHYSX_ENABLED
            PhysXTag::Backend;
#else
            JoltTag::Backend;
#endif

        static void SetUpTestSuite()
        {
            auto backend = std::make_unique<Backend>();
            ASSERT_TRUE(backend->Initialize());
            Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
        }

        static void TearDownTestSuite()
        {
            SceneManager::AddScene(Scene::Create("PredictionTest_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        PredictionSessionTest() : registered(MakePrefab()), serverPeer("server"), network(1) {}

        static bool MakePrefab()
        {
            const GameObject::Ptr root = GameObject::Create(std::string("Prefab_") + kPawn);
            root->CreatePositionable();
            root->GetPositionable()->SetPosition(Math::Vector3(0.0f, 3.0f, 0.0f));
            root->AddComponent<NetworkObject>();
            root->AddComponent<CharacterController>();
            return PrefabManager::RegisterPrefab(kPawn, root);
        }

        ~PredictionSessionTest() override
        {
            PrefabManager::UnregisterPrefab(kPawn);
        }

        void SetUp() override
        {
            ASSERT_TRUE(registered);
            SceneManager::AddScene(Scene::Create(std::string("PredictionTest_") +
                                                     ::testing::UnitTest::GetInstance()->current_test_info()->name()),
                                   true);
            SceneManager::ProcessAnyPendingSceneChange();
            Scene *const ground = SceneManager::GetCurScene();
            const auto floor = GameObject::Create("Ground");
            floor->CreatePositionable();
            floor->GetPositionable()->SetPosition(Math::Vector3(0.0f, -0.5f, 0.0f));
            floor->AddComponent<Physics::BoxCollider>()->SetSize(Math::Vector3(60.0f, 1.0f, 60.0f));
            ground->AddRootGameObject(floor);
            ground->ProcessAttachQueue();
            Application::GetInstance().Get3DPhysicsBackend()->ApplyPendingChanges();
            Application::GetInstance().Get3DPhysicsBackend()->Update(kDt);

            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.inputLayout = GameLayout();
            options.input.delayTicks = kInputDelay;
            options.stateSendRateHz = 60;
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverPeer.world, std::move(options));
            server->SetCurrentTick(kStartTick);
            serverMap = CharacterInputMap::Create({}, GameLayout()).value();
        }

        ClientRig &AddClient(const N2Net::SimLinkConfig &link, const bool predict = true,
                             const std::optional<InputLayout> &layout = std::nullopt)
        {
            auto rig = std::make_unique<ClientRig>();
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            rig->endpoint = &endpoint;
            network.Link(serverEndpoint->Id(), endpoint.Id(), link);
            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.inputLayout = layout.has_value() ? *layout : GameLayout();
            options.prediction.enabled = predict;
            rig->client = std::make_unique<NetworkClient>(endpoint, rig->peer.world, std::move(options));
            clients.push_back(std::move(rig));
            return *clients.back();
        }

        void StepNetwork(const std::uint64_t micros)
        {
            network.Advance(micros);
            const std::uint64_t now = network.NowMicros();
            server->Update(now);
            for (const auto &rig : clients)
            {
                rig->client->Update(now);
            }
        }

        void RunMicros(const std::uint64_t micros)
        {
            for (std::uint64_t elapsed = 0; elapsed < micros; elapsed += 1000)
            {
                StepNetwork(1000);
            }
        }

        template <typename Done>
        bool RunUntil(const Done &done, const std::uint64_t limitMillis = 20'000)
        {
            for (std::uint64_t i = 0; i < limitMillis && !done(); ++i)
            {
                StepNetwork(1000);
            }
            return done();
        }

        /// Joins a client, spawns its pawn on the server and waits until the client has it: from here the server steps
        /// the pawn and the client predicts it (neither does before, so they start from the same state)
        void JoinWithPawn(ClientRig &rig)
        {
            ASSERT_TRUE(RunUntil([&] { return rig.client->IsJoined(); }));
            const auto id = server->Spawn(kPawn, rig.client->LocalClientId());
            ASSERT_TRUE(id.has_value()) << id.error();
            rig.pawnId = *id;
            serverPawns.push_back(*id);
            ASSERT_TRUE(RunUntil([&] { return rig.Pawn() != nullptr; }));
            serverPeer.scene->ProcessAttachQueue();
            rig.peer.scene->ProcessAttachQueue();
        }

        CharacterController *ServerController(const ClientRig &rig) const
        {
            NetworkObject *const pawn = serverPeer.world.Find(*rig.pawnId);
            return pawn != nullptr ? pawn->GetGameObject().GetComponent<CharacterController>() : nullptr;
        }

        /// One game tick: the server steps every pawn on the tick's input and sends; every predicting client queues the
        /// input of `script` for the tick (which steps its own pawn) and the network runs for the length of a tick
        void Tick(std::vector<float> (*script)(std::uint32_t))
        {
            const std::uint32_t tick = kStartTick + tickIndex;
            server->SetCurrentTick(tick);
            for (const NetworkId id : serverPawns)
            {
                if (NetworkObject *const pawn = serverPeer.world.Find(id))
                {
                    (void)StepServerCharacter(*server, *pawn, *serverMap, tick, kDt);
                }
            }
            for (const auto &rig : clients)
            {
                if (rig->pawnId.has_value())
                {
                    EXPECT_TRUE(rig->client->QueueInput(tick, script(tick)).has_value());
                }
            }
            RunMicros(kTickMicros);
            ++tickIndex;
        }

        void Ticks(const std::uint32_t count, std::vector<float> (*script)(std::uint32_t))
        {
            for (std::uint32_t i = 0; i < count; ++i)
            {
                Tick(script);
            }
        }

        /// The server steps a pawn from the tick it is spawned on, with neutral input until the first frame arrives, while
        /// the client starts predicting from the server's first state and its first input: that start-up gap is corrected
        /// by design. The tests that count corrections let the pawn land and rest first (idle input: the same on both
        /// sides whatever the delay) and then zero the counters
        void Settle()
        {
            Ticks(120, &Idle);
            for (const auto &rig : clients)
            {
                rig->client->ResetPredictionStatistics();
            }
        }

        static float Distance(const Math::Vector3 &a, const Math::Vector3 &b) { return (a - b).Length(); }

        bool registered;
        Peer serverPeer;
        N2Net::SimNetwork network;
        N2Net::SimTransport *serverEndpoint = nullptr;
        std::unique_ptr<NetworkServer> server;
        std::optional<CharacterInputMap> serverMap;
        std::vector<std::unique_ptr<ClientRig>> clients;
        std::vector<NetworkId> serverPawns;
        std::uint32_t tickIndex = 0;
    };

    N2Net::SimLinkConfig Clean()
    {
        N2Net::SimLinkConfig link;
        link.latencyMicros = 20'000;
        return link;
    }

    N2Net::SimLinkConfig Impaired()
    {
        N2Net::SimLinkConfig link;
        link.latencyMicros = 30'000;
        link.jitterMicros = 8'000;
        link.lossPpm = 100'000;
        link.reorderPpm = 100'000;
        link.reorderDelayMicros = 30'000;
        link.duplicatePpm = 50'000;
        return link;
    }
}

TEST_F(PredictionSessionTest, ThePawnMovesAtOnceAndTheServerAgreesWithoutAnyCorrection)
{
    ClientRig &rig = AddClient(Clean());
    JoinWithPawn(rig);
    ASSERT_TRUE(rig.client->PredictionActive()) << rig.client->PredictionError();
    Settle();
    CharacterController *const mine = rig.Controller();
    CharacterController *const theirs = ServerController(rig);
    ASSERT_NE(mine, nullptr);
    ASSERT_NE(theirs, nullptr);

    // Warm up: the pawn falls, lands and walks; the first acknowledgements arrive
    Ticks(60, &Script);
    const PredictionStats warm = rig.client->PredictionStatistics();
    ASSERT_GT(warm.inputsPredicted, 50u);

    const Math::Vector3 before = mine->GetState().position;
    Ticks(240, &Script);
    const PredictionStats stats = rig.client->PredictionStatistics();

    // The client moved on its own input long before the server could have told it
    EXPECT_GT(Distance(mine->GetState().position, before), 1.0f);
    EXPECT_EQ(stats.inputsPredicted - warm.inputsPredicted, 240u);
    // Same step, same input: the server's states match the predictions, so nothing is corrected
    EXPECT_EQ(stats.corrections, warm.corrections) << "no correction after warm-up";
    EXPECT_EQ(stats.snaps, 0u);
    EXPECT_EQ(stats.windowOverflows, 0u);
    EXPECT_GT(stats.matches - warm.matches, 150u);
    EXPECT_LT(stats.maxError, 0.01f);
    // The history is trimmed by the acknowledgements: a few ticks of lead, not the whole run
    EXPECT_LT(rig.client->PredictionHistorySize(*rig.pawnId), 16u);
    EXPECT_GT(rig.client->PredictionHistorySize(*rig.pawnId), 0u);
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);

    // And the server's pawn really followed the client's input: after the input stops they come to rest together
    Ticks(120, &Idle);
    EXPECT_LT(Distance(mine->GetState().position, theirs->GetState().position), 0.01f);
    EXPECT_TRUE(mine->GetState().grounded);
    EXPECT_TRUE(theirs->GetState().grounded);
    EXPECT_GT(Distance(theirs->GetState().position, Math::Vector3(0.0f, 3.0f, 0.0f)), 1.0f);
}

TEST_F(PredictionSessionTest, ThePredictedObjectShowsTheStateAndTheClientNeverSendsIt)
{
    ClientRig &rig = AddClient(Clean());
    JoinWithPawn(rig);
    CharacterController *const mine = rig.Controller();
    ASSERT_NE(mine, nullptr);
    EXPECT_TRUE(mine->IsExternallyDriven()) << "the fixed update must not step it a second time";
    Ticks(30, &Script);
    EXPECT_TRUE(rig.client->IsPredicted(*rig.pawnId));
    const Math::Vector3 shown = rig.Pawn()->GetGameObject().GetPositionable()->GetPosition();
    EXPECT_LT(Distance(shown, mine->GetState().position + rig.client->PredictionOffset(*rig.pawnId)), 1e-4f);
}

TEST_F(PredictionSessionTest, AServerSidePushIsCorrectedByReplayAndSmoothed)
{
    ClientRig &rig = AddClient(Clean());
    JoinWithPawn(rig);
    Settle();
    CharacterController *const mine = rig.Controller();
    CharacterController *const theirs = ServerController(rig);
    Ticks(120, &Idle); // landed and still
    ASSERT_TRUE(mine->GetState().grounded);
    ASSERT_EQ(rig.client->PredictionStatistics().snaps, 0u);
    const std::uint64_t correctionsBefore = rig.client->PredictionStatistics().corrections;

    // The server moves the pawn 0.4 units (a push, a platform, an ability): the client did not predict it
    const CharacterState pushed = theirs->GetState();
    ASSERT_TRUE(theirs->Teleport(pushed.position + Math::Vector3(0.4f, 0.0f, 0.0f)));

    // Run until the client has heard of it
    Math::Vector3 firstOffset(0.0f, 0.0f, 0.0f);
    bool seen = false;
    for (int i = 0; i < 20 && !seen; ++i)
    {
        Tick(&Idle);
        if (rig.client->PredictionStatistics().corrections > correctionsBefore)
        {
            seen = true;
            firstOffset = rig.client->PredictionOffset(*rig.pawnId);
        }
    }
    ASSERT_TRUE(seen) << "the correction never came";
    EXPECT_EQ(rig.client->PredictionStatistics().snaps, 0u) << "0.4 is below the snap distance";
    EXPECT_GE(rig.client->PredictionStatistics().replayedSteps, 1u);
    // The state jumped by 0.4; what is shown has not: it starts where it was and slides
    EXPECT_NEAR(firstOffset.x, -0.4f, 0.1f);
    EXPECT_NEAR(firstOffset.z, 0.0f, 0.05f);
    const Math::Vector3 shown = rig.Pawn()->GetGameObject().GetPositionable()->GetPosition();
    EXPECT_GT(Distance(shown, mine->GetState().position), 0.05f);
    EXPECT_NEAR(Distance(shown, mine->GetState().position + rig.client->PredictionOffset(*rig.pawnId)), 0.0f, 1e-3f);

    // Over the smoothing time the offset shrinks monotonically to zero, and the object ends on the state
    float last = firstOffset.Length();
    for (int i = 0; i < 4; ++i)
    {
        Tick(&Idle);
        const float now = rig.client->PredictionOffset(*rig.pawnId).Length();
        EXPECT_LE(now, last + 1e-5f);
        last = now;
    }
    Ticks(10, &Idle); // more than the 100 ms
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);
    EXPECT_LT(Distance(rig.Pawn()->GetGameObject().GetPositionable()->GetPosition(), theirs->GetState().position), 0.01f);
    EXPECT_LT(Distance(mine->GetState().position, theirs->GetState().position), 0.01f);
}

TEST_F(PredictionSessionTest, ALargeErrorSnapsInsteadOfSliding)
{
    ClientRig &rig = AddClient(Clean());
    JoinWithPawn(rig);
    CharacterController *const theirs = ServerController(rig);
    Ticks(120, &Idle);
    ASSERT_TRUE(theirs->Teleport(theirs->GetState().position + Math::Vector3(8.0f, 0.0f, -3.0f)));
    for (int i = 0; i < 20 && rig.client->PredictionStatistics().snaps == 0; ++i)
    {
        Tick(&Idle);
    }
    EXPECT_EQ(rig.client->PredictionStatistics().snaps, 1u);
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);
    Ticks(10, &Idle);
    EXPECT_LT(Distance(rig.Controller()->GetState().position, theirs->GetState().position), 0.01f);
}

TEST_F(PredictionSessionTest, TheTuningCanBeChangedWhileTheClientRuns)
{
    ClientRig &rig = AddClient(Clean());
    JoinWithPawn(rig);
    CharacterController *const theirs = ServerController(rig);
    Ticks(120, &Idle);

    const PredictionTuning initial = rig.client->GetPredictionTuning();
    EXPECT_FLOAT_EQ(initial.snapDistance, 1.0f);
    EXPECT_EQ(initial.smoothingMicros, 100'000u);

    // A snap distance of 20 turns the 8 unit teleport of ALargeErrorSnapsInsteadOfSliding into a smoothed correction
    PredictionTuning wide = initial;
    wide.snapDistance = 20.0f;
    ASSERT_TRUE(rig.client->SetPredictionTuning(wide));
    EXPECT_FLOAT_EQ(rig.client->GetPredictionTuning().snapDistance, 20.0f);
    const std::uint64_t wideBefore = rig.client->PredictionStatistics().corrections;
    const std::uint64_t snapsBefore = rig.client->PredictionStatistics().snaps;
    ASSERT_TRUE(theirs->Teleport(theirs->GetState().position + Math::Vector3(8.0f, 0.0f, -3.0f)));
    for (int i = 0; i < 20 && rig.client->PredictionStatistics().corrections == wideBefore; ++i)
    {
        Tick(&Idle);
    }
    EXPECT_GT(rig.client->PredictionStatistics().corrections, wideBefore);
    EXPECT_EQ(rig.client->PredictionStatistics().snaps, snapsBefore) << "8 units is under the widened snap distance";
    EXPECT_GT(rig.client->PredictionOffset(*rig.pawnId).Length(), 1.0f);
    Ticks(10, &Idle);

    // Smoothing off: the next correction is shown at once (no offset)
    PredictionTuning none = rig.client->GetPredictionTuning();
    none.smoothingMicros = 0;
    ASSERT_TRUE(rig.client->SetPredictionTuning(none));
    const std::uint64_t before = rig.client->PredictionStatistics().corrections;
    ASSERT_TRUE(theirs->Teleport(theirs->GetState().position + Math::Vector3(0.4f, 0.0f, 0.0f)));
    for (int i = 0; i < 20 && rig.client->PredictionStatistics().corrections == before; ++i)
    {
        Tick(&Idle);
    }
    EXPECT_GT(rig.client->PredictionStatistics().corrections, before);
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);

    // Without prediction there is nothing to tune
    ClientRig &off = AddClient(Clean(), false);
    EXPECT_FALSE(off.client->SetPredictionTuning(wide));
}

TEST_F(PredictionSessionTest, LuaReadsAndChangesTheTuning)
{
    static const bool luaReady = Scripting::LuaRuntime::Instance().Initialize();
    ASSERT_TRUE(luaReady);
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    ClientRig &rig = AddClient(Clean()); // the most recent client is the active one
    JoinWithPawn(rig);

    const auto run = [&lua](const std::string &code)
    { return lua.safe_script(code, sol::script_pass_on_error); };
    const sol::protected_function_result got = run("return Network.GetPredictionTuning()");
    ASSERT_TRUE(got.valid());
    const sol::table table = got;
    EXPECT_FLOAT_EQ(table["positionEpsilon"].get<float>(), 0.01f);
    EXPECT_FLOAT_EQ(table["snapDistance"].get<float>(), 1.0f);
    EXPECT_DOUBLE_EQ(table["smoothingMs"].get<double>(), 100.0);
    EXPECT_TRUE(table["applySmoothing"].get<bool>());

    const sol::protected_function_result set = run("return Network.SetPredictionTuning({ smoothingMs = 250, snapDistance = 2 })");
    ASSERT_TRUE(set.valid());
    EXPECT_TRUE(set.get<bool>());
    EXPECT_EQ(rig.client->GetPredictionTuning().smoothingMicros, 250'000u);
    EXPECT_FLOAT_EQ(rig.client->GetPredictionTuning().snapDistance, 2.0f);
    EXPECT_FLOAT_EQ(rig.client->GetPredictionTuning().positionEpsilon, 0.01f) << "fields not given keep their value";

    // A typo or a bad value is an error and changes nothing
    EXPECT_FALSE(run("Network.SetPredictionTuning({ smoothingMS = 5 })").valid());
    EXPECT_FALSE(run("Network.SetPredictionTuning({ snapDistance = -1 })").valid());
    EXPECT_FALSE(run("Network.SetPredictionTuning({ applySmoothing = 1 })").valid());
    EXPECT_FALSE(run("Network.SetPredictionTuning({ snapDistance = 9, smoothingMs = 'x' })").valid());
    EXPECT_EQ(rig.client->GetPredictionTuning().smoothingMicros, 250'000u);
    EXPECT_FLOAT_EQ(rig.client->GetPredictionTuning().snapDistance, 2.0f);
}

TEST_F(PredictionSessionTest, ALossyReorderingLinkConvergesToTheServersState)
{
    ClientRig &rig = AddClient(Impaired());
    JoinWithPawn(rig);
    CharacterController *const mine = rig.Controller();
    CharacterController *const theirs = ServerController(rig);
    Ticks(400, &Script);
    const PredictionStats stats = rig.client->PredictionStatistics();
    // Lost frames make the server repeat or zero an input: the prediction was wrong there and is corrected
    EXPECT_GT(stats.corrections, 0u);
    EXPECT_GT(stats.replayedSteps, 0u);
    EXPECT_EQ(stats.windowOverflows, 0u);
    EXPECT_LT(rig.client->PredictionHistorySize(*rig.pawnId), 40u);

    // When the input stops the two agree
    Ticks(180, &Idle);
    EXPECT_LT(Distance(mine->GetState().position, theirs->GetState().position), 0.02f);
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);
}

TEST_F(PredictionSessionTest, EachClientMovesOnlyItsOwnPawn)
{
    ClientRig &a = AddClient(Clean());
    ClientRig &b = AddClient(Clean());
    JoinWithPawn(a);
    JoinWithPawn(b);
    Settle();
    const Math::Vector3 startB = ServerController(b)->GetState().position;
    Ticks(100, &Script);
    // Each client predicted only its own pawn, and the server moved both
    EXPECT_GT(a.client->PredictionStatistics().inputsPredicted, 90u);
    EXPECT_GT(b.client->PredictionStatistics().inputsPredicted, 90u);
    EXPECT_FALSE(a.client->IsPredicted(*b.pawnId)) << "A does not predict B's pawn";
    EXPECT_FALSE(b.client->IsPredicted(*a.pawnId));
    EXPECT_EQ(a.client->PredictionHistorySize(*b.pawnId), 0u);
    EXPECT_GT(Distance(ServerController(b)->GetState().position, startB), 0.5f);
    EXPECT_EQ(a.client->PredictionStatistics().snaps, 0u);
    EXPECT_EQ(b.client->PredictionStatistics().snaps, 0u);
}

TEST_F(PredictionSessionTest, PredictionIsOffUnlessAskedForAndAMappingThatDoesNotFitSaysWhy)
{
    ClientRig &off = AddClient(Clean(), false);
    EXPECT_FALSE(off.client->PredictionActive());
    EXPECT_TRUE(off.client->PredictionError().empty());

    // A layout without a "move" control cannot be mapped to a character: the client logs it and does not predict
    const InputLayout other = InputLayout::Create({Spec("fire", InputControlKind::Button, 1, false)}).value();
    ClientRig &bad = AddClient(Clean(), true, other);
    EXPECT_FALSE(bad.client->PredictionActive());
    EXPECT_NE(bad.client->PredictionError().find("move"), std::string::npos);
}

TEST_F(PredictionSessionTest, TheServerStepNeedsAControllerAndGivesAnUnownedObjectNoInput)
{
    ClientRig &rig = AddClient(Clean());
    JoinWithPawn(rig);
    // A scene object without a controller
    NetworkObject *const sceneObject = serverPeer.world.Find(NetworkId{1});
    ASSERT_NE(sceneObject, nullptr);
    EXPECT_EQ(StepServerCharacter(*server, *sceneObject, *serverMap, kStartTick, kDt), ServerStepResult::NoController);

    // An object the server owns steps with no input: it falls and lands where it spawned
    const auto own = server->Spawn(kPawn);
    ASSERT_TRUE(own.has_value());
    NetworkObject *const ownObject = serverPeer.world.Find(*own);
    ASSERT_NE(ownObject, nullptr);
    serverPeer.scene->ProcessAttachQueue();
    for (std::uint32_t i = 0; i < 150; ++i)
    {
        EXPECT_EQ(StepServerCharacter(*server, *ownObject, *serverMap, kStartTick + i, kDt), ServerStepResult::Stepped);
    }
    CharacterController *const controller = ownObject->GetGameObject().GetComponent<CharacterController>();
    EXPECT_TRUE(controller->GetState().grounded);
    EXPECT_NEAR(controller->GetState().position.x, 0.0f, 1e-4f);
    EXPECT_NEAR(controller->GetState().position.z, 0.0f, 1e-4f);
    EXPECT_TRUE(controller->IsExternallyDriven());
}

// ---- Riding a kinematic platform (wire version 8) -----------------------------------------------------------------------
//
// The server and the clients of these tests share one physics world, so the platform exists twice (the server's and the
// client's copy, which lags it). Each peer's platform is on its own layer and each peer's pawn collides with its own
// platform only (the layer mask of its controller), as each would in a process of its own.

namespace
{
    constexpr const char *kPlatform = "PredictionTest_Platform";
    constexpr int kServerPlatformLayer = 8;
    constexpr int kClientPlatformLayer = 9;

    using Path = Math::Vector3 (*)(std::uint32_t);

    Math::Vector3 PlatformStill(std::uint32_t) { return Math::Vector3(0.0f, 0.25f, 0.0f); }
    /// 1.8 units a second along x, from the origin: 9 units in 300 ticks
    Math::Vector3 PlatformLinear(const std::uint32_t n) { return Math::Vector3(0.03f * static_cast<float>(n), 0.25f, 0.0f); }
    /// A circle of radius 4 at 3.6 units a second
    Math::Vector3 PlatformCircle(const std::uint32_t n)
    {
        const float angle = 0.015f * static_cast<float>(n);
        return Math::Vector3(4.0f * std::cos(angle) - 4.0f, 0.25f, 4.0f * std::sin(angle));
    }

    /// A slow walk in a small circle (it stays on a 6 unit platform)
    std::vector<float> Stroll(const std::uint32_t s)
    {
        const std::uint32_t n = s - kStartTick;
        const float angle = static_cast<float>(n) * 0.045f;
        return Values(0.0f, 0.15f * std::cos(angle), 0.15f * std::sin(angle));
    }

    /// 90 ticks east at full speed (off a platform 3 units from its centre), idle, then back west with a jump now and then
    std::vector<float> OffAndOn(const std::uint32_t s)
    {
        const std::uint32_t n = s - kStartTick;
        if (n < 90)
        {
            return Values(0.0f, 1.0f, 0.0f);
        }
        if (n < 150)
        {
            return Idle(s);
        }
        return Values(((n - 150) % 25) == 24 ? 1.0f : 0.0f, -1.0f, 0.0f);
    }

    /// The tick the tests' own phase starts on: SettleOnPlatform runs 150 ticks
    constexpr std::uint32_t kAfterSettle = 150;

    /// 7.2 units a second along x from where it settled (no jump at the start)
    Math::Vector3 PlatformFast(const std::uint32_t n)
    {
        const float t = n > kAfterSettle ? static_cast<float>(n - kAfterSettle) : 0.0f;
        return Math::Vector3(0.12f * t, 0.25f, 0.0f);
    }

    /// A second platform touching the first at x = 3, swaying along z (at most 1.2 units a second)
    Math::Vector3 SecondPlatform(const std::uint32_t n)
    {
        const float t = n > kAfterSettle ? static_cast<float>(n - kAfterSettle) : 0.0f;
        return Math::Vector3(6.0f, 0.25f, std::sin(0.02f * t));
    }

    /// After the settle: 72 ticks east at full speed (5 units a second: from the first platform's centre to the second's),
    /// 60 idle, 72 back west, then idle
    std::vector<float> AcrossAndBack(const std::uint32_t s)
    {
        const std::uint32_t n = s - kStartTick;
        if (n < kAfterSettle)
        {
            return Idle(s);
        }
        const std::uint32_t m = n - kAfterSettle;
        if (m < 72)
        {
            return Values(0.0f, 1.0f, 0.0f);
        }
        if (m >= 132 && m < 204)
        {
            return Values(0.0f, -1.0f, 0.0f);
        }
        return Idle(s);
    }
}

class RidingSessionTest : public PredictionSessionTest
{
protected:
    RidingSessionTest() : platformRegistered(MakePlatformPrefab()) {}

    ~RidingSessionTest() override { PrefabManager::UnregisterPrefab(kPlatform); }

    static bool MakePlatformPrefab()
    {
        const GameObject::Ptr root = GameObject::Create(std::string("Prefab_") + kPlatform);
        root->CreatePositionable();
        root->GetPositionable()->SetPosition(Math::Vector3(0.0f, 0.25f, 0.0f));
        root->AddComponent<NetworkObject>();
        root->AddComponent<NetworkTransform>();
        root->AddComponent<Physics::Rigidbody>()->SetBodyType(Physics::BodyType::Kinematic);
        root->AddComponent<Physics::BoxCollider>()->SetSize(Math::Vector3(6.0f, 0.5f, 6.0f));
        return PrefabManager::RegisterPrefab(kPlatform, root);
    }

    GameObject &ServerPlatform() { return serverPeer.world.Find(platformId)->GetGameObject(); }

    /// Joins, spawns the platform and the pawn, and puts each peer's pawn and platform on its own layer
    void SetUpRide(ClientRig &rig)
    {
        ASSERT_TRUE(platformRegistered);
        ASSERT_TRUE(RunUntil([&] { return rig.client->IsJoined(); }));
        const auto platform = server->Spawn(kPlatform);
        ASSERT_TRUE(platform.has_value()) << platform.error();
        platformId = *platform;
        ASSERT_TRUE(RunUntil([&] { return rig.peer.world.Find(platformId) != nullptr; }));
        serverPeer.scene->ProcessAttachQueue();
        rig.peer.scene->ProcessAttachQueue();
        ServerPlatform().SetLayer(kServerPlatformLayer);
        rig.peer.world.Find(platformId)->GetGameObject().SetLayer(kClientPlatformLayer);
        JoinWithPawn(rig);
        ServerController(rig)->layerMask = static_cast<int>(Layers::DefaultRaycastMask & ~(1u << kClientPlatformLayer));
        rig.Controller()->layerMask = static_cast<int>(Layers::DefaultRaycastMask & ~(1u << kServerPlatformLayer));
        Application::GetInstance().Get3DPhysicsBackend()->ApplyPendingChanges();
    }

    /// One game tick with the server's platform at `path(tick)`; the physics scene steps once so the bodies move
    void RideTick(std::vector<float> (*script)(std::uint32_t), const Path path)
    {
        if (serverPeer.world.Find(platformId) != nullptr)
        {
            ServerPlatform().GetPositionable()->SetPosition(path(tickIndex));
        }
        Application::GetInstance().Get3DPhysicsBackend()->Update(kDt);
        Tick(script);
    }

    void RideTicks(const std::uint32_t count, std::vector<float> (*script)(std::uint32_t), const Path path)
    {
        for (std::uint32_t i = 0; i < count; ++i)
        {
            RideTick(script, path);
        }
    }

    /// RideTick as a game that renders between ticks has it: the physics scene also steps after every millisecond of the
    /// network, so the client's copy of the platform (interpolated at every Update) moves between the tick's prediction
    /// and the state that corrects it
    void FrameRideTick(std::vector<float> (*script)(std::uint32_t), const Path path)
    {
        auto *const physics = Application::GetInstance().Get3DPhysicsBackend();
        if (serverPeer.world.Find(platformId) != nullptr)
        {
            ServerPlatform().GetPositionable()->SetPosition(path(tickIndex));
        }
        physics->Update(kDt);
        const std::uint32_t tick = kStartTick + tickIndex;
        server->SetCurrentTick(tick);
        for (const NetworkId id : serverPawns)
        {
            if (NetworkObject *const pawn = serverPeer.world.Find(id))
            {
                (void)StepServerCharacter(*server, *pawn, *serverMap, tick, kDt);
            }
        }
        for (const auto &rig : clients)
        {
            if (rig->pawnId.has_value())
            {
                EXPECT_TRUE(rig->client->QueueInput(tick, script(tick)).has_value());
            }
        }
        for (std::uint64_t elapsed = 0; elapsed < kTickMicros; elapsed += 1000)
        {
            StepNetwork(1000);
            physics->Update(kDt);
        }
        ++tickIndex;
    }

    /// Spawns a second platform at `at` (before the pawn, so the pawn never stands on it at spawn) on the same layers
    NetworkId SpawnSecondPlatform(ClientRig &rig, const Math::Vector3 &at)
    {
        EXPECT_TRUE(RunUntil([&] { return rig.client->IsJoined(); }));
        const auto platform = server->Spawn(kPlatform);
        EXPECT_TRUE(platform.has_value());
        if (!platform.has_value())
        {
            return NetworkId{};
        }
        const NetworkId id = *platform;
        serverPeer.world.Find(id)->GetGameObject().GetPositionable()->SetPosition(at);
        EXPECT_TRUE(RunUntil([&] { return rig.peer.world.Find(id) != nullptr; }));
        serverPeer.scene->ProcessAttachQueue();
        rig.peer.scene->ProcessAttachQueue();
        serverPeer.world.Find(id)->GetGameObject().SetLayer(kServerPlatformLayer);
        rig.peer.world.Find(id)->GetGameObject().SetLayer(kClientPlatformLayer);
        return id;
    }

    /// Lets the pawn land on the still platform and the first states arrive, then zeroes the counters
    void SettleOnPlatform(ClientRig &rig)
    {
        RideTicks(150, &Idle, &PlatformStill);
        rig.client->ResetPredictionStatistics();
    }

    bool platformRegistered;
    NetworkId platformId;
};

TEST_F(RidingSessionTest, ThePawnStandsOnTheServersPlatformAndKnowsIt)
{
    ClientRig &rig = AddClient(Clean());
    SetUpRide(rig);
    SettleOnPlatform(rig);
    const CharacterState server = ServerController(rig)->GetState();
    const CharacterState mine = rig.Controller()->GetState();
    EXPECT_TRUE(server.grounded);
    EXPECT_EQ(server.ground, platformId.value);
    EXPECT_EQ(mine.ground, platformId.value) << "the client predicts the ride on its own copy of the platform";
    EXPECT_NEAR(server.position.y, 0.5f + 1.0f + 0.02f, 0.05f) << "on the platform's top, not the floor";
    EXPECT_NEAR(server.groundLocal.x, mine.groundLocal.x, 0.01f);
    EXPECT_NEAR(server.groundLocal.y, mine.groundLocal.y, 0.01f);
    EXPECT_NEAR(server.groundLocal.z, mine.groundLocal.z, 0.01f);
}

TEST_F(RidingSessionTest, APlatformMovingInALineCostsNoCorrection)
{
    ClientRig &rig = AddClient(Clean());
    SetUpRide(rig);
    SettleOnPlatform(rig);
    const Math::Vector3 startWorld = ServerController(rig)->GetState().position;
    RideTicks(300, &Idle, &PlatformLinear);
    const PredictionStats stats = rig.client->PredictionStatistics();
    // The platform was still at x = 0 when the linear phase began, and its path starts at x = 4.5 (the tick the phase
    // starts on): it jumps there on the first tick, and the pawn is carried with it. The pawn then stands on the platform
    // where the platform's last pose puts it (the pawn spawned at x = 0, so its local offset is 0).
    EXPECT_NEAR(ServerController(rig)->GetState().position.x, PlatformLinear(tickIndex - 1).x, 0.1f);
    EXPECT_GT(ServerController(rig)->GetState().position.x - startWorld.x, 9.0f);
    EXPECT_EQ(ServerController(rig)->GetState().ground, platformId.value);
    EXPECT_EQ(rig.Controller()->GetState().ground, platformId.value);
    EXPECT_GT(stats.matches, 200u);
    EXPECT_EQ(stats.corrections, 0u) << "the platform's motion is not an error in its own frame";
    EXPECT_EQ(stats.snaps, 0u);
    EXPECT_EQ(stats.windowOverflows, 0u);
    EXPECT_LT(stats.maxError, 0.01f);
}

TEST_F(RidingSessionTest, APlatformOnACircleWithAWalkingPawnCostsNoCorrection)
{
    ClientRig &rig = AddClient(Clean());
    SetUpRide(rig);
    SettleOnPlatform(rig);
    RideTicks(450, &Stroll, &PlatformCircle);
    const PredictionStats stats = rig.client->PredictionStatistics();
    EXPECT_GT(stats.inputsPredicted, 400u);
    EXPECT_EQ(rig.Controller()->GetState().ground, platformId.value);
    EXPECT_EQ(stats.corrections, 0u);
    EXPECT_EQ(stats.snaps, 0u);
    EXPECT_LT(stats.maxError, 0.01f);
    // The offset stayed on the platform while the platform went round
    EXPECT_LT(std::fabs(ServerController(rig)->GetState().groundLocal.x), 1.0f);
    EXPECT_LT(std::fabs(ServerController(rig)->GetState().groundLocal.z), 1.0f);
}

TEST_F(RidingSessionTest, ARiderOnALossyLinkConvergesToTheServersOffset)
{
    ClientRig &rig = AddClient(Impaired());
    SetUpRide(rig);
    SettleOnPlatform(rig); // the start-up gap is corrected by design: the counters start after it
    RideTicks(400, &Stroll, &PlatformCircle);
    const PredictionStats stats = rig.client->PredictionStatistics();
    EXPECT_EQ(stats.windowOverflows, 0u);
    EXPECT_EQ(stats.snaps, 0u);
    // The platform keeps moving while the input stops: the two end on the same ground at the same offset
    std::uint32_t lastCorrectionAt = 0;
    std::uint64_t seen = rig.client->PredictionStatistics().corrections;
    for (std::uint32_t i = 0; i < 180; ++i)
    {
        RideTick(&Idle, &PlatformCircle);
        const std::uint64_t now = rig.client->PredictionStatistics().corrections;
        if (now != seen)
        {
            lastCorrectionAt = i;
            seen = now;
        }
    }
    const CharacterState server = ServerController(rig)->GetState();
    const CharacterState mine = rig.Controller()->GetState();
    EXPECT_EQ(server.ground, platformId.value);
    EXPECT_EQ(mine.ground, platformId.value);
    EXPECT_LT(Distance(server.groundLocal, mine.groundLocal), 0.02f);
    // A lost frame near the end can still be a correction in flight (a lossy link corrects whenever the server had to
    // repeat or zero an input): what matters is that nothing is left to show a moment later
    const Math::Vector3 offset = rig.client->PredictionOffset(*rig.pawnId);
    EXPECT_LT(offset.Length(), 1e-3f) << "last correction at idle tick " << lastCorrectionAt << " of 180, server grounded "
                                      << server.grounded << " vy " << server.velocity.y << ", mine grounded "
                                      << mine.grounded << " vy " << mine.velocity.y << ", local error "
                                      << Distance(server.groundLocal, mine.groundLocal) << ", max error "
                                      << rig.client->PredictionStatistics().maxError;
    // ... and a correction that came late is shown out within the smoothing time (100 ms, 6 ticks) unless the lossy link
    // causes another: it settles within a second
    bool settled = rig.client->PredictionOffset(*rig.pawnId).Length() == 0.0f;
    for (std::uint32_t i = 0; i < 60 && !settled; ++i)
    {
        RideTick(&Idle, &PlatformCircle);
        settled = rig.client->PredictionOffset(*rig.pawnId).Length() == 0.0f;
    }
    EXPECT_TRUE(settled);
}

TEST_F(RidingSessionTest, SteppingOffTheEdgeNeverSnapsAndEndsAgreed)
{
    ClientRig &rig = AddClient(Clean());
    SetUpRide(rig);
    SettleOnPlatform(rig);
    bool rode = false;
    bool leftIt = false;
    for (std::uint32_t i = 0; i < 330; ++i)
    {
        RideTick(&OffAndOn, &PlatformLinear);
        const bool onIt = rig.Controller()->GetState().ground == platformId.value;
        if (onIt && !leftIt)
        {
            rode = true;
        }
        if (!onIt && rode && rig.Controller()->GetState().grounded)
        {
            leftIt = true; // standing on the floor, off the edge
        }
    }
    EXPECT_TRUE(rode);
    EXPECT_TRUE(leftIt);
    const PredictionStats stats = rig.client->PredictionStatistics();
    EXPECT_EQ(stats.snaps, 0u) << "a change of ground is corrected like any error, below the snap distance";
    EXPECT_EQ(stats.windowOverflows, 0u);

    RideTicks(120, &Idle, &PlatformLinear);
    const CharacterState server = ServerController(rig)->GetState();
    const CharacterState mine = rig.Controller()->GetState();
    EXPECT_EQ(server.ground, mine.ground);
    EXPECT_EQ(server.grounded, mine.grounded);
    if (server.ground != 0)
    {
        EXPECT_LT(Distance(server.groundLocal, mine.groundLocal), 0.02f);
    }
    else
    {
        EXPECT_LT(Distance(server.position, mine.position), 0.05f);
    }
}

TEST_F(RidingSessionTest, ADespawnedPlatformDropsTheRiderToTheWorldFrameWithoutASnap)
{
    ClientRig &rig = AddClient(Clean());
    SetUpRide(rig);
    SettleOnPlatform(rig);
    RideTicks(60, &Idle, &PlatformLinear);
    ASSERT_EQ(rig.Controller()->GetState().ground, platformId.value);
    ASSERT_TRUE(server->Despawn(platformId));
    // Destroyed GameObjects leave their scenes (and the physics scene) only when the scene is processed: the server's
    // platform, and the client's copy that the Despawn message destroys, are flushed every tick of the wait
    for (std::uint32_t i = 0; i < 180; ++i)
    {
        RideTick(&Idle, &PlatformLinear);
        serverPeer.scene->ProcessDestroyed();
        rig.peer.scene->ProcessDestroyed();
        Application::GetInstance().Get3DPhysicsBackend()->ApplyPendingChanges();
    }
    const PredictionStats stats = rig.client->PredictionStatistics();
    EXPECT_EQ(rig.peer.world.Find(platformId), nullptr);
    EXPECT_EQ(stats.snaps, 0u);
    EXPECT_EQ(stats.windowOverflows, 0u);
    const CharacterState server = ServerController(rig)->GetState();
    const CharacterState mine = rig.Controller()->GetState();
    EXPECT_EQ(server.ground, 0u);
    EXPECT_EQ(mine.ground, 0u);
    EXPECT_TRUE(server.grounded);
    EXPECT_TRUE(mine.grounded);
    EXPECT_NEAR(server.position.y, 1.02f, 0.05f) << "it landed on the floor";
    EXPECT_LT(Distance(server.position, mine.position), 0.02f);
}

TEST_F(RidingSessionTest, APlatformTheClientDoesNotHaveIsTheWorldFrameAndComesBackWhenItIsSeen)
{
    ClientRig &rig = AddClient(Clean());
    SetUpRide(rig);
    SettleOnPlatform(rig);
    RideTicks(60, &Idle, &PlatformLinear);
    ASSERT_EQ(rig.Controller()->GetState().ground, platformId.value);

    // Interest management takes the platform away from this client (it is the server's: only its owner sees it)
    ASSERT_TRUE(serverPeer.world.Find(platformId)->SetInterestPolicy({Relevance::OwnerOnly, 100.0f, 1.0f}));
    for (std::uint32_t i = 0; i < 120; ++i)
    {
        RideTick(&Idle, &PlatformLinear);
    }
    // Hidden is inactive on this client (visibility deactivates the object, it does not remove it)
    ASSERT_NE(rig.peer.world.Find(platformId), nullptr);
    EXPECT_FALSE(rig.peer.world.Find(platformId)->GetGameObject().IsActive()) << "hidden from this client";
    // The server still says the pawn rides; the client cannot, so it follows the server's world position
    EXPECT_EQ(ServerController(rig)->GetState().ground, platformId.value);
    EXPECT_EQ(rig.Controller()->GetState().ground, 0u);
    const PredictionStats hidden = rig.client->PredictionStatistics();
    EXPECT_EQ(hidden.windowOverflows, 0u);
    // (A platform that moves under a pawn that has nothing under it on this client is the old behaviour: corrected)
    EXPECT_GT(hidden.corrections, 0u);

    // Seen again: the client rides it again and the corrections stop
    ASSERT_TRUE(serverPeer.world.Find(platformId)->SetInterestPolicy({Relevance::Always, 100.0f, 1.0f}));
    for (std::uint32_t i = 0; i < 150; ++i)
    {
        RideTick(&Idle, &PlatformLinear);
    }
    ASSERT_NE(rig.peer.world.Find(platformId), nullptr);
    rig.peer.scene->ProcessAttachQueue();
    rig.peer.world.Find(platformId)->GetGameObject().SetLayer(kClientPlatformLayer);
    RideTicks(60, &Idle, &PlatformLinear);
    rig.client->ResetPredictionStatistics();
    RideTicks(120, &Idle, &PlatformLinear);
    EXPECT_EQ(rig.Controller()->GetState().ground, platformId.value);
    EXPECT_EQ(rig.client->PredictionStatistics().corrections, 0u);
    EXPECT_LT(Distance(rig.Controller()->GetState().groundLocal, ServerController(rig)->GetState().groundLocal), 0.02f);
}

TEST_F(RidingSessionTest, ACorrectionOnAMovingPlatformLeavesThePlatformsOwnMotionOutOfTheOffset)
{
    // 30 ms each way: the state that corrects a tick's prediction arrives about 14 ms into a later tick, by when the
    // client's copy of the platform (7.2 units a second) has moved about 0.1 since the prediction placed the pawn
    N2Net::SimLinkConfig link = Clean();
    link.latencyMicros = 30'000;
    ClientRig &rig = AddClient(link);
    SetUpRide(rig);
    SettleOnPlatform(rig);
    ASSERT_EQ(tickIndex, kAfterSettle);
    for (std::uint32_t i = 0; i < 60; ++i)
    {
        FrameRideTick(&Idle, &PlatformFast);
    }
    ASSERT_EQ(rig.Controller()->GetState().ground, platformId.value);
    ASSERT_EQ(rig.client->PredictionStatistics().corrections, 0u) << "riding alone is no error, frames or not";

    // The server pushes the pawn 0.4 across the platform (z; the platform moves along x), and it keeps riding
    CharacterController *const theirs = ServerController(rig);
    CharacterState pushed = theirs->GetState();
    ASSERT_EQ(pushed.ground, platformId.value);
    pushed.position.z += 0.4f;
    pushed.groundLocal.z += 0.4f;
    ASSERT_TRUE(theirs->SetState(pushed));

    Math::Vector3 firstOffset(0.0f, 0.0f, 0.0f);
    bool seen = false;
    for (int i = 0; i < 20 && !seen; ++i)
    {
        FrameRideTick(&Idle, &PlatformFast);
        if (rig.client->PredictionStatistics().corrections > 0)
        {
            seen = true;
            firstOffset = rig.client->PredictionOffset(*rig.pawnId);
        }
    }
    ASSERT_TRUE(seen) << "the correction never came";
    EXPECT_EQ(rig.client->PredictionStatistics().snaps, 0u);
    // The correction is the push and nothing else: what the pawn showed is taken on the platform as it is now, so the
    // distance the platform carried it since the last step is not slid back out
    EXPECT_NEAR(firstOffset.z, -0.4f, 0.1f);
    EXPECT_NEAR(firstOffset.x, 0.0f, 0.02f) << "the platform's own motion is not part of the correction";

    // And it ends agreed, on the platform
    for (std::uint32_t i = 0; i < 30; ++i)
    {
        FrameRideTick(&Idle, &PlatformFast);
    }
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);
    EXPECT_EQ(rig.Controller()->GetState().ground, platformId.value);
    EXPECT_LT(Distance(rig.Controller()->GetState().groundLocal, theirs->GetState().groundLocal), 0.02f);
}

TEST_F(RidingSessionTest, WalkingOntoASecondPlatformAndBackRidesEachWithoutASnap)
{
    ClientRig &rig = AddClient(Clean());
    const NetworkId second = SpawnSecondPlatform(rig, SecondPlatform(0));
    ASSERT_NE(second.value, 0u);
    SetUpRide(rig);
    SettleOnPlatform(rig);
    ASSERT_EQ(tickIndex, kAfterSettle);
    ASSERT_EQ(rig.Controller()->GetState().ground, platformId.value) << "it landed on the first platform";

    const auto rideTick = [&]
    {
        serverPeer.world.Find(second)->GetGameObject().GetPositionable()->SetPosition(SecondPlatform(tickIndex));
        RideTick(&AcrossAndBack, &PlatformStill);
    };
    bool onSecond = false;
    bool backOnFirst = false;
    float largestOffset = 0.0f;
    for (std::uint32_t i = 0; i < 260; ++i)
    {
        rideTick();
        const CharacterState mine = rig.Controller()->GetState();
        if (mine.grounded && mine.ground == second.value)
        {
            onSecond = true;
        }
        if (onSecond && mine.grounded && mine.ground == platformId.value)
        {
            backOnFirst = true;
        }
        largestOffset = std::max(largestOffset, rig.client->PredictionOffset(*rig.pawnId).Length());
    }
    EXPECT_TRUE(onSecond) << "the client rode the second platform";
    EXPECT_TRUE(backOnFirst) << "and the first again";
    const PredictionStats stats = rig.client->PredictionStatistics();
    EXPECT_EQ(stats.snaps, 0u);
    EXPECT_EQ(stats.windowOverflows, 0u);
    EXPECT_LT(largestOffset, 0.5f) << "a change of ground is no jump";

    // Agreed: the same ground, the same offset on it, and riding it costs nothing more
    const CharacterState server = ServerController(rig)->GetState();
    const CharacterState mine = rig.Controller()->GetState();
    EXPECT_EQ(server.ground, platformId.value);
    EXPECT_EQ(mine.ground, platformId.value);
    EXPECT_LT(Distance(server.groundLocal, mine.groundLocal), 0.02f);
    EXPECT_EQ(rig.client->PredictionOffset(*rig.pawnId).Length(), 0.0f);
    rig.client->ResetPredictionStatistics();
    for (std::uint32_t i = 0; i < 60; ++i)
    {
        rideTick();
    }
    EXPECT_EQ(rig.client->PredictionStatistics().corrections, 0u);
    EXPECT_EQ(rig.Controller()->GetState().ground, platformId.value);
}

#endif // a physics backend is in this build
