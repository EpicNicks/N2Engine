#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/Application.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/SimulationClock.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/network/NetworkTimeDilation.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// Client time dilation (netcode stage B6 follow-up): the pure controller against a simulated plant, the project settings,
// SuggestedInputTick, the whole loop on the network simulator (a client stamping input on a fixed-step clock that
// the client's FixedStepScale speeds up or slows down), the Application hook, and the Lua API.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'4454;
    constexpr std::uint64_t kSceneHash = 0xD11A7E5ull;
    constexpr const char *kScenePath = "res://dilation.scene";
    constexpr std::uint64_t kClientClockOffset = 7'000'000'000ull;
    constexpr std::uint32_t kStartTick = 1000;
    constexpr double kRate = 60.0;
    constexpr double kDt = 1.0 / 60.0;

    // ---- The controller against a plant ---------------------------------------------------------------------------------

    /// A client whose tick advances at scale * 60 per second against a target that advances at 60 per second
    struct Plant
    {
        explicit Plant(const TimeDilationOptions &options, const double startError) : controller(options)
        {
            client = startError;
            target = 0.0;
        }

        /// One 10 ms step; `noise` is added to what the controller measures (not to the truth)
        void Step(const double noise = 0.0)
        {
            const double seconds = 0.010;
            now += 10'000;
            const double scale = controller.Scale();
            client += scale * kRate * seconds;
            target += kRate * seconds;
            controller.Update(now, client - target + noise);
            // A resync moves the clock by whole ticks, once
            client += static_cast<double>(controller.TakeResyncTicks());
            scales.push_back(controller.Scale());
        }

        [[nodiscard]] double Error() const { return client - target; }

        TimeDilationController controller;
        double client = 0.0;
        double target = 0.0;
        std::uint64_t now = 1'000'000;
        std::vector<double> scales;
    };

    TimeDilationOptions On()
    {
        TimeDilationOptions options;
        options.enabled = true;
        return options;
    }

    /// Deterministic noise in [-1, 1]
    double Noise(const std::uint32_t i)
    {
        std::uint32_t x = i * 2654435761u + 12345u;
        x ^= x >> 15;
        x *= 2246822519u;
        x ^= x >> 13;
        return (static_cast<double>(x % 2001u) - 1000.0) / 1000.0;
    }

    int ZeroCrossings(const std::vector<double> &scales, const std::size_t from, const double band)
    {
        int crossings = 0;
        int last = 0;
        for (std::size_t i = from; i < scales.size(); ++i)
        {
            const int side = scales[i] > 1.0 + band ? 1 : (scales[i] < 1.0 - band ? -1 : 0);
            if (side != 0)
            {
                if (last != 0 && side != last)
                {
                    ++crossings;
                }
                last = side;
            }
        }
        return crossings;
    }
}

TEST(TimeDilationControllerTest, ADisabledControllerChangesNothing)
{
    TimeDilationOptions options;
    ASSERT_FALSE(options.enabled) << "off by default";
    Plant plant(options, 8.0);
    for (int i = 0; i < 2000; ++i)
    {
        plant.Step();
    }
    EXPECT_EQ(plant.controller.Scale(), 1.0);
    EXPECT_NEAR(plant.Error(), 8.0, 1e-6) << "the clock ran at exactly the target's rate";
    EXPECT_EQ(plant.controller.Stats().updates, 0u);
    EXPECT_EQ(plant.controller.Stats().resyncs, 0u);
    EXPECT_EQ(plant.controller.TakeResyncTicks(), 0);
}

TEST(TimeDilationControllerTest, TheLeadConvergesToTheTargetFromAheadAndFromBehind)
{
    for (const double start : {5.0, -5.0, 2.0})
    {
        Plant plant(On(), start);
        for (int i = 0; i < 1500; ++i)
        {
            plant.Step();
            ASSERT_GE(plant.scales.back(), 0.9 - 1e-9);
            ASSERT_LE(plant.scales.back(), 1.1 + 1e-9);
        }
        EXPECT_LE(std::abs(plant.Error()), 1.0) << "start " << start;
        EXPECT_EQ(plant.controller.Stats().resyncs, 0u) << "a gentle error is never resynced";
        // Ahead means slower, behind means faster, on the way
        const double extreme = start > 0 ? *std::min_element(plant.scales.begin(), plant.scales.end())
                                         : *std::max_element(plant.scales.begin(), plant.scales.end());
        if (start > 0)
        {
            EXPECT_LT(extreme, 1.0);
        }
        else
        {
            EXPECT_GT(extreme, 1.0);
        }
    }
}

TEST(TimeDilationControllerTest, TheScaleIsClampedToTheBoundsAndMovesOnlyBySlew)
{
    TimeDilationOptions options = On();
    options.minScale = 0.95;
    options.maxScale = 1.05;
    Plant plant(options, 8.0); // asks for 1 - 0.02 * 7.5 = 0.85, but 0.95 is the floor
    for (int i = 0; i < 400; ++i)
    {
        plant.Step();
    }
    EXPECT_NEAR(*std::min_element(plant.scales.begin(), plant.scales.end()), 0.95, 1e-9);
    EXPECT_GT(plant.controller.Stats().clampedUpdates, 0u);
    // The scale never steps: 0.25 per second at 10 ms is at most 0.0025 per update
    for (std::size_t i = 1; i < plant.scales.size(); ++i)
    {
        EXPECT_LE(std::abs(plant.scales[i] - plant.scales[i - 1]), 0.0025 + 1e-9);
    }

    // Out-of-range options are clamped; a non-finite one becomes the default
    TimeDilationOptions wild;
    wild.minScale = 0.1;
    wild.maxScale = 9.0;
    wild.gainPerTick = std::numeric_limits<double>::quiet_NaN();
    wild.resyncTicks = 0.0;
    wild.deadZoneTicks = 3.0;
    const TimeDilationOptions clamped = wild.Clamped();
    EXPECT_EQ(clamped.minScale, 0.5);
    EXPECT_EQ(clamped.maxScale, 2.0);
    EXPECT_EQ(clamped.gainPerTick, TimeDilationOptions{}.gainPerTick);
    EXPECT_GE(clamped.resyncTicks, clamped.deadZoneTicks + 1.0) << "a resync must leave room for the gentle correction";
}

TEST(TimeDilationControllerTest, ADeadZoneAndHysteresisKeepASmallErrorFromHuntingTheClock)
{
    Plant inside(On(), 0.4);
    for (int i = 0; i < 1000; ++i)
    {
        inside.Step();
    }
    EXPECT_EQ(inside.controller.Scale(), 1.0);
    EXPECT_FALSE(inside.controller.Correcting());
    EXPECT_NEAR(inside.Error(), 0.4, 1e-6);

    // Past the dead zone a correction starts; it ends only when the error is under half the zone
    TimeDilationController controller(On());
    std::uint64_t now = 1'000'000;
    controller.Update(now, 0.6);
    EXPECT_TRUE(controller.Correcting());
    for (int i = 0; i < 300; ++i)
    {
        now += 10'000;
        controller.Update(now, 0.35);
    }
    EXPECT_TRUE(controller.Correcting()) << "0.35 is inside the zone but not under half of it";
    for (int i = 0; i < 300; ++i)
    {
        now += 10'000;
        controller.Update(now, 0.2);
    }
    EXPECT_FALSE(controller.Correcting());
    EXPECT_NEAR(controller.Scale(), 1.0, 1e-9);
}

TEST(TimeDilationControllerTest, JitterInTheMeasurementStaysBoundedAndDoesNotOscillate)
{
    TimeDilationOptions options = On();
    Plant plant(options, 3.0);
    std::uint32_t i = 0;
    for (; i < 3000; ++i)
    {
        plant.Step(Noise(i) * 1.5); // +-1.5 ticks of measurement noise
        ASSERT_GE(plant.scales.back(), 0.9 - 1e-9);
        ASSERT_LE(plant.scales.back(), 1.1 + 1e-9);
    }
    EXPECT_LE(std::abs(plant.Error()), 1.0) << "the true lead is still near the target";
    EXPECT_EQ(plant.controller.Stats().resyncs, 0u);
    // After it settled (5 s), the scale does not swing from one side of 1 to the other
    EXPECT_LE(ZeroCrossings(plant.scales, 500, 0.01), 2);
    for (std::size_t k = 1; k < plant.scales.size(); ++k)
    {
        EXPECT_LE(std::abs(plant.scales[k] - plant.scales[k - 1]), 0.0025 + 1e-9);
    }
}

TEST(TimeDilationControllerTest, ItRecoversAfterTheTargetStepsAway)
{
    Plant plant(On(), 0.0);
    for (int i = 0; i < 500; ++i)
    {
        plant.Step();
    }
    ASSERT_NEAR(plant.Error(), 0.0, 1e-6);
    // The latency grew: the target moved 6 ticks ahead of the client (the client is now behind it)
    plant.target += 6.0;
    EXPECT_NEAR(plant.Error(), -6.0, 1e-6);
    for (int i = 0; i < 1500; ++i)
    {
        plant.Step();
    }
    EXPECT_LE(std::abs(plant.Error()), 1.0);
    EXPECT_EQ(plant.controller.Stats().resyncs, 0u);
    // ... and the other way
    plant.target -= 6.0;
    for (int i = 0; i < 1500; ++i)
    {
        plant.Step();
    }
    EXPECT_LE(std::abs(plant.Error()), 1.0);
}

TEST(TimeDilationControllerTest, AFarOffClientIsResyncedByWholeTicksOnceThenWaits)
{
    // Ahead by 20: hold 20 ticks at once
    Plant plant(On(), 20.0);
    plant.Step();
    EXPECT_EQ(plant.controller.Stats().resyncs, 1u);
    EXPECT_EQ(plant.controller.Stats().ticksHeld, 20u);
    EXPECT_NEAR(plant.Error(), 0.0, 0.2) << "the plant applied the jump";
    EXPECT_EQ(plant.controller.TakeResyncTicks(), 0) << "taken once";
    EXPECT_EQ(plant.controller.Scale(), 1.0);

    // For the cooldown it does not judge again, even with a (stale) large error
    for (int i = 0; i < 50; ++i)
    {
        plant.controller.Update(plant.now + 10'000 * (i + 1), 25.0);
    }
    EXPECT_EQ(plant.controller.Stats().resyncs, 1u);
    EXPECT_TRUE(plant.controller.InCooldown(plant.now + 100'000));

    // Behind by 15: run 15 extra ticks
    Plant behind(On(), -15.0);
    behind.Step();
    EXPECT_EQ(behind.controller.Stats().ticksSkipped, 15u);
    EXPECT_NEAR(behind.Error(), 0.0, 0.2);

    // A resync is not a gentle error: just under the limit keeps dilating instead
    Plant gentle(On(), 9.0);
    for (int i = 0; i < 5; ++i)
    {
        gentle.Step();
    }
    EXPECT_EQ(gentle.controller.Stats().resyncs, 0u);
    EXPECT_LT(gentle.controller.Scale(), 1.0);

    // After the cooldown it converges and ignores a non-finite measurement
    plant.controller.Update(plant.now + 5'000'000, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(plant.controller.Stats().resyncs, 1u);
}

TEST(TimeDilationControllerTest, ChangingTheOptionsClampsThemAndRestartsOnAToggle)
{
    Plant plant(On(), 5.0);
    for (int i = 0; i < 100; ++i)
    {
        plant.Step();
    }
    ASSERT_LT(plant.controller.Scale(), 1.0);
    TimeDilationOptions off = plant.controller.Options();
    off.enabled = false;
    plant.controller.SetOptions(off);
    EXPECT_EQ(plant.controller.Scale(), 1.0);
    TimeDilationOptions again = On();
    again.maxScale = 7.0;
    plant.controller.SetOptions(again);
    EXPECT_EQ(plant.controller.Options().maxScale, 2.0);
    EXPECT_EQ(plant.controller.Scale(), 1.0);
}

// ---- The project settings ---------------------------------------------------------------------------------------------

TEST(TimeDilationSettingsTest, OffByDefaultAndReadFromTheAuthoritativeBlock)
{
    const NetworkSettings none = ReadNetworkSettings(nlohmann::json::object());
    EXPECT_FALSE(none.authoritative.timeDilation.enabled);
    EXPECT_EQ(none.authoritative.timeDilation.minScale, 0.9);
    EXPECT_EQ(none.authoritative.timeDilation.maxScale, 1.1);
    EXPECT_FALSE(TimeDilationOptions::FromSettings(none).enabled);

    std::vector<std::string> problems;
    const nlohmann::json json = {{"network",
                                  {{"preset", "authoritative"},
                                   {"authoritative", {{"timeDilation", {{"enabled", true}, {"minScale", 0.8}, {"maxScale", 1.25}}}}}}}};
    const NetworkSettings parsed = ReadNetworkSettings(json, &problems);
    EXPECT_TRUE(problems.empty());
    EXPECT_TRUE(parsed.authoritative.timeDilation.enabled);
    EXPECT_EQ(parsed.authoritative.timeDilation.minScale, 0.8);
    EXPECT_EQ(parsed.authoritative.timeDilation.maxScale, 1.25);
    const TimeDilationOptions options = TimeDilationOptions::FromSettings(parsed);
    EXPECT_TRUE(options.enabled);
    EXPECT_EQ(options.minScale, 0.8);
    EXPECT_EQ(options.maxScale, 1.25);
}

TEST(TimeDilationSettingsTest, BadValuesAreClampedOrRefusedWithAProblemEachLikeTheOtherSettings)
{
    std::vector<std::string> problems;
    const nlohmann::json json = {{"network",
                                  {{"authoritative",
                                    {{"timeDilation", {{"enabled", "yes"}, {"minScale", 0.1}, {"maxScale", "big"}}}}}}}};
    const NetworkSettings parsed = ReadNetworkSettings(json, &problems);
    EXPECT_EQ(problems.size(), 3u);
    EXPECT_FALSE(parsed.authoritative.timeDilation.enabled);
    EXPECT_EQ(parsed.authoritative.timeDilation.minScale, 0.5) << "clamped to the lowest allowed";
    EXPECT_EQ(parsed.authoritative.timeDilation.maxScale, 1.1) << "refused: the default";

    problems.clear();
    (void)ReadNetworkSettings(nlohmann::json::parse(R"({"network":{"authoritative":{"timeDilation":5}}})"), &problems);
    EXPECT_EQ(problems.size(), 1u);
}

// ---- The whole loop on the network simulator -------------------------------------------------------------------------

namespace
{
    InputLayout JumpLayout()
    {
        InputControlSpec spec;
        spec.name = "jump";
        spec.kind = InputControlKind::Button;
        spec.bits = 1;
        spec.range = 1.0f;
        spec.isSigned = false;
        return InputLayout::Create({spec}).value();
    }

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

    /// A server and one client over a link, plus the client's game: a fixed-step clock that stamps one input per tick and
    /// is driven the way Application::Tick drives it (the frame delta times FixedStepScale, and the resync ticks once)
    class Session
    {
    public:
        explicit Session(const std::uint64_t latencyMicros = 30'000, const std::uint64_t jitterMicros = 0,
                         const TimeDilationOptions &dilation = {})
            : network(7), serverPeer("server"), clientPeer("client")
        {
            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.inputLayout = JumpLayout();
            options.input.delayTicks = 2;
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverPeer.world, std::move(options));
            server->SetCurrentTick(kStartTick);

            clientEndpoint = &network.AddEndpoint();
            link.latencyMicros = latencyMicros;
            link.jitterMicros = jitterMicros;
            network.Link(serverEndpoint->Id(), clientEndpoint->Id(), link);
            NetworkClientOptions client_options;
            client_options.protocolId = kProtocol;
            client_options.scenePath = kScenePath;
            client_options.sceneHash = kSceneHash;
            client_options.prefabs = PrefabTable::Capture();
            client_options.inputLayout = JumpLayout();
            client_options.serverInputDelayTicks = 2;
            client_options.timeDilation = dilation;
            client = std::make_unique<NetworkClient>(*clientEndpoint, clientPeer.world, std::move(client_options));
        }

        void SetLatency(const std::uint64_t latencyMicros)
        {
            link.latencyMicros = latencyMicros;
            (void)network.SetLinkConfig(serverEndpoint->Id(), clientEndpoint->Id(), link);
            (void)network.SetLinkConfig(clientEndpoint->Id(), serverEndpoint->Id(), link);
        }

        [[nodiscard]] std::uint32_t TickAt(const std::uint64_t now) const
        {
            return static_cast<std::uint32_t>(kStartTick + now * 60 / 1'000'000);
        }

        void Step()
        {
            network.Advance(1000);
            const std::uint64_t now = network.NowMicros();
            server->SetCurrentTick(TickAt(now));
            server->Update(now);
            client->Update(now + kClientClockOffset);
            if (gameRuns)
            {
                accumulator += 0.001 * client->FixedStepScale();
                accumulator += static_cast<double>(client->TakeFixedStepAdjustTicks()) * kDt;
                while (accumulator >= kDt)
                {
                    accumulator -= kDt;
                    const std::vector<float> values{0.0f};
                    EXPECT_TRUE(client->QueueInput(gameTick++, values).has_value());
                    ++gameTicks;
                }
            }
            scalesSeen.push_back(client->FixedStepScale());
        }

        bool RunUntil(const std::function<bool()> &done, const std::uint64_t limitMillis = 20'000)
        {
            for (std::uint64_t i = 0; i < limitMillis && !done(); ++i)
            {
                Step();
            }
            return done();
        }

        void RunFor(const std::uint64_t millis)
        {
            for (std::uint64_t i = 0; i < millis; ++i)
            {
                Step();
            }
        }

        /// Joined, clock synced and a second of settling
        void JoinAndSync()
        {
            ASSERT_TRUE(RunUntil([&] { return client->IsJoined() && client->ClockSynced(); }));
            RunFor(1500);
        }

        /// The game starts stamping `ahead` ticks beyond the suggested tick
        void StartGame(const int ahead)
        {
            const std::optional<std::uint32_t> suggested = client->SuggestedInputTick();
            ASSERT_TRUE(suggested.has_value());
            gameTick = *suggested + static_cast<std::uint32_t>(ahead);
            gameRuns = true;
            scalesSeen.clear();
        }

        N2Net::SimNetwork network;
        Peer serverPeer;
        Peer clientPeer;
        N2Net::SimTransport *serverEndpoint = nullptr;
        N2Net::SimTransport *clientEndpoint = nullptr;
        N2Net::SimLinkConfig link;
        std::unique_ptr<NetworkServer> server;
        std::unique_ptr<NetworkClient> client;
        bool gameRuns = false;
        double accumulator = 0.0;
        std::uint32_t gameTick = 0;
        std::uint64_t gameTicks = 0;
        std::vector<double> scalesSeen;
    };

    TimeDilationOptions Enabled()
    {
        TimeDilationOptions options;
        options.enabled = true;
        return options;
    }
}

TEST(SuggestedInputTickTest, NoneBeforeTheClockSyncsThenTheDocumentedFormula)
{
    Session session(40'000);
    EXPECT_FALSE(session.client->SuggestedInputTick().has_value());
    EXPECT_FALSE(session.client->SuggestedInputTickExact().has_value());
    EXPECT_FALSE(session.client->InputLeadTicks().has_value());
    session.JoinAndSync();

    const std::uint64_t rtt = session.client->RttMicros();
    const std::uint64_t jitter = session.client->JitterMicros();
    EXPECT_NEAR(static_cast<double>(rtt), 80'000.0, 6'000.0) << "40 ms each way";
    const double rate = session.client->ServerTickRate();
    ASSERT_EQ(rate, 60.0);
    const double expectedLead = std::ceil((static_cast<double>(rtt) / 2.0 + static_cast<double>(jitter)) * rate / 1.0e6);
    const double expected = std::floor(session.client->ServerTick()) + expectedLead - 2.0;
    const std::optional<std::uint32_t> suggested = session.client->SuggestedInputTick();
    ASSERT_TRUE(suggested.has_value());
    EXPECT_EQ(*suggested, static_cast<std::uint32_t>(expected));
    // One-way 40 ms = 2.4 ticks, rounded up to 3, less the server's delay of 2: the suggestion is about 1 tick ahead of
    // the server's own tick
    EXPECT_EQ(static_cast<double>(*suggested) - std::floor(session.client->ServerTick()), expectedLead - 2.0);

    // The exact value is within one tick below the rounded one
    const std::optional<double> exact = session.client->SuggestedInputTickExact();
    ASSERT_TRUE(exact.has_value());
    EXPECT_LE(*exact, static_cast<double>(*suggested) + 1.0);
    EXPECT_GT(*exact, static_cast<double>(*suggested) - 1.0);
}

TEST(SuggestedInputTickTest, TheJitterAllowanceAndTheServersDelayAreInTheFormula)
{
    Session session(30'000, 6'000);
    session.JoinAndSync();
    const double base = *session.client->SuggestedInputTickExact();
    EXPECT_GT(session.client->JitterMicros(), 0u);
    const double rate = 60.0;
    const double expected = session.client->ServerTick() +
                            (static_cast<double>(session.client->RttMicros()) / 2.0 +
                             static_cast<double>(session.client->JitterMicros())) *
                                rate / 1.0e6 -
                            2.0;
    EXPECT_NEAR(base, expected, 1e-6);
}

TEST(TimeDilationSessionTest, OffTheClientRunsAtTheRateOfTheServerAndKeepsItsLead)
{
    Session session(30'000, 0, TimeDilationOptions{});
    session.JoinAndSync();
    session.StartGame(6);
    session.RunFor(12'000);
    ASSERT_TRUE(session.client->InputLeadTicks().has_value());
    EXPECT_GT(*session.client->InputLeadTicks(), 4.0) << "nothing corrects the lead";
    EXPECT_LT(*session.client->InputLeadTicks(), 8.0);
    for (const double scale : session.scalesSeen)
    {
        ASSERT_EQ(scale, 1.0);
    }
    EXPECT_EQ(session.client->TimeDilationStatistics().updates, 0u);
    EXPECT_EQ(session.client->TakeFixedStepAdjustTicks(), 0);
    EXPECT_NEAR(static_cast<double>(session.gameTicks), 12.0 * 60.0, 2.0);
}

TEST(TimeDilationSessionTest, OnTheLeadConvergesToTheTargetUnderASteadyRoundTrip)
{
    Session session(30'000, 0, Enabled());
    session.JoinAndSync();
    session.StartGame(6);
    session.RunFor(3'000);
    EXPECT_LT(*std::min_element(session.scalesSeen.begin(), session.scalesSeen.end()), 0.99) << "ahead: the clock slowed";
    session.RunFor(12'000);
    for (int i = 0; i < 100; ++i)
    {
        session.RunFor(50);
        ASSERT_TRUE(session.client->InputLeadTicks().has_value());
        EXPECT_LE(std::abs(*session.client->InputLeadTicks()), 1.6);
    }
    for (const double scale : session.scalesSeen)
    {
        ASSERT_GE(scale, 0.9 - 1e-9);
        ASSERT_LE(scale, 1.1 + 1e-9);
    }
    EXPECT_EQ(session.client->TimeDilationStatistics().resyncs, 0u);
    EXPECT_NEAR(session.client->FixedStepScale(), 1.0, 0.02) << "settled";

    // Started behind: the clock runs fast
    Session behind(30'000, 0, Enabled());
    behind.JoinAndSync();
    behind.StartGame(-6);
    behind.RunFor(3'000);
    EXPECT_GT(*std::max_element(behind.scalesSeen.begin(), behind.scalesSeen.end()), 1.01);
    behind.RunFor(12'000);
    EXPECT_LE(std::abs(*behind.client->InputLeadTicks()), 1.6);
}

TEST(TimeDilationSessionTest, UnderJitterTheScaleStaysBoundedAndTheLeadNearTheTarget)
{
    Session session(30'000, 8'000, Enabled());
    session.JoinAndSync();
    session.StartGame(4);
    session.RunFor(20'000);
    session.scalesSeen.clear();
    double worst = 0.0;
    for (int i = 0; i < 200; ++i)
    {
        session.RunFor(50);
        worst = std::max(worst, std::abs(*session.client->InputLeadTicks()));
    }
    EXPECT_LE(worst, 3.0) << "the target itself moves with the jittery estimate";
    for (const double scale : session.scalesSeen)
    {
        ASSERT_GE(scale, 0.9 - 1e-9);
        ASSERT_LE(scale, 1.1 + 1e-9);
    }
    EXPECT_LE(ZeroCrossings(session.scalesSeen, 0, 0.01), 4) << "no oscillation";
    EXPECT_EQ(session.client->TimeDilationStatistics().resyncs, 0u);
}

TEST(TimeDilationSessionTest, ItRecoversAfterTheLatencyStepsUp)
{
    Session session(30'000, 0, Enabled());
    session.JoinAndSync();
    session.StartGame(0);
    session.RunFor(8'000);
    const double before = *session.client->SuggestedInputTickExact() - std::floor(session.client->ServerTick());
    session.SetLatency(80'000);
    session.RunFor(45'000); // the round trip estimate is a median of recent pings: it takes a while to follow
    const double after = *session.client->SuggestedInputTickExact() - std::floor(session.client->ServerTick());
    EXPECT_GT(after, before + 2.0) << "the step was noticed";
    EXPECT_LE(std::abs(*session.client->InputLeadTicks()), 1.6);
    for (const double scale : session.scalesSeen)
    {
        ASSERT_GE(scale, 0.9 - 1e-9);
        ASSERT_LE(scale, 1.1 + 1e-9);
    }
}

TEST(TimeDilationSessionTest, AFarOffClientIsHeldAndThenConverges)
{
    Session session(30'000, 0, Enabled());
    session.JoinAndSync();
    session.StartGame(20);
    session.RunFor(15'000);
    const TimeDilationStats stats = session.client->TimeDilationStatistics();
    EXPECT_GE(stats.resyncs, 1u);
    EXPECT_GE(stats.ticksHeld, 15u);
    EXPECT_EQ(stats.ticksSkipped, 0u);
    EXPECT_LE(std::abs(*session.client->InputLeadTicks()), 1.6);
}

TEST(TimeDilationSessionTest, TheControlsCanBeChangedWhileTheClientRuns)
{
    Session session(30'000, 0, TimeDilationOptions{});
    session.JoinAndSync();
    session.StartGame(6);
    session.RunFor(3'000);
    EXPECT_EQ(session.client->FixedStepScale(), 1.0);
    TimeDilationOptions options = session.client->GetTimeDilation();
    options.enabled = true;
    options.maxScale = 7.0; // clamped
    session.client->SetTimeDilation(options);
    EXPECT_EQ(session.client->GetTimeDilation().maxScale, 2.0);
    session.RunFor(10'000);
    EXPECT_LE(std::abs(*session.client->InputLeadTicks()), 1.6);
    options.enabled = false;
    session.client->SetTimeDilation(options);
    EXPECT_EQ(session.client->FixedStepScale(), 1.0);
}

// ---- Lua ----------------------------------------------------------------------------------------------------------------

TEST(TimeDilationSessionTest, LuaReadsAndChangesTheDilationAndReadsTheSuggestedTick)
{
    static const bool luaReady = Scripting::LuaRuntime::Instance().Initialize();
    ASSERT_TRUE(luaReady);
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    Session session(30'000, 0, TimeDilationOptions{}); // the most recent client is the active one
    const auto run = [&lua](const std::string &code) { return lua.safe_script(code, sol::script_pass_on_error); };

    const sol::protected_function_result before = run("return Network.GetSuggestedInputTick()");
    ASSERT_TRUE(before.valid());
    EXPECT_EQ(before.get_type(), sol::type::lua_nil) << "the clock has not synced";
    session.JoinAndSync();
    session.StartGame(6);
    const sol::protected_function_result suggested = run("return Network.GetSuggestedInputTick()");
    ASSERT_TRUE(suggested.valid());
    EXPECT_EQ(suggested.get<double>(), static_cast<double>(*session.client->SuggestedInputTick()));

    const sol::protected_function_result got = run("return Network.GetTimeDilation()");
    ASSERT_TRUE(got.valid());
    const sol::table table = got;
    EXPECT_FALSE(table["enabled"].get<bool>());
    EXPECT_DOUBLE_EQ(table["minScale"].get<double>(), 0.9);
    EXPECT_DOUBLE_EQ(table["maxScale"].get<double>(), 1.1);
    EXPECT_DOUBLE_EQ(table["scale"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(table["resyncTicks"].get<double>(), 10.0);

    const sol::protected_function_result set =
        run("return Network.SetTimeDilation({ enabled = true, minScale = 0.8, resyncTicks = 12, smoothingMs = 300 })");
    ASSERT_TRUE(set.valid());
    EXPECT_TRUE(set.get<bool>());
    EXPECT_TRUE(session.client->GetTimeDilation().enabled);
    EXPECT_DOUBLE_EQ(session.client->GetTimeDilation().minScale, 0.8);
    EXPECT_DOUBLE_EQ(session.client->GetTimeDilation().maxScale, 1.1) << "fields not given keep their value";
    EXPECT_EQ(session.client->GetTimeDilation().smoothingMicros, 300'000u);

    // A typo, a bad type or a read-only field is an error and changes nothing
    EXPECT_FALSE(run("Network.SetTimeDilation({ minScal = 0.5 })").valid());
    EXPECT_FALSE(run("Network.SetTimeDilation({ enabled = 1 })").valid());
    EXPECT_FALSE(run("Network.SetTimeDilation({ maxScale = -1 })").valid());
    EXPECT_FALSE(run("Network.SetTimeDilation({ scale = 1 })").valid());
    EXPECT_FALSE(run("Network.SetTimeDilation({ maxScale = 1.5, resyncTicks = 'x' })").valid());
    EXPECT_DOUBLE_EQ(session.client->GetTimeDilation().maxScale, 1.1);
    EXPECT_DOUBLE_EQ(session.client->GetTimeDilation().minScale, 0.8);

    session.RunFor(2'000);
    const sol::protected_function_result lead = run("return Network.GetTimeDilation().leadTicks");
    ASSERT_TRUE(lead.valid());
    EXPECT_EQ(lead.get_type(), sol::type::number);
}

// ---- The Application hook ---------------------------------------------------------------------------------------------

namespace
{
    constexpr double kExactStep = 1.0 / 64.0;
}

class TimeDilationApplicationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Config::ApplicationOptions options;
        options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
        options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
        options.isHeadless = true;
        Window &window = Application::GetInstance().GetWindow();
        ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
        ASSERT_TRUE(window.SetRenderSize(8, 4));
        Application &app = Application::GetInstance();
        app.Set3DPhysicsBackend(nullptr);
        app.SetSimulationMode(SimulationMode::Authoritative);
        app.GetSimulationClock().SetBehindHandler({});
        app.GetSimulationClock().Reset();
        Time::SetTimeScale(1.0f);
        ASSERT_TRUE(Time::SetFixedTimestep(kExactStep));
        SceneManager::AddScene(Scene::Create("TimeDilationApplicationTest_" + std::to_string(++sceneCounter)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    }

    void TearDown() override
    {
        session.reset();
        Application &app = Application::GetInstance();
        app.SetSimulationMode(SimulationMode::None);
        app.GetSimulationClock().Reset();
        Time::SetFixedTimestep(0.02);
        TickOptions rest;
        rest.render = false;
        rest.pollEvents = false;
        rest.deltaSeconds = 0.0;
        app.Tick(rest);
        SceneManager::AddScene(Scene::Create("TimeDilationApplicationTest_Empty_" + std::to_string(++sceneCounter)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        app.GetWindow().Shutdown();
    }

    static void TickFor(const double seconds)
    {
        TickOptions options;
        options.render = false;
        options.pollEvents = false;
        options.deltaSeconds = seconds;
        Application::GetInstance().Tick(options);
    }

    /// Ticks run by five 0.25 s frames
    static std::uint64_t TicksOverFiveFrames()
    {
        const std::uint64_t before = Time::GetTick();
        for (int i = 0; i < 5; ++i)
        {
            TickFor(0.25);
        }
        return Time::GetTick() - before;
    }

    /// A joined client whose newest stamp is `ahead` ticks beyond the target, after one Update that judged it
    void MakeClientAhead(const bool enabled, const int ahead)
    {
        session = std::make_unique<Session>(30'000, 0, enabled ? Enabled() : TimeDilationOptions{});
        session->JoinAndSync();
        const std::vector<float> values{0.0f};
        ASSERT_TRUE(session->client->QueueInput(*session->client->SuggestedInputTick() + static_cast<std::uint32_t>(ahead), values)
                        .has_value());
        session->Step();
    }

    std::unique_ptr<Session> session;
    static inline int sceneCounter = 0;
};

TEST_F(TimeDilationApplicationTest, AResyncHoldsTicksInTheAuthoritativeLoopAndOnlyThere)
{
    MakeClientAhead(true, 20);
    const std::uint64_t held = session->client->TimeDilationStatistics().ticksHeld;
    ASSERT_GE(held, 15u);
    // 5 * 0.25 s is 80 ticks of 1/64 s; the resync holds `held` of them back, once
    EXPECT_EQ(TicksOverFiveFrames(), 80u - held);
    EXPECT_EQ(session->client->TakeFixedStepAdjustTicks(), 0) << "the hook took it";
}

TEST_F(TimeDilationApplicationTest, DilationOffLeavesTheLoopAsItWas)
{
    MakeClientAhead(false, 20);
    EXPECT_EQ(TicksOverFiveFrames(), 80u);
}

TEST_F(TimeDilationApplicationTest, TheNoneModeNeverAppliesIt)
{
    MakeClientAhead(true, 20);
    Application::GetInstance().SetSimulationMode(SimulationMode::None);
    // None mode: the time scale is the old one's and the hook is not consulted
    EXPECT_EQ(TicksOverFiveFrames(), 80u);
    EXPECT_NE(session->client->TakeFixedStepAdjustTicks(), 0) << "still pending: nothing consumed it";
}

TEST_F(TimeDilationApplicationTest, AFixedTickDriverKeepsTheLoopToItself)
{
    MakeClientAhead(true, 20);
    Application &app = Application::GetInstance();
    std::uint64_t driverCalls = 0;
    app.SetFixedTickDriver([&](double) { ++driverCalls; });
    TickFor(0.25);
    app.SetFixedTickDriver(nullptr);
    EXPECT_EQ(driverCalls, 1u);
    EXPECT_NE(session->client->TakeFixedStepAdjustTicks(), 0) << "a lockstep or rollback driver is never dilated";
}

TEST_F(TimeDilationApplicationTest, TheScaleSpeedsOrSlowsTheFrameDelta)
{
    session = std::make_unique<Session>(30'000, 0, Enabled());
    session->JoinAndSync();
    session->StartGame(6);
    ASSERT_TRUE(session->RunUntil([&] { return session->client->FixedStepScale() < 0.95; }, 5'000));
    session->gameRuns = false;
    const double scale = session->client->FixedStepScale();
    ASSERT_LT(scale, 0.95);
    ASSERT_EQ(session->client->TakeFixedStepAdjustTicks(), 0);
    const std::uint64_t before = Time::GetTick();
    TickFor(0.25);
    EXPECT_EQ(Time::GetTick() - before, static_cast<std::uint64_t>(std::floor(0.25 * scale / kExactStep)));
    EXPECT_LT(Time::GetTick() - before, 16u) << "slower than real time";
    EXPECT_EQ(Time::GetFixedDeltaTime(), static_cast<float>(kExactStep)) << "the physics dt is never scaled";
}
