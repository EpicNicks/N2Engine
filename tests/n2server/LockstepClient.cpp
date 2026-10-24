// n2server_lockstep_client: the test client of tests/smoke/n2server_lockstep.py: a headless LockstepRuntime that joins a
// running `N2Server --relay` through the secure handshake and runs the C2 replay scene (ReplaySimulation) from the
// confirmed ticks. It reports what happens, one line each on stdout:
//
//   welcome slot=<s> slots=<n> tickRate=<r> hashInterval=<i>
//   started
//   hash tick=<t> 0x<hash>                       every checkpoint, the scene's state hash
//   dropped slot=<s> from=<t> reason=<text>      the relay dropped a slot (every peer applies it at that tick)
//   stopped reason=<text> tick=<t>               the relay ended the session
//   divergence kind=<k> object=<o> field=<f> rounds=<n>      what the bisection found (desync stops)
//   report path=<p>                              the n2desync report was written
//   refused reason=<text> / failed <text>
//   replay path=<p> ticks=<n>                    the recorded session was written (--replay-out)
//   done ticks=<n>                               --ticks reached; the client left
//
//   n2server_lockstep_client --port <n> --layout-file <json> [--host <ip>] [--protocol-id <n>] [--scene <name>]
//       [--seed <n>] [--ticks <n>] [--diverge-at-tick <t>] [--stall-at-tick <t>] [--report <path>]
//       [--replay-out <path>] [--max-ms <n>] [--peer-name <text>]
//
// The join token is the first line of stdin. --diverge-at-tick flips one bit of the scene's reported state from that
// tick on (the injected divergence). --stall-at-tick stops the client dead (no packets, no reads) at that tick, as a
// hung process would, until it is killed or --max-ms passes. Exit code 0 for done, a desync stop or a stop by the relay,
// 1 for a failure, a refusal or a timeout.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

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
        long long divergeAtTick = -1;
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
            else if (key == "--diverge-at-tick") a.divergeAtTick = std::atoll(value.c_str());
            else if (key == "--stall-at-tick") a.stallAtTick = std::atoll(value.c_str());
            else if (key == "--report") a.report = value;
            else if (key == "--replay-out") a.replayOut = value;
            else if (key == "--max-ms") a.maxMs = std::atoi(value.c_str());
            else if (key == "--peer-name") a.peerName = value;
            else return std::nullopt;
        }
        if (a.port <= 0 || a.port > 65535 || a.layoutFile.empty())
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

        long long ticksRun = 0;
        std::array<Replay::SlotState, Replay::kMaxSlots> held{};
        std::uint32_t slotCount = 0;

        Network::LockstepRuntimeOptions options;
        options.protocolId = args.protocolId;
        options.layout = *layout;
        options.peerName = args.peerName;
        options.scene = scene;
        options.seed = args.seed;
        options.reportPath = args.report;
        options.desyncLingerMicros = 3'000'000;
        const Network::InputLayout layoutCopy = *layout;
        std::uint32_t slotForSampling = 0;
        options.sampleInput = [&slotForSampling, &layoutCopy](const std::uint32_t tick)
        {
            // A player holding a direction for a few ticks at a time, and tapping a button
            std::vector<float> values(layoutCopy.ValueCount(), 0.0f);
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                values[i] = static_cast<float>(((tick / 9) * 7 + slotForSampling * 5 + i * 3) % 21) / 10.0f - 1.0f;
            }
            return values;
        };
        options.applyTick = [&](const N2Net::TickInputs &inputs, const std::uint32_t slots)
        {
            slotCount = slots;
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
            ++ticksRun;
            return true;
        };
        options.buildTree = [&](const std::uint32_t tick)
        {
            std::uint64_t hash = simulation->Hash();
            if (args.divergeAtTick >= 0 && static_cast<long long>(tick) >= args.divergeAtTick)
            {
                hash ^= 1ull << 17; // the injected divergence: one bit of the reported scene state
            }
            Say("hash tick=" + std::to_string(tick) + " " + Replay::FormatHash(hash));
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
        options.observer = [&](const Network::LockstepEvent &event)
        {
            using Type = Network::LockstepEvent::Type;
            switch (event.type)
            {
            case Type::Welcomed:
                Say("welcome " + event.text + " slot=" + std::to_string(event.slot));
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
            }
        };

        Network::LockstepRuntime runtime(*opened.transport, std::move(options));
        const auto start = std::chrono::steady_clock::now();
        auto last = start;
        bool done = false;
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
        if (const auto &stop = runtime.StopInfo())
        {
            if (stop->reason == Network::StopReason::Desync)
            {
                Say(std::string("divergence kind=") + Replay::DivergenceKindName(stop->divergence.kind) +
                    " object=" + std::to_string(stop->divergence.objectId) +
                    " field=" + std::to_string(stop->divergence.fieldId) + " rounds=" + std::to_string(stop->divergence.rounds));
            }
        }
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
        std::fprintf(stderr, "usage: n2server_lockstep_client --port <n> --layout-file <json> [--host ip] [--protocol-id n] "
                             "[--scene name] [--seed n] [--ticks n] [--diverge-at-tick t] [--stall-at-tick t] [--report path] "
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
