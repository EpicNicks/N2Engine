#include "../physics/PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <gtest/gtest.h>

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
#include "engine/physics/CharacterController.hpp"
#include "engine/prefabs/PrefabManager.hpp"

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

#endif // a physics backend is in this build
