#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <future>
#include <system_error>
#include <utility>

#include "engine/Application.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/Version.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaScriptTemplate.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

#include "renderer/common/FrameRows.hpp"
#include "renderer/common/Renderer.hpp"

#include "editor-server/EditorServer.hpp"
#include "editor-server/Protocol.hpp"
#include "editor-server/Commands.hpp"
#include "editor-server/Serialization.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
using NativeSocket = SOCKET;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#define CLOSE_SOCKET close
#define INVALID_SOCKET -1
#define SOCKET_ERROR_CODE errno
using NativeSocket = int;
#endif

namespace N2Engine::Editor
{
    using namespace Protocol;

    namespace
    {
        // Sockets are kept as int (as before); an invalid SOCKET converts to -1
        constexpr int NoSocket = -1;
        // How often a waiting network thread checks whether the server is stopping
        constexpr long PollIntervalMicroseconds = 100 * 1000;

        /// A path as UTF-8 text for the wire (string() would throw for characters the code page lacks)
        std::string Utf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        /// A UTF-8 path from the wire as a filesystem path
        std::filesystem::path FromUtf8(const std::string &text)
        {
            return std::filesystem::path(std::u8string(text.begin(), text.end()));
        }

        std::vector<std::string> PathStrings(const std::vector<IO::ResourcePath> &paths)
        {
            std::vector<std::string> strings;
            strings.reserve(paths.size());
            for (const IO::ResourcePath &path : paths)
            {
                strings.push_back(path.ToString());
            }
            return strings;
        }

        std::expected<std::string, std::string> ReadTextFile(const std::filesystem::path &file)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return std::unexpected("can't read " + Utf8(file));
            }
            std::ostringstream text;
            text << stream.rdbuf();
            if (stream.bad())
            {
                return std::unexpected("can't read " + Utf8(file));
            }
            return std::move(text).str();
        }

        /// A scene's JSON as its file holds it: indented by 2, ending in a newline (diffs well under version control)
        std::string SceneFileText(const nlohmann::json &scene)
        {
            return scene.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        }

        // ==================== Hierarchy and entities ====================

        nlohmann::json Vec3Json(const Math::Vector3 &v)
        {
            return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
        }

        nlohmann::json QuatJson(const Math::Quaternion &q)
        {
            return nlohmann::json{{"x", q.GetX()}, {"y", q.GetY()}, {"z", q.GetZ()}, {"w", q.GetW()}};
        }

        /// The object with this UUID string in the scene, or nullptr (also for text that isn't a UUID)
        std::shared_ptr<GameObject> FindEntity(Scene &scene, const std::string &entityId)
        {
            if (const auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
            {
                return scene.FindGameObjectByUUID(uuid.value());
            }
            return nullptr;
        }

        std::string NotFoundMessage(const char *what, const std::string &entityId)
        {
            return std::format("{} not found: {}", what, EditorServer::SanitizeForLog(entityId));
        }

        /// The UUID string of an object's parent, or empty for a root
        std::string ParentIdOf(const GameObject &gameObject)
        {
            const auto parent = gameObject.GetParent();
            return parent ? parent->GetUUID().ToString() : std::string{};
        }

        /// The fields GetHierarchy and GetEntity share (the EntityHeader of protocol.json, plus what a node adds)
        nlohmann::json HeaderJson(const GameObject &gameObject, const std::string &parentId, const size_t index)
        {
            return nlohmann::json{
                {"id", gameObject.GetUUID().ToString()},
                {"parentId", parentId},
                {"index", index},
                {"name", gameObject.GetName()},
                {"active", gameObject.IsActive()},
                {"activeInHierarchy", gameObject.IsActiveInHierarchy()},
                {"layer", gameObject.GetLayer()},
                {"tag", gameObject.GetTag()},
            };
        }

        /// Appends the object and, depth first, everything under it: a HierarchyNode each
        void AppendHierarchy(const GameObject &gameObject, const std::string &parentId, const size_t index,
                             nlohmann::json &nodes)
        {
            nlohmann::json node = HeaderJson(gameObject, parentId, index);
            nlohmann::json components = nlohmann::json::array();
            for (const auto &component : gameObject.GetAllComponents())
            {
                components.push_back(component->GetTypeName());
            }
            node["components"] = std::move(components);
            nodes.push_back(std::move(node));

            const std::string id = gameObject.GetUUID().ToString();
            size_t childIndex = 0;
            for (const auto &child : gameObject.GetChildren())
            {
                if (child && !child->IsDestroyed())
                {
                    AppendHierarchy(*child, id, childIndex, nodes);
                }
                ++childIndex;
            }
        }

        /// The EntityDetails of protocol.json: header, local transform (when the object has one) and the components
        /// as they are saved
        nlohmann::json EntityDetailsJson(const GameObject &gameObject)
        {
            nlohmann::json entity;
            entity["header"] = HeaderJson(gameObject, ParentIdOf(gameObject), gameObject.GetSiblingIndex());
            if (const Positionable *positionable = gameObject.GetPositionable())
            {
                entity["transform"] = nlohmann::json{
                    {"position", Vec3Json(positionable->GetLocalPosition())},
                    {"rotation", QuatJson(positionable->GetLocalRotation())},
                    {"scale", Vec3Json(positionable->GetLocalScale())},
                };
            }
            nlohmann::json components = nlohmann::json::array();
            for (const auto &component : gameObject.GetAllComponents())
            {
                components.push_back(nlohmann::json{
                    {"type", component->GetTypeName()},
                    {"uuid", component->GetUUID().ToString()},
                    {"values", component->Serialize()},
                });
            }
            entity["components"] = std::move(components);
            return entity;
        }

        /// The UUID strings of the object and everything under it
        std::vector<std::string> SubtreeIds(const GameObject &gameObject)
        {
            std::vector<std::string> ids{gameObject.GetUUID().ToString()};
            for (const auto &descendant : gameObject.GetChildrenRecursive())
            {
                ids.push_back(descendant->GetUUID().ToString());
            }
            return ids;
        }

        /// What CreateEntityEx's preset adds to the new object (which always gets a transform)
        struct EntityPreset
        {
            std::string_view name;
            /// The new object's name when the request gives none
            std::string_view objectName;
            void (*addComponents)(GameObject &);
        };

        const std::vector<EntityPreset> &EntityPresets()
        {
            static const std::vector<EntityPreset> presets = {
                {"", "GameObject", [](GameObject &) {}},
                {"Empty", "GameObject", [](GameObject &) {}},
                {"Cube", "Cube", [](GameObject &object) { object.AddComponent<Example::CubeRenderer>(); }},
                {"Sphere", "Sphere", [](GameObject &object) { object.AddComponent<Example::SphereRenderer>(); }},
                {"Quad", "Quad", [](GameObject &object) { object.AddComponent<Example::QuadRenderer>(); }},
                {"Light", "Light", [](GameObject &object) { object.AddComponent<Rendering::Light>(); }},
                {"DirectionalLight", "Directional Light", [](GameObject &object)
                {
                    object.AddComponent<Rendering::Light>()->type = Rendering::LightType::Directional;
                }},
                {"PointLight", "Point Light", [](GameObject &object)
                {
                    object.AddComponent<Rendering::Light>()->type = Rendering::LightType::Point;
                }},
                {"SpotLight", "Spot Light", [](GameObject &object)
                {
                    object.AddComponent<Rendering::Light>()->type = Rendering::LightType::Spot;
                }},
            };
            return presets;
        }

        /// The name a duplicate of `source` gets among `siblings`, as Unity names it: "Cube" and "Cube (3)" give
        /// "Cube (1)", then "Cube (2)", ... the first of those no sibling has
        std::string DuplicateName(const GameObject &source, const std::vector<std::shared_ptr<GameObject>> &siblings)
        {
            std::string base = source.GetName();
            // "Cube (3)" is a duplicate of "Cube", not of itself
            if (base.size() > 4 && base.back() == ')')
            {
                const size_t open = base.rfind(" (");
                if (open != std::string::npos && base.size() - open >= 4)
                {
                    const std::string_view digits = std::string_view(base).substr(open + 2, base.size() - open - 3);
                    if (!digits.empty() && std::ranges::all_of(digits, [](const char c) { return c >= '0' && c <= '9'; }))
                    {
                        base.erase(open);
                    }
                }
            }
            for (int number = 1;; ++number)
            {
                std::string candidate = std::format("{} ({})", base, number);
                const bool taken = std::ranges::any_of(siblings, [&candidate](const std::shared_ptr<GameObject> &sibling)
                {
                    return sibling && sibling->GetName() == candidate;
                });
                if (!taken)
                {
                    return candidate;
                }
            }
        }

        /// A resolver that knows every object and component of the scene, by UUID
        ReferenceResolver SceneReferences(const Scene &scene)
        {
            ReferenceResolver references;
            scene.TraverseAll([&references](const std::shared_ptr<GameObject> &gameObject)
            {
                references.RegisterGameObject(gameObject->GetUUID(), gameObject.get());
                for (const auto &component : gameObject->GetAllComponents())
                {
                    references.RegisterComponent(component->GetUUID(), component.get());
                }
            });
            return references;
        }

        /// SetEntityProperties' properties, checked
        struct EntityProperties
        {
            std::optional<std::string> name;
            std::optional<bool> active;
            std::optional<std::string> tag;
            std::optional<int> layer;
        };

        std::expected<EntityProperties, std::string> ParseEntityProperties(const nlohmann::json &properties)
        {
            if (!properties.is_object())
            {
                return std::unexpected("properties must be a JSON object with any of name, active, tag and layer");
            }
            EntityProperties parsed;
            for (const auto &[key, value] : properties.items())
            {
                if (key == "name" || key == "tag")
                {
                    if (!value.is_string())
                    {
                        return std::unexpected(std::format("{} must be a string", key));
                    }
                    (key == "name" ? parsed.name : parsed.tag) = value.get<std::string>();
                }
                else if (key == "active")
                {
                    if (!value.is_boolean())
                    {
                        return std::unexpected("active must be true or false");
                    }
                    parsed.active = value.get<bool>();
                }
                else if (key == "layer")
                {
                    // Not clamped: a client that means layer 40 has a bug, and finds out
                    if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() >= Layers::Count)
                    {
                        return std::unexpected(std::format("layer must be an integer from 0 to {}", Layers::Count - 1));
                    }
                    parsed.layer = static_cast<int>(value.get<int64_t>());
                }
                else
                {
                    return std::unexpected(std::format("Unknown property '{}' (the properties are name, active, tag and "
                                                       "layer)", EditorServer::SanitizeForLog(key)));
                }
            }
            return parsed;
        }
    }

    EditorServer::EditorServer()
    {
        // Every line logged while the server exists becomes a log event. The subscriber runs on the thread that logged,
        // holding the Logger's lock, so it only copies the line into the ring (under the ring's own mutex) and never logs
        // or waits on another thread (see Logger.hpp)
        _logSubscription = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
        {
            try
            {
                _events.PushLog(level, message, EventRing::NowUnixMilliseconds());
            }
            catch (...)
            {
                // (bad_alloc) Logging must never fail because the editor couldn't keep a copy of the line
            }
        };
    }

    EditorServer::~EditorServer()
    {
        Stop();
        // Once this returns no subscriber call is running (dispatch holds the Logger's lock), so _events can go
        Logger::logEvent -= _logSubscription;
    }

    bool EditorServer::Start(int port, const std::string &bindAddress)
    {
        if (_running) return true;
        // A client Shutdown stops serving without Stop(): join that thread before starting another
        if (_serverThread.joinable()) Stop();

#ifdef _WIN32
        if (!_socketsInitialized)
        {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
            {
                Logger::Error("WSAStartup failed");
                return false;
            }
        }
#endif
        _socketsInitialized = true;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1)
        {
            Logger::Error("Invalid bind address (expected an IPv4 address): " + bindAddress);
            return false;
        }

        _listenSocket = static_cast<int>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (_listenSocket == INVALID_SOCKET)
        {
            Logger::Error("Failed to create socket");
            _listenSocket = -1;
            return false;
        }

#ifndef _WIN32
        // Lets a restarted host rebind while the last connection is in TIME_WAIT. Not on Windows, where
        // SO_REUSEADDR allows hijacking the port, and the default already rebinds over TIME_WAIT.
        int opt = 1;
        setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

        if (bind(_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            Logger::Error("Failed to bind to " + bindAddress + ":" + std::to_string(port));
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
            return false;
        }

        if (listen(_listenSocket, 1) < 0)
        {
            Logger::Error("Failed to listen");
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
            return false;
        }

        // The actual port, in case port 0 asked the OS to pick one
        sockaddr_in boundAddr{};
        socklen_t boundAddrSize = sizeof(boundAddr);
        _port = getsockname(_listenSocket, reinterpret_cast<sockaddr*>(&boundAddr), &boundAddrSize) == 0
                    ? ntohs(boundAddr.sin_port)
                    : port;

        _commands.Reopen();
        _running = true;
        _serverThread = std::thread(&EditorServer::ServerLoop, this, _listenSocket);
        Logger::Info("Editor server listening on " + bindAddress + ":" + std::to_string(_port));
        return true;
    }

    void EditorServer::Stop()
    {
        // The network thread never blocks on a socket for long (see WaitUntilReady), so clearing _running
        // stops it within one poll interval. Closing the queue wakes it if it is waiting for a command's
        // result. Sockets are only closed once it has exited: closing or shutting down a socket another
        // thread is blocked on doesn't reliably wake it (shutdown doesn't on Windows, close doesn't on Linux).
        _running = false;
        _commands.Close();

        if (_serverThread.joinable())
            _serverThread.join();

        if (_listenSocket != -1)
        {
            CLOSE_SOCKET(_listenSocket);
            _listenSocket = -1;
        }
        _port = 0;

#ifdef _WIN32
        if (_socketsInitialized)
        {
            WSACleanup();
        }
#endif
        _socketsInitialized = false;
    }

    size_t EditorServer::ProcessCommands(std::chrono::milliseconds maxWait)
    {
        return _commands.Drain(maxWait);
    }

    void EditorServer::UpdateAudio()
    {
        auto &audio = Audio::AudioSystem::Instance();
        const auto now = std::chrono::steady_clock::now();
        if (_lastAudioUpdate && audio.IsLoopback())
        {
            // AdvanceStream caps one step (a stalled main thread doesn't mix minutes at once)
            audio.AdvanceStream(std::chrono::duration<double>(now - *_lastAudioUpdate).count());
            audio.Update();
        }
        _lastAudioUpdate = now;
    }

    void EditorServer::PostLog(std::string message, bool isWarning)
    {
        // Fire and forget: the future is dropped, and the line is lost if the queue is already closed
        (void)_commands.Enqueue([message = std::move(message), isWarning]
        {
            if (isWarning)
                Logger::Warn(message);
            else
                Logger::Info(message);
            return CommandQueue::Response{};
        });
    }

    bool EditorServer::SetAccessToken(std::string token)
    {
        if (_running)
        {
            Logger::Warn("The editor server's access token can't change while it is running");
            return false;
        }
        _accessToken = std::move(token);
        return true;
    }

    bool EditorServer::SetHelloTimeout(std::chrono::milliseconds timeout)
    {
        if (_running || timeout <= std::chrono::milliseconds::zero())
        {
            Logger::Warn("The editor server's Hello timeout can't change while it is running, or be zero or negative");
            return false;
        }
        _helloTimeout = timeout;
        return true;
    }

    bool EditorServer::SetStopOnDisconnect(const bool stop)
    {
        if (_running)
        {
            Logger::Warn("Whether the editor server stops on disconnect can't change while it is running");
            return false;
        }
        _stopOnDisconnect = stop;
        return true;
    }

    std::string EditorServer::SanitizeForLog(std::string_view text, size_t maxBytes)
    {
        bool truncated = false;
        if (text.size() > maxBytes)
        {
            // Back up over UTF-8 continuation bytes (10xxxxxx), so the cut never splits a character
            size_t cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                --cut;
            text = text.substr(0, cut);
            truncated = true;
        }

        std::string result;
        result.reserve(text.size() + 3);
        for (const char c : text)
        {
            const auto byte = static_cast<unsigned char>(c);
            result += byte < 0x20 || byte == 0x7F ? '?' : c;
        }
        if (truncated)
            result += "...";
        return result;
    }

    std::string_view EditorServer::EngineVersion()
    {
        // The engine's own (CMake's project version), the one project files record
        return N2Engine::EngineVersion();
    }

    std::vector<std::string> EditorServer::Capabilities()
    {
        return {};
    }

    bool EditorServer::TokensMatch(std::string_view accessToken, std::string_view token)
    {
        // Every byte of the access token is compared whatever the token holds; only the length is learnt early
        unsigned int difference = accessToken.size() == token.size() ? 0u : 1u;
        for (size_t i = 0; i < accessToken.size(); ++i)
        {
            const unsigned char given = i < token.size() ? static_cast<unsigned char>(token[i]) : 0;
            difference |= static_cast<unsigned char>(accessToken[i]) ^ given;
        }
        return difference == 0;
    }

    bool EditorServer::IsAllowedBeforeHello(uint8_t commandType)
    {
        return static_cast<CommandType>(commandType) == CommandType::Hello;
    }

    void EditorServer::ServerLoop(int listenSocket)
    {
        while (_running)
        {
            PostLog("Waiting for editor connection...");

            int clientSocket = NoSocket;
            if (WaitUntilReady(listenSocket, false))
            {
                clientSocket = static_cast<int>(accept(listenSocket, nullptr, nullptr));
            }
            if (clientSocket == NoSocket)
            {
                if (_running)
                {
                    PostLog("Accept failed: " + std::to_string(SOCKET_ERROR_CODE), true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // don't spin on a broken listener
                }
                continue;
            }

            PostLog("Editor connected");
            bool sessionOpened = false;
            try
            {
                HandleClient(clientSocket, sessionOpened);
            }
            catch (...)
            {
                // e.g. bad_alloc; nothing may escape this thread (that would terminate the host)
                PostLog("Dropping the editor connection after an unexpected error", true);
            }

            CLOSE_SOCKET(clientSocket);
            PostLog("Editor disconnected");

            // A Shutdown (or Stop) has already cleared _running; this is the client going away on its own
            if (_stopOnDisconnect && sessionOpened && _running)
            {
                PostLog("Stopping the editor server: the client's session ended, and it stops on disconnect");
                _running = false;
            }
        }
    }

    void EditorServer::HandleClient(int clientSocket, bool &sessionOpened)
    {
        // This connection's session: whether its last Hello succeeded. A new connection starts without one, so a
        // client that reconnects must send Hello again.
        bool helloAccepted = false;
        // With an access token, a connection that hasn't succeeded a Hello must not hold the (one-at-a-time) server:
        // it has until this deadline to do so, and is closed after a refused command or a failed Hello
        const bool gated = !_accessToken.empty();
        // Without a token every connection is a session (for SetStopOnDisconnect); with one, from its first good Hello
        sessionOpened = !gated;
        const auto helloDeadline = std::chrono::steady_clock::now() + _helloTimeout;
        const auto deadline = [&]() -> std::optional<std::chrono::steady_clock::time_point>
        {
            if (gated && !helloAccepted)
                return helloDeadline;
            return std::nullopt;
        };
        const auto closeUnauthorized = [this](const std::string &reason)
        {
            PostLog("Closing an editor connection that hasn't sent a successful Hello: " + reason, true);
        };
        const auto logIfHelloTimedOut = [&]
        {
            if (deadline().has_value() && _running && std::chrono::steady_clock::now() >= helloDeadline)
                closeUnauthorized(std::format("no Hello within {} ms", _helloTimeout.count()));
        };

        while (_running)
        {
            // Read command header: [type: 1 byte][length: 4 bytes]
            uint8_t cmdType;
            uint32_t payloadLength;
            if (!Receive(clientSocket, &cmdType, 1, deadline()) ||
                !Receive(clientSocket, &payloadLength, sizeof(payloadLength), deadline()))
            {
                logIfHelloTimedOut();
                break;
            }

            // The length is client-supplied, so check it before allocating. The oversized payload is
            // still in the stream, so the connection can't be resynchronized and is closed. Before a Hello on a
            // gated connection, a payload bigger than any Hello needs is refused the same way.
            const bool beforeHello = deadline().has_value();
            const uint32_t limit = beforeHello ? MaxPayloadBytesBeforeHello : MaxPayloadBytes;
            if (payloadLength > limit)
            {
                const std::string message = std::format("Payload of {} bytes exceeds the {} byte limit{}",
                                                        payloadLength, limit, beforeHello ? " before Hello" : "");
                BufferWriter response;
                WriteError(response, message);
                Send(clientSocket, response.Data().data(), response.Size());
                PostLog(message, true);
                break;
            }

            // Read payload
            std::vector<uint8_t> payload(payloadLength);
            if (payloadLength > 0 && !Receive(clientSocket, payload.data(), payloadLength, deadline()))
            {
                logIfHelloTimedOut();
                break;
            }

            // With an access token, nothing but Hello runs (or is even queued) until a Hello has succeeded, and
            // anything else ends the connection once its Error is sent
            if (gated && !helloAccepted && !IsAllowedBeforeHello(cmdType))
            {
                BufferWriter response;
                WriteError(response, HelloRequiredError);
                Send(clientSocket, response.Data().data(), response.Size());
                closeUnauthorized(std::format("it sent command 0x{:X}", cmdType));
                break;
            }

            if (static_cast<CommandType>(cmdType) == CommandType::Shutdown)
            {
                BufferWriter response;
                WriteOk(response);
                Send(clientSocket, response.Data().data(), response.Size());
                // Stop serving, so the host's main loop sees !IsRunning() and exits cleanly
                PostLog("Shutdown requested by editor");
                _running = false;
                break;
            }

            // Everything else touches engine state, so it runs on the main thread (see ProcessCommands)
            std::future<CommandQueue::Response> pending = _commands.Enqueue(
                [this, cmdType, payload = std::move(payload)] { return ExecuteCommand(cmdType, payload); });

            CommandQueue::Response response;
            try
            {
                response = pending.get();
            }
            catch (const std::future_error &)
            {
                break; // the queue was closed: the server is stopping
            }

            // The session follows the connection's last Hello: a refused one (wrong token or version) ends it
            const bool isHello = static_cast<CommandType>(cmdType) == CommandType::Hello;
            if (isHello)
            {
                helloAccepted = !response.empty() && response[0] == static_cast<uint8_t>(ResponseType::ServerInfo);
                sessionOpened = sessionOpened || helloAccepted;
            }

            if (!Send(clientSocket, response.data(), response.size()))
                break;

            // With an access token, a refused Hello also ends the connection, once its Error is sent
            if (gated && isHello && !helloAccepted)
            {
                closeUnauthorized("its Hello was refused");
                break;
            }
        }
    }

    std::vector<uint8_t> EditorServer::ExecuteCommand(uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        _response.clear();

        bool failed = false;
        std::string failure;
        try
        {
            // No socket: handlers only build the response, which SendResponse records in _response
            ProcessCommand(-1, commandType, payload);
        }
        catch (const std::exception &e)
        {
            failed = true;
            failure = e.what();
        }
        catch (...)
        {
            failed = true;
            failure = "unknown exception";
        }

        if (failed || _response.empty())
        {
            _response.clear();
            if (!failed)
            {
                failure = "the command produced no response";
            }
            Logger::Error(std::format("Command 0x{:X} failed: {}", commandType, failure));

            BufferWriter response;
            WriteError(response, "Command failed: " + failure);
            return {response.Data().begin(), response.Data().end()};
        }

        return std::exchange(_response, {});
    }

    bool EditorServer::IsViewportSizeValid(int32_t width, int32_t height)
    {
        return width > 0 && height > 0 && width <= MaxViewportDimension && height <= MaxViewportDimension;
    }

    std::string EditorServer::RenderTargetError(const int width, const int height)
    {
        return std::format("Couldn't create a {}x{} render target", width, height);
    }

    void EditorServer::ReadFrame(const Renderer::Common::IRenderer &renderer, const int width, const int height,
                                 std::vector<uint8_t> &pixels)
    {
        const size_t rowBytes = width > 0 ? static_cast<size_t>(width) * 4 : 0;
        const size_t rows = height > 0 ? static_cast<size_t>(height) : 0;
        // resize, not assign: every live backend writes the whole frame, so zeroing ~3.7 MB per 1280x720 frame
        // would be wasted. New bytes are zero, so a backend that writes nothing (Vulkan's stub) gives black.
        pixels.resize(rowBytes * rows);
        if (pixels.empty())
        {
            return;
        }

        // Every backend reads back RGBA, bottom row first (IRenderer::ReadFramebuffer); FrameData is top row first
        renderer.ReadFramebuffer(pixels.data(), width, height);
        Renderer::Common::FlipRows(pixels.data(), rowBytes, rows);

        // The viewport is opaque: alpha is whatever blending left in the target, which a client drawing the
        // pixels as an image would show as see-through
        for (size_t i = 3; i < pixels.size(); i += 4)
        {
            pixels[i] = 0xFF;
        }
    }

    std::optional<std::filesystem::path> EditorServer::ResolveSceneFile(const std::filesystem::path &scenesDirectory,
                                                                        const std::string &sceneName)
    {
        if (scenesDirectory.empty() || sceneName.empty() || sceneName.find('\0') != std::string::npos)
            return std::nullopt;

        std::filesystem::path relative(sceneName);
        // Rejects "C:\x", "/x", "\x" and "C:x" outright (only a relative path inside the directory is valid)
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
            return std::nullopt;
        // "dir/", "." and ".." name a directory, not a scene file
        const std::filesystem::path originalFileName = relative.filename();
        if (originalFileName.empty() || originalFileName == "." || originalFileName == "..")
            return std::nullopt;

        // Append the extension unless the name already ends with it (in any case), so "Level.1" works
        const std::string_view extension = SceneFileExtension;
        const bool hasExtension =
            sceneName.size() > extension.size() &&
            std::equal(extension.begin(), extension.end(), sceneName.end() - static_cast<std::ptrdiff_t>(extension.size()),
                       [](char a, char b)
                       {
                           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                       });
        if (!hasExtension)
            relative += SceneFileExtension;

        // The parent is canonicalized (symlinks and ".." resolved) and must be the directory or inside it.
        // The file name itself is not resolved, so a symlinked scene file is deleted as a link, never its target.
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(scenesDirectory, error);
        if (error)
            return std::nullopt;
        const std::filesystem::path parent = std::filesystem::weakly_canonical((root / relative).parent_path(), error);
        if (error)
            return std::nullopt;

        const std::filesystem::path fromRoot = parent.lexically_relative(root);
        if (fromRoot.empty() || *fromRoot.begin() == "..")
            return std::nullopt;

        return parent / relative.filename();
    }

    bool EditorServer::IsPolledCommand(uint8_t commandType)
    {
        switch (static_cast<CommandType>(commandType))
        {
        case CommandType::RenderFrame:        // every animation frame
        case CommandType::GetAudio:           // every ~25 ms while audio plays
        case CommandType::PollEvents:         // every ~100 ms; a line logged per poll would be an event every poll
        case CommandType::GetAllEntities:     // the hierarchy panel
        case CommandType::GetEntityTransform: // the inspector
        case CommandType::GetCameraPosition:  // the scene view
        case CommandType::GetEngineHealth:    // the status panel
        case CommandType::GetHierarchy:       // the hierarchy panel, whenever the scene revision moves
        case CommandType::GetEntity:          // the inspector, whenever the selected object changes
            return true;
        default:
            return false;
        }
    }

    void EditorServer::ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        // No generic "command issued" line: every log line is an event for the editor (PollEvents), and handlers log
        // what matters themselves. A polled command's handler never logs per call (see IsPolledCommand); failures are
        // logged by ExecuteCommand.
        auto cmd = static_cast<CommandType>(commandType);

        switch (cmd)
        {
        case CommandType::Hello:
            HandleHello(clientSocket, payload);
            break;
        case CommandType::RenderFrame:
            HandleRenderFrame(clientSocket);
            break;
        case CommandType::SetViewportSize:
            HandleSetViewportSize(clientSocket, payload);
            break;
        case CommandType::GetAudio:
            HandleGetAudio(clientSocket);
            break;
        case CommandType::PollEvents:
            HandlePollEvents(clientSocket, payload);
            break;
        case CommandType::SetCameraPosition:
            HandleSetCameraPosition(clientSocket, payload);
            break;
        case CommandType::GetCameraPosition:
            HandleGetCameraPosition(clientSocket);
            break;
        case CommandType::CreateScene:
            HandleCreateScene(clientSocket, payload);
            break;
        case CommandType::LoadScene:
            HandleLoadScene(clientSocket, payload);
            break;
        case CommandType::SaveScene:
            HandleSaveScene(clientSocket);
            break;
        case CommandType::DeleteScene:
            HandleDeleteScene(clientSocket, payload);
            break;
        case CommandType::GetCurrentScene:
            HandleGetCurrentScene(clientSocket);
            break;
        case CommandType::OpenScene:
            HandleOpenScene(clientSocket, payload);
            break;
        case CommandType::SaveSceneToFile:
            HandleSaveSceneToFile(clientSocket, payload);
            break;
        case CommandType::NewScene:
            HandleNewScene(clientSocket, payload);
            break;
        case CommandType::GetHierarchy:
            HandleGetHierarchy(clientSocket);
            break;
        case CommandType::GetOpenScene:
            HandleGetOpenScene(clientSocket);
            break;
        case CommandType::GetProjectInfo:
            HandleGetProjectInfo(clientSocket);
            break;
        case CommandType::SetProjectSettings:
            HandleSetProjectSettings(clientSocket, payload);
            break;
        case CommandType::SetStartupScene:
            HandleSetStartupScene(clientSocket, payload);
            break;
        case CommandType::CreateEntity:
            HandleCreateEntity(clientSocket, payload);
            break;
        case CommandType::DestroyEntity:
            HandleDestroyEntity(clientSocket, payload);
            break;
        case CommandType::SetEntityTransform:
            HandleSetEntityTransform(clientSocket, payload);
            break;
        case CommandType::GetEntityTransform:
            HandleGetEntityTransform(clientSocket, payload);
            break;
        case CommandType::GetAllEntities:
            HandleGetAllEntities(clientSocket);
            break;
        case CommandType::CreateEntityEx:
            HandleCreateEntityEx(clientSocket, payload);
            break;
        case CommandType::SetEntityParent:
            HandleSetEntityParent(clientSocket, payload);
            break;
        case CommandType::SetEntityProperties:
            HandleSetEntityProperties(clientSocket, payload);
            break;
        case CommandType::DuplicateEntity:
            HandleDuplicateEntity(clientSocket, payload);
            break;
        case CommandType::GetEntity:
            HandleGetEntity(clientSocket, payload);
            break;
        case CommandType::SetLocalTransform:
            HandleSetLocalTransform(clientSocket, payload);
            break;
        case CommandType::CreateScript:
            HandleCreateScript(clientSocket, payload);
            break;
        case CommandType::RescanAssets:
            HandleRescanAssets(clientSocket);
            break;
        case CommandType::GetEngineHealth:
            HandleGetEngineHealth(clientSocket);
            break;
        default:
            Logger::Warn("Unknown command: " + std::to_string(commandType));
            BufferWriter response;
            WriteError(response, "Unknown command");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            break;
        }
    }

    void EditorServer::HandleHello(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const HelloCmd cmd = HelloCmd::Deserialize(reader);

        // Client-supplied text only reaches the log, or the Error, sanitised and shortened; the token never does
        const std::string clientName = SanitizeForLog(cmd.clientName);
        const std::string clientVersionText = SanitizeForLog(cmd.protocolVersion, 40);
        BufferWriter response;

        // The token first, so a client without it learns nothing else
        if (!_accessToken.empty() && !TokensMatch(_accessToken, cmd.token))
        {
            Logger::Warn("Refused Hello from editor client '" + clientName + "': wrong access token");
            WriteError(response, "Invalid access token");
            SendResponse(clientSocket, response.Release());
            return;
        }

        const auto clientVersion = ParseProtocolVersion(cmd.protocolVersion);
        const auto serverVersion = ParseProtocolVersion(ProtocolVersion);
        if (!clientVersion.has_value() || !serverVersion.has_value() ||
            clientVersion->majorVersion != serverVersion->majorVersion)
        {
            const std::string message =
                clientVersion.has_value()
                    ? std::format("Protocol version mismatch: this server speaks {}, the client {} (the major versions "
                                  "must match)", ProtocolVersion, clientVersionText)
                    : std::format("Invalid protocol version '{}' (expected major.minor.patch; this server speaks {})",
                                  clientVersionText, ProtocolVersion);
            Logger::Warn("Refused Hello from editor client '" + clientName + "': " + message);
            WriteError(response, message);
            SendResponse(clientSocket, response.Release());
            return;
        }

        if (clientVersion->minorVersion != serverVersion->minorVersion)
        {
            // Compatible, but one side has commands the other doesn't: those fail with "Unknown command"
            Logger::Warn(std::format("Editor client '{}' said Hello with protocol {}, this server speaks {}: commands "
                                     "only one of them knows will fail", clientName, clientVersionText, ProtocolVersion));
        }
        else
        {
            Logger::Info(std::format("Editor client '{}' said Hello (protocol {})", clientName, clientVersionText));
        }
        // A project is loaded when the host opened one (--project, SetProject)
        WriteServerInfo(response, ProtocolVersion, EngineVersion(), Capabilities(), HasProject());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleRenderFrame(int clientSocket)
    {
        auto &app = Application::GetInstance();
        auto &window = app.GetWindow();

        auto *renderer = window.GetRenderer();
        if (!renderer)
        {
            BufferWriter response;
            WriteError(response, "No renderer available (see GetEngineHealth)");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // Render at the viewport size (validated by SetViewportSize). SetViewportSize already applied it; this
        // covers the default size, before any SetViewportSize, does nothing when the size is unchanged, and
        // retries a size the renderer failed to make before. A frame of another size is never sent.
        if (!window.SetRenderSize(_viewportWidth, _viewportHeight))
        {
            BufferWriter response;
            WriteError(response, RenderTargetError(_viewportWidth, _viewportHeight));
            SendResponse(clientSocket, response.Release());
            return;
        }
        app.RenderEditorFrame();

        // RGBA, top row first, whatever the backend
        ReadFrame(*renderer, _viewportWidth, _viewportHeight, _frameBuffer);

        BufferWriter response;
        WriteFrameData(response,
                       static_cast<uint32_t>(_viewportWidth),
                       static_cast<uint32_t>(_viewportHeight),
                       _frameBuffer);

        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetAudio(int clientSocket)
    {
        auto &audio = Audio::AudioSystem::Instance();
        BufferWriter response;
        if (!audio.IsLoopback())
        {
            WriteError(response, "No audio stream: audio isn't running on a loopback device (see GetEngineHealth)");
            SendResponse(clientSocket, response.Release());
            return;
        }

        // Everything mixed since the previous GetAudio (at most AudioSystem::StreamBufferMilliseconds of it)
        const Audio::LoopbackFormat &format = audio.GetLoopbackFormat();
        const Audio::StreamedAudio streamed = audio.TakeStreamedAudio();
        WriteAudioSamples(response, format.sampleRate, format.channels, std::string{Audio::ToString(format.sampleFormat)},
                          streamed.frameCount, streamed.droppedFrames, streamed.samples);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandlePollEvents(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const PollEventsCmd cmd = PollEventsCmd::Deserialize(reader);

        // Nothing here may log: every logged line is an event, so a poll that logged would always find one more. (Only
        // a failure, such as a malformed payload, is logged, by ExecuteCommand, as for every polled command.)
        EventBatch batch = _events.Read(cmd.afterSeq, cmd.maxEvents, cmd.epoch);
        BufferWriter response;
        WriteEvents(response, batch.epoch, batch.nextSeq, batch.dropped, nlohmann::json(std::move(batch.events)));
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetViewportSizeCmd::Deserialize(reader);

        if (!IsViewportSizeValid(cmd.width, cmd.height))
        {
            BufferWriter response;
            WriteError(response, std::format("Invalid viewport size {}x{} (each must be 1 to {})",
                                             cmd.width, cmd.height, MaxViewportDimension));
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        _viewportWidth = cmd.width;
        _viewportHeight = cmd.height;

        // The renderer renders at this size from the next frame (an offscreen target on OpenGL, the CPU buffer's
        // size on the software renderer), so frames are neither cropped nor resampled; this also sets the camera's
        // aspect. Without a renderer only the aspect changes. The size is kept even when the renderer can't make
        // the target, so RenderFrame retries it (and answers Error until it works).
        auto &app = Application::GetInstance();
        auto &window = app.GetWindow();
        if (!window.GetRenderer())
        {
            app.OnWindowResize(_viewportWidth, _viewportHeight);
        }
        else if (!window.SetRenderSize(_viewportWidth, _viewportHeight))
        {
            BufferWriter response;
            WriteError(response, RenderTargetError(_viewportWidth, _viewportHeight));
            SendResponse(clientSocket, response.Release());
            return;
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSetCameraPosition(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetCameraPositionCmd::Deserialize(reader);

        auto *camera = Application::GetInstance().GetMainCamera();
        if (camera)
            camera->SetPosition({cmd.x, cmd.y, cmd.z});

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetCameraPosition(int clientSocket)
    {
        auto *camera = Application::GetInstance().GetMainCamera();

        BufferWriter response;
        if (camera)
        {
            auto pos = camera->GetPosition();
            WriteCameraPosition(response, pos[0], pos[1], pos[2]);
        }
        else
        {
            WriteError(response, "No camera");
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleCreateScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateSceneCmd::Deserialize(reader);

        BufferWriter response;

        // AddScene replaces a stored scene with the same name, which would wipe that scene's data
        if (SceneManager::HasScene(cmd.name))
        {
            WriteError(response, "Scene already exists: " + cmd.name);
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        try
        {
            auto newScene = Scene::Create(cmd.name);

            // Serialize it before moving
            nlohmann::json sceneJson = newScene->Serialize();
            std::string sceneJsonString = sceneJson.dump();

            SceneManager::AddScene(std::move(newScene), false);

            WriteSceneData(response, sceneJsonString);
            Logger::Info("Created scene: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            Logger::Error("Failed to create scene: " + std::string(e.what()));
            WriteError(response, std::string("Failed to create scene: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleLoadScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const std::string sceneJsonString = LoadSceneCmd::Deserialize(reader).sceneJson;

        Logger::Info("Loading scene from scene data: " + sceneJsonString.substr(0, 200));

        // Non-throwing parse: the JSON comes from the client
        nlohmann::json sceneJson = nlohmann::json::parse(sceneJsonString, nullptr, false);
        if (sceneJson.is_discarded())
        {
            BufferWriter response;
            WriteError(response, "The scene passed is not valid JSON");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        auto scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "The scene passed was invalid or corrupt");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        if (int sceneIndex = SceneManager::GetSceneIndex(scene->sceneName); sceneIndex != -1)
        {
            SceneManager::UpdateScene(sceneIndex, sceneJson);
            SceneManager::LoadScene(sceneIndex);
        }
        else
        {
            SceneManager::AddScene(sceneJson);
            SceneManager::LoadScene(scene->sceneName);
        }
        SceneManager::ProcessAnyPendingSceneChange();

        // The scene came from the client, not a file: it has none (until SaveSceneToFile names one), and it is
        // unsaved
        _openScenePath.clear();
        _openScene = SceneManager::GetCurScene();
        if (SceneManager::GetCurScene() != nullptr)
        {
            // Opened for editing, like every scene this host loads
            SceneManager::GetCurScene()->SetEditMode(true);
        }
        MarkSceneChanged({}, true);

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSaveScene(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            Logger::Warn("No scene loaded to save");
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});

            return;
        }
        nlohmann::json sceneJson = SceneManager::GetCurSceneRef().Serialize();
        SceneManager::UpdateScene(SceneManager::GetCurSceneIndex(), sceneJson);

        // dump(): passing the json itself converted it to a string, which throws for an object
        WriteSceneData(response, sceneJson.dump());
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleDeleteScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = DeleteSceneCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            // The name comes from the client: only ever delete a scene file inside the scenes directory.
            // Checking then removing is racy, but exploiting that needs local write access to scenes/,
            // which is accepted for a dev tool.
            const auto sceneFile = ResolveSceneFile(_scenesDirectory, cmd.sceneName);
            const Scene *loadedScene = SceneManager::GetCurScene();
            if (_scenesDirectory.empty())
            {
                WriteError(response, "No scenes directory configured (start the host with --project)");
            }
            else if (!sceneFile.has_value())
            {
                Logger::Warn("Rejected DeleteScene outside the scenes directory: " + cmd.sceneName);
                WriteError(response, "Not a scene file in the project's scenes directory: " + cmd.sceneName);
            }
            else if (loadedScene != nullptr && sceneFile->stem() == std::filesystem::path(loadedScene->sceneName))
            {
                // As SceneManager::DeleteScene refuses to delete the loaded scene
                WriteError(response, "Can't delete the loaded scene's file: " + cmd.sceneName);
            }
            else if (std::filesystem::is_regular_file(*sceneFile))
            {
                // Logged with the client's (UTF-8) name, before removing: converting the path back to a
                // narrow string can throw for names the code page can't represent.
                // Only the file is removed. Scenes in SceneManager aren't tied to files (the editor loads
                // them by sending JSON), so a stored scene that happens to share the name is left alone.
                Logger::Info("Deleting scene file: " + cmd.sceneName);
                std::filesystem::remove(*sceneFile);
                WriteOk(response);
            }
            else
            {
                WriteError(response, "Scene file not found: " + cmd.sceneName);
            }
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to delete scene: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetCurrentScene(int clientSocket)
    {
        BufferWriter response;

        if (SceneManager::GetCurScene() == nullptr)
        {
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        try
        {
            Scene &scene = SceneManager::GetCurSceneRef();
            nlohmann::json sceneJson = scene.Serialize();

            WriteSceneData(response, sceneJson.dump());
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
        }
        catch (const std::exception &e)
        {
            Logger::Error("Failed to get current scene: " + std::string(e.what()));
            WriteError(response, "Failed to get current scene: " + std::string(e.what()));
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
        }
    }

    void EditorServer::HandleCreateEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateEntityCmd::Deserialize(reader);

        BufferWriter response;

        // Create entity in current scene
        if (SceneManager::GetCurScene() != nullptr)
        {
            auto gameObject = GameObject::Create(cmd.name);
            SceneManager::GetCurSceneRef().AddRootGameObject(gameObject);
            MarkSceneChanged({gameObject->GetUUID().ToString()});
            WriteEntityCreated(response, gameObject->GetUUID().ToString());
        }
        else
        {
            Logger::Warn("No scene present");
            WriteError(response, "No scene present");
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleDestroyEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const std::string entityId = DestroyEntityCmd::Deserialize(reader).entityId;

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        bool entityDestroyed = false;
        if (const auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            if (const auto foundGameObject = scene->FindGameObjectByUUID(uuid.value()))
            {
                // A real destroy (OnDisable/OnDestroy, children, components), not just detaching the root.
                // The editor host never runs game frames, so the end-of-frame teardown is run here instead;
                // it flushes every queued destroy in the scene and runs component (incl. Lua) callbacks.
                // If a callback throws, ExecuteCommand reports it as an Error (some objects may then stay
                // marked but unpurged) and later commands keep working.
                // Its whole subtree goes with it: the ids a client drops
                std::vector<std::string> destroyedIds = SubtreeIds(*foundGameObject);
                if (scene->DestroyGameObject(foundGameObject))
                {
                    // Marked first: a callback that throws still leaves the scene changed
                    MarkSceneChanged(std::move(destroyedIds));
                    scene->ProcessDestroyed();
                    entityDestroyed = true;
                }
            }
        }

        BufferWriter response;
        if (entityDestroyed)
        {
            WriteOk(response);
        }
        else
        {
            WriteError(response, "No game object found with entity id " + entityId);
        }
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleSetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = SetEntityTransformCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            BufferWriter response;
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // Apply transform to entity
        bool applied = false;
        if (auto uuid = Math::UUID::FromString(cmd.entityId); uuid.has_value())
        {
            auto entity = scene->FindGameObjectByUUID(uuid.value());
            if (entity != nullptr && entity->HasPositionable())
            {
                entity->GetPositionable()->SetPositionAndRotation(
                    cmd.position,
                    Math::Quaternion::FromEulerAngles(cmd.rotation)
                );
                entity->GetPositionable()->SetScale(cmd.scale);
                applied = true;
                MarkSceneChanged({entity->GetUUID().ToString()});
            }
        }

        BufferWriter response;
        if (applied)
            WriteOk(response);
        else
            WriteError(response, "Entity not found (or has no transform): " + cmd.entityId);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetAllEntities(int clientSocket)
    {
        BufferWriter response;

        // No scene: an empty list
        std::vector<EntityInfo> entities;
        if (SceneManager::GetCurScene() != nullptr)
        {
            for (const auto &go : SceneManager::GetCurSceneRef().GetAllGameObjects())
            {
                entities.push_back({go->GetUUID().ToString(), go->GetName()});
            }
        }

        WriteEntityList(response, entities);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetEngineHealth(int clientSocket)
    {
        BufferWriter response;
        WriteEngineHealth(response, Application::GetInstance().GetHealth());
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleGetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const std::string entityId = GetEntityTransformCmd::Deserialize(reader).entityId;

        BufferWriter response;

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            WriteError(response, "No scene loaded");
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        if (auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            auto entity = scene->FindGameObjectByUUID(uuid.value());
            if (entity && entity->HasPositionable())
            {
                auto &transform = entity->GetPositionable()->GetGlobalTransform();
                WriteEntityTransform(response, transform.GetPosition(), transform.GetRotation().ToEulerAngles(),
                                     transform.GetScale());
                SendResponse(clientSocket, response.Release());
                return;
            }
        }

        // An unknown entity used to get an identity transform, indistinguishable from a real one
        WriteError(response, "Entity not found (or has no transform): " + entityId);
        SendResponse(clientSocket, response.Release());
    }

    // ==================== Hierarchy and entities (#6, E4) ====================

    void EditorServer::HandleGetHierarchy(int clientSocket)
    {
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        // Depth first, parents before children, siblings in order: the order the tree shows
        nlohmann::json nodes = nlohmann::json::array();
        size_t rootIndex = 0;
        for (const auto &root : scene->GetRootGameObjects())
        {
            if (root && !root->IsDestroyed())
            {
                AppendHierarchy(*root, std::string{}, rootIndex, nodes);
            }
            ++rootIndex;
        }

        BufferWriter response;
        WriteHierarchy(response, _sceneRevision, nodes);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleCreateEntityEx(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const CreateEntityExCmd cmd = CreateEntityExCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }

        // Everything is checked before anything is made, so a refused request changes nothing
        std::shared_ptr<GameObject> parent;
        if (!cmd.parentId.empty())
        {
            parent = FindEntity(*scene, cmd.parentId);
            if (parent == nullptr)
            {
                SendError(clientSocket, NotFoundMessage("Parent", cmd.parentId));
                return;
            }
        }
        const auto &presets = EntityPresets();
        const auto preset = std::ranges::find_if(presets, [&cmd](const EntityPreset &candidate)
        {
            return candidate.name == cmd.preset;
        });
        if (preset == presets.end())
        {
            std::string known;
            for (const EntityPreset &candidate : presets)
            {
                if (!candidate.name.empty())
                {
                    known += (known.empty() ? "" : ", ") + std::string(candidate.name);
                }
            }
            SendError(clientSocket, std::format("Unknown preset '{}' (the presets are {}, or empty for an empty object)",
                                                SanitizeForLog(cmd.preset), known));
            return;
        }

        // An editor object always has a transform (CreateEntity's has none until something gives it one)
        auto gameObject = GameObject::Create(cmd.name.empty() ? std::string(preset->objectName) : cmd.name);
        gameObject->CreatePositionable();
        preset->addComponents(*gameObject);

        if (parent != nullptr)
        {
            parent->AddChild(gameObject, false);
        }
        else
        {
            scene->AddRootGameObject(gameObject);
        }
        if (cmd.siblingIndex >= 0)
        {
            gameObject->SetSiblingIndex(static_cast<size_t>(cmd.siblingIndex));
        }

        const std::string id = gameObject->GetUUID().ToString();
        MarkSceneChanged({id});
        BufferWriter response;
        WriteEntityCreated(response, id);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetEntityParent(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetEntityParentCmd cmd = SetEntityParentCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }
        std::shared_ptr<GameObject> newParent;
        if (!cmd.parentId.empty())
        {
            newParent = FindEntity(*scene, cmd.parentId);
            if (newParent == nullptr)
            {
                SendError(clientSocket, NotFoundMessage("Parent", cmd.parentId));
                return;
            }
            if (newParent == entity || newParent->IsChildOf(entity))
            {
                SendError(clientSocket, std::format("Can't make '{}' a child of itself or of its own descendant '{}'",
                                                    SanitizeForLog(entity->GetName()), SanitizeForLog(newParent->GetName())));
                return;
            }
        }

        // The place it takes: the last of its new siblings unless the request says otherwise (and a place past the
        // last is the last). Moving it within its own parent leaves one fewer sibling to count.
        const std::shared_ptr<GameObject> oldParent = entity->GetParent();
        const bool sameParent = oldParent == newParent;
        const size_t siblingCount =
            (newParent != nullptr ? newParent->GetChildCount() : scene->GetRootGameObjectCount()) + (sameParent ? 0 : 1);
        const size_t target = cmd.siblingIndex < 0 ? siblingCount - 1
                                                   : std::min(static_cast<size_t>(cmd.siblingIndex), siblingCount - 1);
        if (sameParent && target == entity->GetSiblingIndex())
        {
            // Already there: nothing changes, so the revision doesn't move (the scene isn't made unsaved)
            BufferWriter response;
            WriteOk(response);
            SendResponse(clientSocket, response.Release());
            return;
        }

        entity->SetParent(newParent, cmd.keepWorldTransform);
        if (entity->GetParent() != newParent)
        {
            SendError(clientSocket, "The entity couldn't be moved there");
            return;
        }
        entity->SetSiblingIndex(target);

        MarkSceneChanged({entity->GetUUID().ToString()});
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetEntityProperties(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetEntityPropertiesCmd cmd = SetEntityPropertiesCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }
        // All of it is checked before any of it is applied
        const std::expected<EntityProperties, std::string> parsed = ParseEntityProperties(cmd.properties);
        if (!parsed)
        {
            SendError(clientSocket, parsed.error());
            return;
        }

        bool changed = false;
        if (parsed->name && *parsed->name != entity->GetName())
        {
            entity->SetName(*parsed->name);
            changed = true;
        }
        if (parsed->tag && *parsed->tag != entity->GetTag())
        {
            entity->SetTag(*parsed->tag);
            changed = true;
        }
        if (parsed->layer && *parsed->layer != entity->GetLayer())
        {
            entity->SetLayer(*parsed->layer);
            changed = true;
        }
        if (parsed->active && *parsed->active != entity->IsActive())
        {
            entity->SetActive(*parsed->active);
            changed = true;
        }
        if (changed)
        {
            MarkSceneChanged({entity->GetUUID().ToString()});
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleDuplicateEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const DuplicateEntityCmd cmd = DuplicateEntityCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> source = FindEntity(*scene, cmd.entityId);
        if (source == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        // The subtree as it is saved, built again with fresh UUIDs for every object and component. References
        // between its own objects point at the copies; references to anything else in the scene are kept (the
        // scene's own references are the fallback), so a copied script still points at the same target.
        const ReferenceResolver sceneReferences = SceneReferences(*scene);
        const std::shared_ptr<GameObject> copy = PrefabManager::InstantiatePrefab(source->Serialize(), sceneReferences);
        if (copy == nullptr)
        {
            SendError(clientSocket, "The entity couldn't be duplicated (see the log)");
            return;
        }

        // Next to the original, under its parent
        const std::shared_ptr<GameObject> parent = source->GetParent();
        copy->SetName(DuplicateName(*source, parent != nullptr ? parent->GetChildren() : scene->GetRootGameObjects()));
        if (parent != nullptr)
        {
            parent->AddChild(copy, false);
        }
        else
        {
            scene->AddRootGameObject(copy);
        }
        copy->SetSiblingIndex(source->GetSiblingIndex() + 1);

        MarkSceneChanged(SubtreeIds(*copy));
        BufferWriter response;
        WriteEntityCreated(response, copy->GetUUID().ToString());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleGetEntity(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const GetEntityCmd cmd = GetEntityCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        // An object without a transform is at the origin of the world
        const Positionable *positionable = entity->GetPositionable();
        const Math::Matrix<float, 4, 4> worldMatrix =
            positionable != nullptr ? positionable->GetLocalToWorldMatrix() : Math::Matrix<float, 4, 4>::identity();

        BufferWriter response;
        WriteEntityData(response, EntityDetailsJson(*entity), worldMatrix);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleSetLocalTransform(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetLocalTransformCmd cmd = SetLocalTransformCmd::Deserialize(reader);

        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            SendError(clientSocket, "No scene loaded");
            return;
        }
        const std::shared_ptr<GameObject> entity = FindEntity(*scene, cmd.entityId);
        if (entity == nullptr)
        {
            SendError(clientSocket, NotFoundMessage("Entity", cmd.entityId));
            return;
        }

        const auto finite = [](const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const Math::Quaternion &rotation = cmd.rotation;
        const bool rotationFinite = std::isfinite(rotation.GetX()) && std::isfinite(rotation.GetY()) &&
                                    std::isfinite(rotation.GetZ()) && std::isfinite(rotation.GetW());
        if (!finite(cmd.position) || !finite(cmd.scale) || !rotationFinite)
        {
            SendError(clientSocket, "The transform has a value that isn't a finite number");
            return;
        }
        // A client sends a rotation it may have accumulated rounding in: it is normalised, and only a quaternion
        // with no direction at all (all zeros) is refused
        const float rotationLength = rotation.Length();
        if (!(rotationLength > 1e-6f))
        {
            SendError(clientSocket, "The rotation is a zero quaternion");
            return;
        }

        // An object CreateEntity made has no transform: setting one gives it one
        entity->CreatePositionable();
        Positionable *positionable = entity->GetPositionable();
        positionable->SetLocalPositionAndRotation(cmd.position, rotation.Normalized());
        positionable->SetLocalScale(cmd.scale);

        MarkSceneChanged({entity->GetUUID().ToString()});
        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateScriptCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            std::string scriptTemplate = Scripting::MakeLuaScriptTemplate(cmd.name);
            WriteScriptData(response, scriptTemplate);
            Logger::Info("Generated script template for: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to create script: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleRescanAssets(int clientSocket)
    {
        const IO::ResourceLoader::RescanResult changes = IO::ResourceLoader::Instance().RescanAssets();
        if (!changes.Empty())
        {
            _events.Push("assetsChanged", nlohmann::json{{"added", PathStrings(changes.added)},
                                                         {"removed", PathStrings(changes.removed)},
                                                         {"modified", PathStrings(changes.modified)}});
        }

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    // ==================== Project and scene files ====================

    void EditorServer::SetProject(std::filesystem::path root, IO::ProjectFile project)
    {
        _project = ProjectState{std::move(root), std::move(project)};
    }

    std::expected<ResolvedScenePath, std::string> EditorServer::ResolveScenePath(const std::filesystem::path &assetsRoot,
                                                                                const std::string &path)
    {
        const std::string shown = SanitizeForLog(path);
        if (!IO::ProjectFile::IsScenePath(path))
        {
            return std::unexpected(std::format("Not a scene path: '{}' (expected res://<folder>/<name>.scene)", shown));
        }
        const IO::ResourcePath resourcePath(path);
        const std::filesystem::path relative = FromUtf8(resourcePath.GetPath());
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        {
            return std::unexpected(std::format("Not a scene path inside the project: '{}'", shown));
        }

        // Lexically inside the assets folder, then also with symlinks resolved for the part that exists (as
        // ResolveSceneFile does), so a link inside assets/ can't lead a write elsewhere
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(assetsRoot, error);
        if (error)
        {
            return std::unexpected("The project's assets folder can't be resolved");
        }
        const std::filesystem::path file = (root / relative).lexically_normal();
        const std::filesystem::path parent = std::filesystem::weakly_canonical(file.parent_path(), error);
        if (error)
        {
            return std::unexpected(std::format("Can't resolve the folder of '{}'", shown));
        }
        const std::filesystem::path lexical = file.lexically_relative(root);
        const std::filesystem::path resolved = parent.lexically_relative(root);
        const auto outside = [](const std::filesystem::path &fromRoot)
        {
            return fromRoot.empty() || *fromRoot.begin() == "..";
        };
        if (outside(lexical) || (resolved != "." && outside(resolved)))
        {
            return std::unexpected(std::format("Not a scene path inside the project's assets folder: '{}'", shown));
        }

        // An existing file is resolved itself too: a symlinked file can't lead a read or write out of the folder, and
        // the path takes the spelling the file system has. That spelling, relative to the folder, is the scene's
        // res:// path, so "res://Scenes/main.scene" on a case-insensitive file system is the same scene (the same
        // UUID, the same .meta) as "res://scenes/Main.scene".
        std::filesystem::path target = parent / file.filename();
        if (std::error_code existsError; std::filesystem::exists(target, existsError))
        {
            target = std::filesystem::weakly_canonical(target, error);
            if (error)
            {
                return std::unexpected(std::format("Can't resolve '{}'", shown));
            }
        }
        const std::filesystem::path fromRoot = target.lexically_relative(root);
        if (outside(fromRoot))
        {
            return std::unexpected(std::format("Not a scene path inside the project's assets folder: '{}'", shown));
        }
        IO::ResourcePath canonicalPath(IO::PathType::Resource, IO::PathToUtf8(fromRoot));
        if (!IO::ProjectFile::IsScenePath(canonicalPath.ToString()))
        {
            // A link to a file that isn't a .scene
            return std::unexpected(std::format("Not a scene file: '{}'", shown));
        }
        return ResolvedScenePath{std::move(canonicalPath), std::move(target)};
    }

    std::string EditorServer::OpenScenePath() const
    {
        const Scene *loaded = SceneManager::GetCurScene();
        return loaded != nullptr && loaded == _openScene ? _openScenePath : std::string{};
    }

    void EditorServer::PushSceneChanged(std::vector<std::string> entityIds, const bool full)
    {
        nlohmann::json event{{"revision", _sceneRevision},
                             {"savedRevision", _savedRevision},
                             {"path", OpenScenePath()}};
        if (full)
        {
            // Every id a client holds is invalid, so which ones changed doesn't matter
            event["full"] = true;
        }
        else if (!entityIds.empty() && entityIds.size() <= MaxEventEntityIds)
        {
            event["entityIds"] = std::move(entityIds);
        }
        _events.Push("sceneChanged", std::move(event));
    }

    void EditorServer::MarkSceneChanged(std::vector<std::string> entityIds, const bool full)
    {
        ++_sceneRevision;
        PushSceneChanged(std::move(entityIds), full);
    }

    void EditorServer::SendError(int clientSocket, const std::string &message)
    {
        BufferWriter response;
        WriteError(response, message);
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::LoadOpenedScene(std::unique_ptr<Scene> scene, std::string path)
    {
        // This very object becomes the loaded scene (edit mode: built, never attached)
        Scene *const opened = scene.get();
        opened->SetEditMode(true);
        SceneManager::AddScene(std::move(scene), true);
        SceneManager::ProcessAnyPendingSceneChange();
        if (SceneManager::GetCurScene() != opened)
        {
            throw std::runtime_error("the scene couldn't be loaded");
        }
        _openScene = opened;
        _openScenePath = std::move(path);
        ++_sceneRevision;
        _savedRevision = _sceneRevision;
        // Another scene: every id a client holds is invalid
        PushSceneChanged({}, true);
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::GetOpenSceneInfo() const
    {
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            return std::unexpected("No scene loaded");
        }
        return OpenSceneInfo{
            .path = OpenScenePath(),
            .name = scene->sceneName,
            .uuid = scene->GetUUID().ToString(),
            .revision = _sceneRevision,
            .savedRevision = _savedRevision,
        };
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::OpenSceneFile(const std::string &path)
    {
        if (!_project)
        {
            return std::unexpected("No project: scene files need a host started with --project");
        }
        auto resolved = ResolveScenePath(_project->root / "assets", path);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }
        const std::string resourcePath = resolved->resourcePath.ToString();

        std::error_code error;
        if (!std::filesystem::is_regular_file(resolved->file, error))
        {
            return std::unexpected("Scene file not found: " + SanitizeForLog(resourcePath));
        }
        auto text = ReadTextFile(resolved->file);
        if (!text)
        {
            return std::unexpected(text.error());
        }
        const nlohmann::json sceneJson = nlohmann::json::parse(*text, nullptr, false);
        if (sceneJson.is_discarded())
        {
            return std::unexpected("Not valid JSON: " + SanitizeForLog(resourcePath));
        }
        std::unique_ptr<Scene> scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            return std::unexpected("Not a valid scene (see the log): " + SanitizeForLog(resourcePath));
        }

        // The file's asset UUID, the same every run (and what a .meta records for it): a scene's UUID is its file's.
        // A file added since the last scan is indexed now.
        auto &loader = IO::ResourceLoader::Instance();
        if (!loader.Exists(resolved->resourcePath))
        {
            (void)loader.Reload(resolved->resourcePath);
        }
        scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
        scene->SetResourcePath(resolved->resourcePath);

        LoadOpenedScene(std::move(scene), resourcePath);
        Logger::Info("Opened scene " + resourcePath);
        return GetOpenSceneInfo();
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::NewSceneFile(const std::string &path, const std::string &name)
    {
        std::optional<ResolvedScenePath> resolved;
        if (!path.empty())
        {
            if (!_project)
            {
                return std::unexpected("No project: scene files need a host started with --project");
            }
            auto checked = ResolveScenePath(_project->root / "assets", path);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
            std::error_code error;
            if (std::filesystem::exists(checked->file, error))
            {
                return std::unexpected("A file already exists at " + SanitizeForLog(checked->resourcePath.ToString()) +
                                       " (open it with OpenScene)");
            }
            resolved = std::move(*checked);
        }

        std::string sceneName = name;
        if (sceneName.empty())
        {
            sceneName = resolved ? resolved->resourcePath.GetStem() : std::string("Untitled");
        }
        std::unique_ptr<Scene> scene = Scene::Create(sceneName);

        std::string resourcePath;
        if (resolved)
        {
            // Written before the switch: a file that can't be written leaves the loaded scene as it is
            std::error_code error;
            std::filesystem::create_directories(resolved->file.parent_path(), error);
            if (auto written = IO::WriteTextFileAtomically(resolved->file, SceneFileText(scene->Serialize())); !written)
            {
                return std::unexpected(written.error());
            }
            resourcePath = resolved->resourcePath.ToString();
            (void)IO::ResourceLoader::Instance().Reload(resolved->resourcePath);
            scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
            scene->SetResourcePath(resolved->resourcePath);
        }

        LoadOpenedScene(std::move(scene), resourcePath);
        Logger::Info(resourcePath.empty() ? "New scene " + SanitizeForLog(sceneName) + " (not saved to a file yet)"
                                          : "New scene " + SanitizeForLog(sceneName) + " at " + resourcePath);
        return GetOpenSceneInfo();
    }

    std::expected<OpenSceneInfo, std::string> EditorServer::SaveSceneFile(const std::string &path)
    {
        Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
        {
            return std::unexpected("No scene loaded");
        }
        if (!_project)
        {
            return std::unexpected("No project: scene files need a host started with --project");
        }
        const std::string target = path.empty() ? OpenScenePath() : path;
        if (target.empty())
        {
            return std::unexpected("The open scene has no file yet: pass the path to save it to "
                                   "(res://<folder>/<name>.scene)");
        }
        auto resolved = ResolveScenePath(_project->root / "assets", target);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }

        nlohmann::json sceneJson = scene->Serialize();
        std::error_code error;
        std::filesystem::create_directories(resolved->file.parent_path(), error);
        if (auto written = IO::WriteTextFileAtomically(resolved->file, SceneFileText(sceneJson)); !written)
        {
            return std::unexpected(written.error());
        }
        // As SaveScene does: the stored snapshot is what loading this scene by name rebuilds
        SceneManager::UpdateScene(SceneManager::GetCurSceneIndex(), sceneJson);

        // The file is indexed (or its .meta brought up to date), and a cached copy of the old file dropped
        (void)IO::ResourceLoader::Instance().Reload(resolved->resourcePath);
        const std::string resourcePath = resolved->resourcePath.ToString();
        if (resourcePath != OpenScenePath())
        {
            // Saved as another file: that is the scene's file now, and its UUID the scene's
            scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
            scene->SetResourcePath(resolved->resourcePath);
            _openScene = scene;
            _openScenePath = resourcePath;
        }
        _savedRevision = _sceneRevision;
        PushSceneChanged();
        Logger::Info("Saved scene " + resourcePath);
        return GetOpenSceneInfo();
    }

    std::expected<void, std::string> EditorServer::SaveProject(IO::ProjectFile changed)
    {
        if (auto saved = changed.Save(_project->root); !saved)
        {
            return std::unexpected("Couldn't save " + std::string(IO::ProjectFile::FileName) + ": " + saved.error());
        }
        _project->file = std::move(changed);
        _events.Push("projectChanged", nlohmann::json::object());
        return {};
    }

    void EditorServer::SendSceneInfo(int clientSocket, const std::expected<OpenSceneInfo, std::string> &info)
    {
        BufferWriter response;
        if (info)
        {
            WriteSceneInfo(response, info->path, info->name, info->uuid, info->revision, info->savedRevision);
        }
        else
        {
            WriteError(response, info.error());
        }
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::SendProjectInfo(int clientSocket)
    {
        BufferWriter response;
        WriteProjectInfo(response, Utf8(_project->root), Utf8(IO::ResourceLoader::Instance().GetUserDataRoot()),
                         _project->file.ToJson());
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::SendNoProject(int clientSocket)
    {
        BufferWriter response;
        WriteError(response, "No project: the host wasn't started with --project");
        SendResponse(clientSocket, response.Release());
    }

    void EditorServer::HandleOpenScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const OpenSceneCmd cmd = OpenSceneCmd::Deserialize(reader);
        SendSceneInfo(clientSocket, OpenSceneFile(cmd.path));
    }

    void EditorServer::HandleSaveSceneToFile(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SaveSceneToFileCmd cmd = SaveSceneToFileCmd::Deserialize(reader);
        SendSceneInfo(clientSocket, SaveSceneFile(cmd.path));
    }

    void EditorServer::HandleNewScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const NewSceneCmd cmd = NewSceneCmd::Deserialize(reader);
        SendSceneInfo(clientSocket, NewSceneFile(cmd.path, cmd.name));
    }

    void EditorServer::HandleGetOpenScene(int clientSocket)
    {
        SendSceneInfo(clientSocket, GetOpenSceneInfo());
    }

    void EditorServer::HandleGetProjectInfo(int clientSocket)
    {
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }
        SendProjectInfo(clientSocket);
    }

    void EditorServer::HandleSetProjectSettings(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetProjectSettingsCmd cmd = SetProjectSettingsCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }

        BufferWriter response;
        if (!cmd.settings.is_object())
        {
            WriteError(response, "settings must be a JSON object: a merge patch (RFC 7386) for the project's settings");
            SendResponse(clientSocket, response.Release());
            return;
        }

        // A merge patch: keys set to null are removed, objects merge recursively, anything else replaces
        nlohmann::json merged = _project->file.settings;
        merged.merge_patch(cmd.settings);

        // Only the blocks the patch touches are applied, live, before saving; the state they change is captured first,
        // so a refusal (or a failed save) puts back exactly what was running, including a block the previous settings
        // never had
        std::set<std::string> touched;
        for (const auto &[key, value] : cmd.settings.items())
        {
            touched.insert(key);
        }
        const ProjectSettingsSnapshot before = ProjectSettingsSnapshot::Capture();
        if (const std::vector<std::string> problems = ApplyProjectSettings(merged, touched); !problems.empty())
        {
            before.Restore();
            std::string message = "Project settings refused:";
            for (const std::string &problem : problems)
            {
                message += " " + problem + ";";
            }
            message.pop_back();
            WriteError(response, message);
            SendResponse(clientSocket, response.Release());
            return;
        }

        IO::ProjectFile changed = _project->file;
        changed.settings = std::move(merged);
        if (auto saved = SaveProject(std::move(changed)); !saved)
        {
            before.Restore();
            WriteError(response, saved.error());
            SendResponse(clientSocket, response.Release());
            return;
        }
        Logger::Info("Project settings changed and saved");
        SendProjectInfo(clientSocket);
    }

    void EditorServer::HandleSetStartupScene(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        const SetStartupSceneCmd cmd = SetStartupSceneCmd::Deserialize(reader);
        if (!_project)
        {
            SendNoProject(clientSocket);
            return;
        }

        BufferWriter response;
        IO::ProjectFile changed = _project->file;
        if (cmd.path.empty())
        {
            changed.startupScene.clear();
        }
        else
        {
            const auto resolved = ResolveScenePath(_project->root / "assets", cmd.path);
            std::error_code error;
            if (!resolved || !std::filesystem::is_regular_file(resolved->file, error))
            {
                WriteError(response, resolved ? "Scene file not found: " + SanitizeForLog(cmd.path) : resolved.error());
                SendResponse(clientSocket, response.Release());
                return;
            }
            // Normalised, and added to the scene list if it isn't there
            changed.startupScene = resolved->resourcePath.ToString();
            if (std::ranges::find(changed.scenes, changed.startupScene) == changed.scenes.end())
            {
                changed.scenes.push_back(changed.startupScene);
            }
        }

        if (auto saved = SaveProject(std::move(changed)); !saved)
        {
            WriteError(response, saved.error());
            SendResponse(clientSocket, response.Release());
            return;
        }
        Logger::Info(_project->file.startupScene.empty() ? std::string("The project has no startup scene now")
                                                         : "Startup scene: " + _project->file.startupScene);
        SendProjectInfo(clientSocket);
    }

    bool EditorServer::Send(int socket, const void *data, size_t size)
    {
        size_t sent = 0;
        auto *bytes = static_cast<const char*>(data);

        while (sent < size)
        {
            if (!WaitUntilReady(socket, true)) return false;
            int result = send(socket, bytes + sent, static_cast<int>(size - sent), 0);
            if (result <= 0) return false;
            sent += result;
        }
        return true;
    }

    bool EditorServer::Receive(int socket, void *data, size_t size,
                               std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        size_t received = 0;
        auto *bytes = static_cast<char*>(data);

        while (received < size)
        {
            if (!WaitUntilReady(socket, false, deadline)) return false;
            int result = recv(socket, bytes + received, static_cast<int>(size - received), 0);
            if (result <= 0) return false;
            received += result;
        }
        return true;
    }

    bool EditorServer::WaitUntilReady(int sock, bool forWrite,
                                      std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        const auto nativeSocket = static_cast<NativeSocket>(sock);
        while (_running)
        {
            if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline)
                return false;

            fd_set set;
            FD_ZERO(&set);
            FD_SET(nativeSocket, &set);
            timeval timeout{0, PollIntervalMicroseconds};

            // The first argument is ignored on Windows
            const int result = select(sock + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &timeout);
            if (result > 0) return true;
            if (result < 0) return false;
        }
        return false;
    }

    void EditorServer::SendResponse(int /*clientSocket*/, std::vector<uint8_t> data)
    {
        // Main thread: recorded for ExecuteCommand to return; the network thread does the sending.
        // By value and moved, since a frame can be tens of MiB.
        _response = std::move(data);
    }
}
