// n2server_net_client: the test client of tests/smoke/n2server_net.py: a headless NetworkClient. It starts the engine
// without a window, renderer or audio, loads the project's scene (the same code N2Server loads it with), joins a running
// server through the secure handshake and the world protocol, and reports what happens, one line each on stdout:
//
//   joined client=<id> objects=<n> spawned=<k>     the world matched the server's
//   spawn id=<n> prefab=<name> owner=<c>           the server spawned an object
//   despawn id=<n>                                 the server destroyed one this client had
//   refused reason=<reason>                        the fingerprint differs (the error text is on stderr)
//   closed <text> / failed <text>                  the server ended the connection / broke the protocol
//   objects count=<n> ids=<a,b,...>                the answer to "list"
//   position id=<n> x=<f> y=<f> z=<f>              the answer to "where <id>": the object's local position (nan when this
//                                                  client has no such object), as the server's NetworkTransform set it
//   left                                           the client left after "quit"
//
//   n2server_net_client --project <folder> --port <n> [--scene <res://...>] [--host <ip>] [--protocol-id <n>]
//                       [--expect-refusal] [--no-crate] [--max-ms <n>]
//
// The join token is the first line of stdin (never a command line argument). Later lines are commands: "list" and
// "quit" and "where <id>". --no-crate leaves out the prefab the server registers (the registries then differ: a refusal). Exit code 0
// when the outcome is the expected one (joined, or with --expect-refusal refused), else 1.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "NetTestCommon.hpp"
#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/PrefabTable.hpp"
#include "engine/network/n2net/SecureTransport.hpp"
#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/security/Crypto.hpp"
#include "n2server/SceneLoading.hpp"
#include "n2server/ServerOptions.hpp"

using namespace N2Engine;

namespace
{
    // Static: the reader thread may still be blocked in getline when main returns
    NetTest::CommandQueue g_commands;

    struct ClientArguments
    {
        std::string project;
        std::string scene;
        std::string host = "127.0.0.1";
        int port = 0;
        std::uint32_t protocolId = Server::ServerOptions::DefaultProtocolId;
        bool expectRefusal = false;
        bool registerCrate = true;
        int maxMs = 60000;
    };

    void Say(const std::string &line)
    {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    int Usage()
    {
        std::fprintf(stderr, "usage: n2server_net_client --project <folder> --port <n> [--scene <res://...>] [--host <ip>] "
                             "[--protocol-id <n>] [--expect-refusal] [--no-crate] [--max-ms <n>]   (the token is the first "
                             "line of stdin)\n");
        return 2;
    }

    std::optional<ClientArguments> ParseArguments(int argc, char *argv[])
    {
        ClientArguments arguments;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--expect-refusal")
            {
                arguments.expectRefusal = true;
                continue;
            }
            if (arg == "--no-crate")
            {
                arguments.registerCrate = false;
                continue;
            }
            if (i + 1 >= argc)
            {
                return std::nullopt;
            }
            const std::string value = argv[++i];
            if (arg == "--project")
            {
                arguments.project = value;
            }
            else if (arg == "--scene")
            {
                arguments.scene = value;
            }
            else if (arg == "--host")
            {
                arguments.host = value;
            }
            else if (arg == "--port")
            {
                arguments.port = std::atoi(value.c_str());
            }
            else if (arg == "--max-ms")
            {
                arguments.maxMs = std::atoi(value.c_str());
            }
            else if (arg == "--protocol-id")
            {
                const auto id = Server::ParseProtocolId(value);
                if (!id)
                {
                    return std::nullopt;
                }
                arguments.protocolId = *id;
            }
            else
            {
                return std::nullopt;
            }
        }
        if (arguments.project.empty() || arguments.port <= 0 || arguments.port > 65535)
        {
            return std::nullopt;
        }
        return arguments;
    }

    /// The answer to "where <id>"
    std::string PositionLine(const Network::NetworkWorld &world, const std::uint32_t id)
    {
        float x = std::nanf("");
        float y = x;
        float z = x;
        if (const Network::NetworkObject *const object = world.Find(Network::NetworkId{id}))
        {
            const GameObject &gameObject = object->GetGameObject();
            if (const Positionable *const positionable = gameObject.GetPositionable())
            {
                const Math::Vector3 position = positionable->GetLocalPosition();
                x = position.x;
                y = position.y;
                z = position.z;
            }
            else
            {
                x = 0.0f;
                y = 0.0f;
                z = 0.0f;
            }
        }
        char text[160];
        std::snprintf(text, sizeof text, "position id=%u x=%.4f y=%.4f z=%.4f", static_cast<unsigned>(id),
                      static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
        return text;
    }

    std::string IdList(const Network::NetworkWorld &world)
    {
        std::string list;
        for (const auto &entry : world.Objects())
        {
            list += (list.empty() ? "" : ",") + std::to_string(entry.first);
        }
        return list;
    }

    /// The engine up headless and the scene loaded, as N2Server does it; nullopt (a line on stderr) when it can't be
    struct Started
    {
        std::string scenePath;
        std::uint64_t sceneHash = 0;
    };

    std::optional<Started> StartEngineAndLoadScene(const ClientArguments &arguments)
    {
        std::error_code error;
        const std::filesystem::path projectDir = std::filesystem::canonical(arguments.project, error);
        if (error || !std::filesystem::is_directory(projectDir, error))
        {
            std::fprintf(stderr, "project folder not found: %s\n", arguments.project.c_str());
            return std::nullopt;
        }
        auto project = IO::ProjectFile::Load(projectDir);
        if (!project)
        {
            std::fprintf(stderr, "%s\n", project.error().c_str());
            return std::nullopt;
        }
        const std::string scenePath = arguments.scene.empty() ? project->startupScene : arguments.scene;

        Application &app = Application::GetInstance();
        (void)app.Init({
            .projectPath = projectDir.string(),
            .physicsBackend = Config::ApplicationOptions::PhysicsBackend::AUTO,
            .renderBackend = Config::ApplicationOptions::RenderBackend::NONE,
            .isHeadless = true,
            .audio = false,
            .physicsGpu = false,
        });
        IO::ResourceUUID::Initialize(project->projectId);
        IO::ResourceLoader::Instance().Initialize(projectDir, project->UserDataPath());
        (void)ApplyProjectSettings(project->settings);

        const auto loaded = Server::LoadSceneForRun(projectDir, scenePath);
        if (!loaded)
        {
            std::fprintf(stderr, "can't load the scene: %s\n", loaded.error().c_str());
            return std::nullopt;
        }
        return Started{loaded->resourcePath, loaded->contentHash};
    }

    /// Everything after the engine started; the locals (the world, the transport) are gone when it returns
    int RunClient(const ClientArguments &arguments, const Started &started, std::string &token)
    {
        if (arguments.registerCrate && !NetTest::RegisterCratePrefab())
        {
            std::fprintf(stderr, "the test prefab could not be registered\n");
            return 1;
        }

        Network::NetworkWorld world;
        const auto matched = world.BindScene(SceneManager::GetCurSceneRef());
        if (!matched)
        {
            std::fprintf(stderr, "the scene's network objects can't be matched: %s\n", matched.error().c_str());
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
        secureOptions.protocolId = arguments.protocolId;
        secureOptions.joinToken = token;
        N2Net::SecureUdpOpenResult opened = N2Net::SecureTransport::OpenUdp(udp, std::move(secureOptions));
        Security::SecureZero(token.data(), token.size());
        if (opened.transport == nullptr)
        {
            std::fprintf(stderr, "can't open the client transport: %s\n", opened.message.c_str());
            return 1;
        }
        const N2Net::UdpConnectResult peer = opened.udp->Connect(arguments.host, static_cast<std::uint16_t>(arguments.port));
        if (peer.error != N2Net::UdpError::None || !opened.transport->Connect(peer.peer))
        {
            std::fprintf(stderr, "can't start the handshake with %s:%d: %s\n", arguments.host.c_str(), arguments.port,
                         peer.message.c_str());
            return 1;
        }

        Network::NetworkClientOptions options;
        options.protocolId = arguments.protocolId;
        options.scenePath = started.scenePath;
        options.sceneHash = started.sceneHash;
        options.prefabs = Network::PrefabTable::Capture();
        Network::NetworkClient client(*opened.transport, world, std::move(options));

        bool everJoined = false;
        client.SetEventObserver(
            [&](const Network::NetworkSessionEvent &event)
            {
                using Type = Network::NetworkSessionEvent::Type;
                switch (event.type)
                {
                case Type::Joined:
                    everJoined = true;
                    Say("joined client=" + std::to_string(client.LocalClientId()) +
                        " objects=" + std::to_string(world.Count()) +
                        " spawned=" + std::to_string(client.Stats().spawnsApplied));
                    break;
                case Type::Spawned:
                    Say("spawn id=" + std::to_string(event.id.value) + " prefab=" + event.prefab +
                        " owner=" + std::to_string(event.owner));
                    break;
                case Type::Despawned:
                    Say("despawn id=" + std::to_string(event.id.value));
                    break;
                case Type::Refused:
                    Say("refused reason=" + std::string(Network::ToString(client.Refusal())));
                    std::fprintf(stderr, "%s\n", event.text.c_str());
                    break;
                case Type::Closed:
                    Say("closed " + event.text);
                    break;
                case Type::Failed:
                    Say("failed " + event.text);
                    break;
                }
            });

        const auto start = std::chrono::steady_clock::now();
        const auto elapsed = [&start]
        { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start); };
        bool leaving = false;
        bool timedOut = false;
        while (!client.IsFinished())
        {
            if (opened.transport->ClientState() == N2Net::SecureClientState::Failed)
            {
                Say("handshake-failed");
                return 1;
            }
            if (elapsed() > std::chrono::milliseconds(arguments.maxMs))
            {
                timedOut = true;
                break;
            }
            client.Update(static_cast<std::uint64_t>(elapsed().count()));

            const bool closed = g_commands.Closed();
            std::string line;
            while (g_commands.Pop(line))
            {
                if (line == "list")
                {
                    Say("objects count=" + std::to_string(world.Count()) + " ids=" + IdList(world));
                }
                else if (line.rfind("where ", 0) == 0)
                {
                    const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(line.c_str() + 6, nullptr, 10));
                    Say(PositionLine(world, id));
                }
                else if (line == "quit")
                {
                    leaving = true;
                }
            }
            if ((leaving || closed) && !client.IsFinished() && client.IsJoined())
            {
                client.Leave();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (timedOut)
        {
            Say("timeout state=" + std::string(Network::ToString(client.State())));
            return 1;
        }
        if (leaving)
        {
            Say("left");
        }

        if (arguments.expectRefusal)
        {
            return client.State() == Network::NetworkClientState::Refused ? 0 : 1;
        }
        return everJoined && client.State() != Network::NetworkClientState::Failed ? 0 : 1;
    }
}

int main(int argc, char *argv[])
{
    const std::optional<ClientArguments> arguments = ParseArguments(argc, argv);
    if (!arguments)
    {
        return Usage();
    }

    std::string token;
    std::getline(std::cin, token);
    token = NetTest::StripLineEnd(std::move(token));
    g_commands.StartReadingStdin();

    const std::optional<Started> started = StartEngineAndLoadScene(*arguments);
    int exitCode = 1;
    if (started)
    {
        exitCode = RunClient(*arguments, *started, token);
    }
    Application::GetInstance().Shutdown();
    // The thread that reads stdin is blocked in a read that holds stdin's lock, and the C library's exit takes every
    // stream's lock: returning from main would hang. The engine is shut down and the output flushed, so end here.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(exitCode);
}
