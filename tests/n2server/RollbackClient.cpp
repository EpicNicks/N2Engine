// n2server_rollback_client: the test client of tests/smoke/n2server_rollback.py: a headless LockstepRuntime in ROLLBACK mode
// (rollbackWindow > 0: a RollbackSession runs the ticks on confirmed input and on a guess for the rest, and rolls back and
// resimulates when a late input differs) over the C2 replay scene (ReplaySimulation, whose Save and Restore are the
// snapshot hooks), or a late SPECTATOR that starts from a player's snapshot. It joins a running `N2Server --relay` through
// the secure handshake. It is a test tool: it is not a shipped game client and it is not packaged. It reports what happens,
// one line each on stdout (the lines of n2server_lockstep_client, plus the rollback ones):
//
//   welcome slot=<s> slots=<n> tickRate=<r> hashInterval=<i>      (a spectator: welcome spectator slots=<n> ... slot=255)
//   rollback window=<w> mispredict=<0|1>                          once, after the welcome, for a player
//   started
//   hash tick=<t> 0x<hash>        every checkpoint, the scene's state hash. A player prints it only once the checkpoint is
//                                 CONFIRMED (every tick before it ran on confirmed input), so the lines of a player, of a
//                                 lockstep client and of a spectator are comparable; a spectator prints the checkpoints
//                                 after its start
//   spectator-joined tick=<t> snapshotBytes=<n>                   a spectator took its snapshot and starts at tick t
//   snapshot-sent tick=<t>                                        a player sent its snapshot whole to the relay
//   dropped slot=<s> from=<t> reason=<text>
//   stopped reason=<text> tick=<t>
//   rollback-stats window=<w> rollbacks=<n> resimulated=<n> mispredictions=<n> predicted=<n> deepest=<n> snapshots=<n> ran=<n>
//                                 at the end of a player's run (not for a spectator or a client killed from outside)
//   refused reason=<text> / failed <text> / timeout state=<s>
//   replay path=<p> ticks=<n>                                     (--replay-out)
//   done ticks=<n>                                                --ticks reached; the client left
//
//   n2server_rollback_client --port <n> --layout-file <json> [--host <ip>] [--protocol-id <n>] [--scene <name>] [--seed <n>]
//       [--ticks <n>] [--rollback-window <0..15>] [--spectate <0|1>] [--mispredict <0|1>] [--hitch-every <ticks>]
//       [--hitch-ms <n>] [--snapshot-pad-kb <n>] [--stall-at-tick <t>] [--report <path>] [--replay-out <path>]
//       [--max-ms <n>] [--peer-name <text>]
//
// The join token is the first line of stdin. The injected misprediction has two parts, because on loopback every remote
// input normally arrives before its tick, so nothing would ever be guessed: --hitch-every <t> with --hitch-ms <m> makes the
// client freeze for m milliseconds each time it passes a multiple of t ticks, as a frame hitch would (the OTHER players then
// run ticks on a guess), and --mispredict 1 makes every guess wrong (the guess is the last confirmed input with the jump
// button flipped), so every guessed tick is rolled back. The inputs change every 3 ticks. --snapshot-pad-kb adds that many
// KiB of padding to every snapshot this player makes, so that a snapshot sent to a spectator takes many parts (a test of a
// source that dies during the transfer); the spectator ignores the padding. --stall-at-tick stops the client dead at that
// tick, as a hung process would. Exit code 0 for done, a stop by the relay, 1 for a failure, a refusal or a timeout.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/network/LockstepRuntime.hpp"
#include "engine/network/n2net/SecureTransport.hpp"
#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/replay/GoldenReplay.hpp"
#include "engine/replay/LockstepReplay.hpp"
#include "engine/security/Crypto.hpp"
#include "n2server/ServerOptions.hpp"

using namespace N2Engine;

namespace
{
    struct Arguments
    {
        std::string host = "127.0.0.1";
        int port = 0;
        std::uint32_t protocolId = Server::ServerOptions::DefaultProtocolId;
        std::string layoutFile;
        std::string scene;
        std::uint32_t seed = 7;
        long long ticks = -1;
        int rollbackWindow = 6;
        bool spectate = false;
        bool mispredict = false;
        long long hitchEvery = 0;
        int hitchMs = 0;
        long long snapshotPadKb = 0;
        long long stallAtTick = -1;
        std::string report;
        std::string replayOut;
        int maxMs = 90000;
        std::string peerName = "client";
    };

    void Say(const std::string &line)
    {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    std::optional<Arguments> ParseArguments(int argc, char *argv[])
    {
        Arguments a;
        for (int i = 1; i + 1 < argc; i += 2)
        {
            const std::string_view key = argv[i];
            const std::string value = argv[i + 1];
            if (key == "--host") a.host = value;
            else if (key == "--port") a.port = std::atoi(value.c_str());
            else if (key == "--protocol-id")
            {
                const auto id = Server::ParseProtocolId(value);
                if (!id) return std::nullopt;
                a.protocolId = *id;
            }
            else if (key == "--layout-file") a.layoutFile = value;
            else if (key == "--scene") a.scene = value;
            else if (key == "--seed") a.seed = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            else if (key == "--ticks") a.ticks = std::atoll(value.c_str());
            else if (key == "--rollback-window") a.rollbackWindow = std::atoi(value.c_str());
            else if (key == "--spectate") a.spectate = value != "0";
            else if (key == "--mispredict") a.mispredict = value != "0";
            else if (key == "--hitch-every") a.hitchEvery = std::atoll(value.c_str());
            else if (key == "--hitch-ms") a.hitchMs = std::atoi(value.c_str());
            else if (key == "--snapshot-pad-kb") a.snapshotPadKb = std::atoll(value.c_str());
            else if (key == "--stall-at-tick") a.stallAtTick = std::atoll(value.c_str());
            else if (key == "--report") a.report = value;
            else if (key == "--replay-out") a.replayOut = value;
            else if (key == "--max-ms") a.maxMs = std::atoi(value.c_str());
            else if (key == "--peer-name") a.peerName = value;
            else return std::nullopt;
        }
        if (a.port <= 0 || a.port > 65535 || a.layoutFile.empty() || a.rollbackWindow < 0 ||
            a.rollbackWindow > static_cast<int>(Network::kMaxRollbackWindow) || a.hitchEvery < 0 || a.hitchMs < 0 ||
            a.snapshotPadKb < 0 || a.snapshotPadKb > 8192)
        {
            return std::nullopt;
        }
        return a;
    }

    std::array<std::uint8_t, 8> Bytes(const std::uint64_t value)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (int i = 0; i < 8; ++i)
        {
            bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
        return bytes;
    }

    // The scene's snapshot blob travels in the opaque host bytes of a WorldSnapshot (the part meant for the physics
    // backend's own bytes: this scene has no physics): u32 blob size, the blob, then the padding (--snapshot-pad-kb)
    void PackSnapshot(const std::vector<std::uint8_t> &blob, const std::size_t padBytes, Network::WorldSnapshot &out)
    {
        out.hasPhysics = true;
        out.physics.clear();
        out.physics.reserve(4 + blob.size() + padBytes);
        for (int i = 0; i < 4; ++i)
        {
            out.physics.push_back(static_cast<std::byte>((blob.size() >> (8 * i)) & 0xFFu));
        }
        for (const std::uint8_t byte : blob)
        {
            out.physics.push_back(static_cast<std::byte>(byte));
        }
        out.physics.resize(out.physics.size() + padBytes, std::byte{0});
    }

    std::expected<std::vector<std::uint8_t>, std::string> UnpackSnapshot(const Network::WorldSnapshot &snapshot)
    {
        if (!snapshot.hasPhysics || snapshot.physics.size() < 4)
        {
            return std::unexpected(std::string("the snapshot carries no scene state"));
        }
        std::size_t size = 0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            size |= static_cast<std::size_t>(std::to_integer<std::uint8_t>(snapshot.physics[i])) << (8 * i);
        }
        if (size > snapshot.physics.size() - 4)
        {
            return std::unexpected(std::string("the snapshot's scene state is longer than the snapshot"));
        }
        std::vector<std::uint8_t> blob(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            blob[i] = std::to_integer<std::uint8_t>(snapshot.physics[4 + i]);
        }
        return blob;
    }

    int Run(const Arguments &args, std::string &token)
    {
        std::ifstream layoutStream(args.layoutFile, std::ios::binary);
        std::stringstream layoutText;
        layoutText << layoutStream.rdbuf();
        auto layout = Network::ParseInputLayoutJson(layoutText.str());
        if (!layout)
        {
            std::fprintf(stderr, "layout: %s\n", layout.error().c_str());
            return 1;
        }
        const std::string scene = args.scene.empty() ? Replay::SceneNames().front() : args.scene;
        std::string error;
        std::unique_ptr<Replay::ReplaySimulation> simulation = Replay::ReplaySimulation::Create(scene, args.seed, error);
        if (!simulation)
        {
            std::fprintf(stderr, "scene: %s\n", error.c_str());
            return 1;
        }
        if (!Security::CryptoAvailable() || !Security::InitializeCrypto())
        {
            std::fprintf(stderr, "this build has no working crypto library\n");
            return 1;
        }
        N2Net::UdpTransportOptions udp;
        udp.acceptUnknownPeers = false;
        N2Net::SecureTransportOptions secureOptions;
        secureOptions.role = N2Net::SecureRole::Client;
        secureOptions.protocolId = args.protocolId;
        secureOptions.joinToken = token;
        N2Net::SecureUdpOpenResult opened = N2Net::SecureTransport::OpenUdp(udp, std::move(secureOptions));
        Security::SecureZero(token.data(), token.size());
        if (opened.transport == nullptr)
        {
            std::fprintf(stderr, "can't open the client transport: %s\n", opened.message.c_str());
            return 1;
        }
        const N2Net::UdpConnectResult peer = opened.udp->Connect(args.host, static_cast<std::uint16_t>(args.port));
        if (peer.error != N2Net::UdpError::None || !opened.transport->Connect(peer.peer))
        {
            std::fprintf(stderr, "can't start the handshake: %s\n", peer.message.c_str());
            return 1;
        }

        std::array<Replay::SlotState, Replay::kMaxSlots> held{};
        std::uint32_t slotCount = 0;
        Network::LockstepRuntime *runtimePtr = nullptr;
        // Checkpoint hashes built (again after every resimulation) and not yet confirmed, by tick
        std::map<std::uint32_t, std::uint64_t> pendingHashes;

        Network::LockstepRuntimeOptions options;
        options.protocolId = args.protocolId;
        options.layout = *layout;
        options.peerName = args.peerName;
        options.scene = scene;
        options.seed = args.seed;
        options.reportPath = args.report;
        options.desyncLingerMicros = 3'000'000;
        options.spectate = args.spectate;
        const Network::InputLayout layoutCopy = *layout;
        std::uint32_t slotForSampling = 0;
        options.sampleInput = [&slotForSampling, &layoutCopy](const std::uint32_t tick)
        {
            // A player changing what it holds every 3 ticks, so "the same as last time" is often wrong
            std::vector<float> values(layoutCopy.ValueCount(), 0.0f);
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                values[i] = static_cast<float>(((tick / 3) * 7 + slotForSampling * 5 + i * 3) % 21) / 10.0f - 1.0f;
            }
            return values;
        };
        options.applyTick = [&](const N2Net::TickInputs &inputs, const std::uint32_t slots)
        {
            slotCount = slots;
            if (static_cast<std::uint32_t>(simulation->TicksRun()) != inputs.tick)
            {
                std::fprintf(stderr, "the scene is at tick %d but the session runs tick %u\n", simulation->TicksRun(), inputs.tick);
                return false;
            }
            for (std::uint32_t s = 0; s < slots; ++s)
            {
                if (!Replay::SceneStateFromInput(layoutCopy, inputs.inputs[s], held[s]))
                {
                    return false;
                }
            }
            std::string stepError;
            if (!simulation->Step(std::span<const Replay::SlotState>(held.data(), slots), stepError))
            {
                std::fprintf(stderr, "scene step: %s\n", stepError.c_str());
                return false;
            }
            // A spectator reports no roots, so it prints the checkpoint it has just run (a spectator never rolls back)
            const std::uint32_t done = inputs.tick + 1;
            if (args.spectate && runtimePtr != nullptr && runtimePtr->HashInterval() != 0 && done % runtimePtr->HashInterval() == 0)
            {
                Say("hash tick=" + std::to_string(done) + " " + Replay::FormatHash(simulation->Hash()));
            }
            return true;
        };
        options.buildTree = [&](const std::uint32_t tick)
        {
            const std::uint64_t hash = simulation->Hash();
            pendingHashes.insert_or_assign(tick, hash);
            N2Net::HashTreeBuilder builder(tick);
            (void)builder.AddField(1, 0, Bytes(hash));
            for (std::uint32_t s = 0; s < slotCount; ++s)
            {
                const Replay::SlotState &state = held[s];
                const std::array<std::uint8_t, 6> encoded{
                    static_cast<std::uint8_t>(state.moveX), static_cast<std::uint8_t>(state.moveY),
                    static_cast<std::uint8_t>(state.buttons & 0xFF), static_cast<std::uint8_t>((state.buttons >> 8) & 0xFF),
                    static_cast<std::uint8_t>((state.buttons >> 16) & 0xFF), static_cast<std::uint8_t>(state.buttons >> 24)};
                (void)builder.AddField(2 + s, 0, encoded);
            }
            return builder.Build();
        };
        options.describeField = [](const std::uint32_t object, const std::uint16_t field)
        { return "object " + std::to_string(object) + " field " + std::to_string(field); };

        // Rollback: the snapshot is the scene's own blob (ReplaySimulation::Save and Restore)
        options.rollbackWindow = args.spectate ? 0u : static_cast<std::uint32_t>(args.rollbackWindow);
        const std::size_t padBytes = static_cast<std::size_t>(args.snapshotPadKb) * 1024u;
        options.save = [&](const std::uint32_t tick, Network::WorldSnapshot &out)
        {
            if (static_cast<std::uint32_t>(simulation->TicksRun()) != tick)
            {
                std::fprintf(stderr, "save: the scene is at tick %d, asked for the state before tick %u\n", simulation->TicksRun(), tick);
                return false;
            }
            std::string saveError;
            const std::vector<std::uint8_t> blob = simulation->Save(saveError);
            if (blob.empty())
            {
                std::fprintf(stderr, "save: %s\n", saveError.c_str());
                return false;
            }
            out = Network::WorldSnapshot{};
            out.tick = tick;
            PackSnapshot(blob, padBytes, out);
            return true;
        };
        const auto restoreInto = [&](const Network::WorldSnapshot &snapshot) -> std::expected<void, std::string>
        {
            auto blob = UnpackSnapshot(snapshot);
            if (!blob)
            {
                return std::unexpected(blob.error());
            }
            std::string restoreError;
            if (!simulation->Restore(*blob, restoreError))
            {
                return std::unexpected(restoreError);
            }
            if (static_cast<std::uint32_t>(simulation->TicksRun()) != snapshot.tick)
            {
                return std::unexpected(std::string("the snapshot's scene state is of another tick than the snapshot"));
            }
            return {};
        };
        options.restore = restoreInto;
        options.join = [&](const Network::WorldSnapshot &snapshot) -> std::expected<void, std::string>
        {
            if (auto restored = restoreInto(snapshot); !restored)
            {
                return restored;
            }
            Say("spectator-joined tick=" + std::to_string(snapshot.tick) + " snapshotBytes=" + std::to_string(snapshot.physics.size()));
            return {};
        };
        options.confirmed = [&](const std::uint32_t firstUnconfirmed)
        {
            // Every tick before firstUnconfirmed is final: the checkpoints up to it are what a lockstep peer hashes
            while (!pendingHashes.empty() && pendingHashes.begin()->first <= firstUnconfirmed)
            {
                Say("hash tick=" + std::to_string(pendingHashes.begin()->first) + " " +
                    Replay::FormatHash(pendingHashes.begin()->second));
                pendingHashes.erase(pendingHashes.begin());
            }
        };
        if (args.mispredict)
        {
            options.predict = [](const std::uint32_t, const std::uint32_t, const N2Net::LockstepInput &last)
            {
                // Always wrong: the jump button of the last confirmed input, flipped (the button bits are the last ones)
                N2Net::LockstepInput guess = last;
                if (guess.size > 0)
                {
                    std::uint8_t &lastByte = guess.bytes[static_cast<std::size_t>(guess.size) - 1];
                    lastByte = static_cast<std::uint8_t>(lastByte ^ 1u);
                }
                return guess;
            };
        }
        options.observer = [&](const Network::LockstepEvent &event)
        {
            using Type = Network::LockstepEvent::Type;
            switch (event.type)
            {
            case Type::Welcomed:
                Say("welcome " + event.text + " slot=" + std::to_string(event.slot));
                if (!args.spectate)
                {
                    Say("rollback window=" + std::to_string(args.rollbackWindow) + " mispredict=" + (args.mispredict ? "1" : "0"));
                }
                break;
            case Type::Started:
                Say("started");
                break;
            case Type::SlotDropped:
                Say("dropped slot=" + std::to_string(event.slot) + " from=" + std::to_string(event.tick) +
                    " reason=" + event.text);
                break;
            case Type::Stopped:
                Say("stopped reason=" + event.text + " tick=" + std::to_string(event.tick));
                break;
            case Type::ReportWritten:
                Say("report path=" + event.text);
                break;
            case Type::Refused:
                Say("refused " + event.text);
                break;
            case Type::Failed:
                Say("failed " + event.text);
                break;
            case Type::SnapshotSent:
                Say("snapshot-sent tick=" + std::to_string(event.tick));
                break;
            }
        };

        Network::LockstepRuntime runtime(*opened.transport, std::move(options));
        runtimePtr = &runtime;
        const auto start = std::chrono::steady_clock::now();
        auto last = start;
        bool done = false;
        long long nextHitch = args.hitchEvery;
        const auto printStats = [&]
        {
            if (const Network::RollbackSession *session = runtime.Session())
            {
                const Network::RollbackStats &stats = session->Stats();
                Say("rollback-stats window=" + std::to_string(args.rollbackWindow) + " rollbacks=" + std::to_string(stats.rollbacks) +
                    " resimulated=" + std::to_string(stats.ticksResimulated) + " mispredictions=" + std::to_string(stats.mispredictions) +
                    " predicted=" + std::to_string(stats.ticksPredicted) + " deepest=" + std::to_string(stats.deepestRollback) +
                    " snapshots=" + std::to_string(stats.snapshotsSaved) + " ran=" + std::to_string(stats.ticksRun));
            }
        };
        while (!runtime.Finished())
        {
            const auto now = std::chrono::steady_clock::now();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
            if (micros > static_cast<long long>(args.maxMs) * 1000)
            {
                Say("timeout state=" + std::string(Network::ToString(runtime.State())));
                return 1;
            }
            slotForSampling = runtime.Slot();
            const double delta = std::chrono::duration<double>(now - last).count();
            last = now;
            runtime.Update(static_cast<std::uint64_t>(micros), delta);
            if (args.stallAtTick >= 0 && runtime.TicksRun() >= static_cast<std::uint32_t>(args.stallAtTick))
            {
                Say("stalling");
                // A hung process: nothing is read or sent until the harness kills it
                std::this_thread::sleep_for(std::chrono::milliseconds(args.maxMs));
                return 1;
            }
            if (args.hitchEvery > 0 && args.hitchMs > 0 && runtime.State() == Network::LockstepState::Running &&
                static_cast<long long>(runtime.TicksRun()) >= nextHitch)
            {
                // A frame hitch: nothing is read or sent for a moment, so the other players run ticks on a guess
                nextHitch = static_cast<long long>(runtime.TicksRun()) + args.hitchEvery;
                std::this_thread::sleep_for(std::chrono::milliseconds(args.hitchMs));
            }
            if (args.ticks >= 0 && runtime.State() == Network::LockstepState::Running &&
                runtime.TicksRun() >= static_cast<std::uint32_t>(args.ticks))
            {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (const Replay::ReplayLog *log = runtime.Log(); log != nullptr && !args.replayOut.empty())
        {
            Replay::ReplayLog copy = *log;
            if (done && copy.ticks > args.ticks)
            {
                copy.ticks = static_cast<int>(args.ticks);
                std::erase_if(copy.inputs, [&](const Replay::ReplayInput &row) { return row.tick >= copy.ticks; });
            }
            std::ofstream out(args.replayOut, std::ios::binary);
            out << Replay::SerializeReplayLog(copy);
            Say("replay path=" + args.replayOut + " ticks=" + std::to_string(copy.ticks));
        }
        printStats();
        if (done)
        {
            Say("done ticks=" + std::to_string(runtime.TicksRun()));
            runtime.Leave();
            // Let the leave reach the relay
            for (int i = 0; i < 20; ++i)
            {
                runtime.Update(static_cast<std::uint64_t>(
                                   std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()),
                               0.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return 0;
        }
        return runtime.State() == Network::LockstepState::Stopped ? 0 : 1;
    }
}

int main(int argc, char *argv[])
{
    const std::optional<Arguments> arguments = ParseArguments(argc, argv);
    if (!arguments)
    {
        std::fprintf(stderr, "usage: n2server_rollback_client --port <n> --layout-file <json> [--host ip] [--protocol-id n] "
                             "[--scene name] [--seed n] [--ticks n] [--rollback-window 0..15] [--spectate 0|1] [--mispredict 0|1] "
                             "[--hitch-every ticks] [--hitch-ms n] [--snapshot-pad-kb n] [--stall-at-tick t] [--report path] "
                             "[--replay-out path] [--max-ms n] [--peer-name text]   (the token is the first line of stdin)\n");
        return 2;
    }
    std::string token;
    std::getline(std::cin, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n'))
    {
        token.pop_back();
    }
    const int code = Run(*arguments, token);
    std::fflush(stdout);
    std::fflush(stderr);
    return code;
}
