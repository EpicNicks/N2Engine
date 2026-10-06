#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string_view>
#include <future>
#include <system_error>
#include <utility>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

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
    }

    EditorServer::~EditorServer()
    {
        Stop();
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
            try
            {
                HandleClient(clientSocket);
            }
            catch (...)
            {
                // e.g. bad_alloc; nothing may escape this thread (that would terminate the host)
                PostLog("Dropping the editor connection after an unexpected error", true);
            }

            CLOSE_SOCKET(clientSocket);
            PostLog("Editor disconnected");
        }
    }

    void EditorServer::HandleClient(int clientSocket)
    {
        while (_running)
        {
            // Read command header: [type: 1 byte][length: 4 bytes]
            uint8_t cmdType;
            if (!Receive(clientSocket, &cmdType, 1))
                break;

            uint32_t payloadLength;
            if (!Receive(clientSocket, &payloadLength, sizeof(payloadLength)))
                break;

            // The length is client-supplied, so check it before allocating. The oversized payload is
            // still in the stream, so the connection can't be resynchronized and is closed.
            if (!IsPayloadLengthAllowed(payloadLength))
            {
                const std::string message = std::format("Payload of {} bytes exceeds the {} byte limit",
                                                        payloadLength, MaxPayloadBytes);
                BufferWriter response;
                WriteError(response, message);
                Send(clientSocket, response.Data().data(), response.Size());
                PostLog(message, true);
                break;
            }

            // Read payload
            std::vector<uint8_t> payload(payloadLength);
            if (payloadLength > 0 && !Receive(clientSocket, payload.data(), payloadLength))
                break;

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

            if (!Send(clientSocket, response.data(), response.size()))
                break;
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

    void EditorServer::ReadFrame(const Renderer::Common::IRenderer &renderer, const int width, const int height,
                                 std::vector<uint8_t> &pixels)
    {
        const size_t rowBytes = width > 0 ? static_cast<size_t>(width) * 4 : 0;
        const size_t rows = height > 0 ? static_cast<size_t>(height) : 0;
        pixels.assign(rowBytes * rows, 0);
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

    void EditorServer::ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload)
    {
        // RenderFrame and GetAudio are polled every frame; logging them would drown everything else
        if (commandType != static_cast<uint8_t>(CommandType::RenderFrame) &&
            commandType != static_cast<uint8_t>(CommandType::GetAudio))
        {
            Logger::Info("Command Issued: " + std::format("0x{:X}", commandType));
        }
        auto cmd = static_cast<CommandType>(commandType);

        switch (cmd)
        {
        case CommandType::RenderFrame:
            HandleRenderFrame(clientSocket);
            break;
        case CommandType::SetViewportSize:
            HandleSetViewportSize(clientSocket, payload);
            break;
        case CommandType::GetAudio:
            HandleGetAudio(clientSocket);
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
        // covers the default size, before any SetViewportSize, and does nothing when the size is unchanged.
        window.SetRenderSize(_viewportWidth, _viewportHeight);
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
        // aspect. Without a renderer only the aspect changes.
        auto &app = Application::GetInstance();
        if (!app.GetWindow().SetRenderSize(_viewportWidth, _viewportHeight))
        {
            app.OnWindowResize(_viewportWidth, _viewportHeight);
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
                if (scene->DestroyGameObject(foundGameObject))
                {
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

        if (SceneManager::GetCurScene() == nullptr)
        {
            // No scene, return empty list
            response.WriteU8(static_cast<uint8_t>(ResponseType::EntityList));
            response.WriteU32(4); // payload size
            response.WriteU32(0); // count = 0
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        Scene &scene = SceneManager::GetCurSceneRef();
        auto gameObjects = scene.GetAllGameObjects();
        Logger::Info("HandleGetAllEntities: Found " + std::to_string(gameObjects.size()) + " game objects");

        // Build payload
        BufferWriter payload;
        payload.WriteU32(static_cast<uint32_t>(gameObjects.size()));

        for (const auto &go : gameObjects)
        {
            payload.WriteString(go->GetUUID().ToString());
            payload.WriteString(go->GetName());
        }

        response.WriteU8(static_cast<uint8_t>(ResponseType::EntityList));
        response.WriteU32(static_cast<uint32_t>(payload.Size()));
        response.WriteBytes(payload.Data());

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
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

        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
        float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;

        bool found = false;
        if (auto uuid = Math::UUID::FromString(entityId); uuid.has_value())
        {
            auto entity = scene->FindGameObjectByUUID(uuid.value());
            if (entity && entity->HasPositionable())
            {
                found = true;
                auto &transform = entity->GetPositionable()->GetGlobalTransform();
                posX = transform.GetPosition().x;
                posY = transform.GetPosition().y;
                posZ = transform.GetPosition().z;

                auto eulerRotation = transform.GetRotation().ToEulerAngles();

                rotX = eulerRotation.x;
                rotY = eulerRotation.y;
                rotZ = eulerRotation.z;

                scaleX = transform.GetScale().x;
                scaleY = transform.GetScale().y;
                scaleZ = transform.GetScale().z;
            }
        }

        // An unknown entity used to get an identity transform, indistinguishable from a real one
        if (!found)
        {
            WriteError(response, "Entity not found (or has no transform): " + entityId);
            SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
            return;
        }

        // Write response
        response.WriteU8(static_cast<uint8_t>(ResponseType::EntityTransform));
        response.WriteU32(36); // 9 floats = 36 bytes

        response.WriteF32(posX);
        response.WriteF32(posY);
        response.WriteF32(posZ);

        response.WriteF32(rotX);
        response.WriteF32(rotY);
        response.WriteF32(rotZ);

        response.WriteF32(scaleX);
        response.WriteF32(scaleY);
        response.WriteF32(scaleZ);

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    void EditorServer::HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload)
    {
        BufferReader reader(payload);
        auto cmd = CreateScriptCmd::Deserialize(reader);

        BufferWriter response;

        try
        {
            std::string scriptTemplate = GenerateScriptTemplate(cmd.name);
            WriteScriptData(response, scriptTemplate);
            Logger::Info("Generated script template for: " + cmd.name);
        }
        catch (const std::exception &e)
        {
            WriteError(response, std::string("Failed to create script: ") + e.what());
        }

        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
    }

    std::string EditorServer::GenerateScriptTemplate(const std::string &scriptName)
    {
        std::string className = scriptName;
        if (!className.empty())
        {
            className[0] = std::toupper(className[0]);
        }

        size_t dotPos = className.find('.');
        if (dotPos != std::string::npos)
        {
            className = className.substr(0, dotPos);
        }

        for (char &c : className)
        {
            if (!std::isalnum(c))
            {
                c = '_';
            }
        }

        return FillTemplate(GetScriptTemplate(), className);
    }

    std::string EditorServer::GetScriptTemplate()
    {
        return R"(
-- {C} Script
-- Auto-generated by N2Engine Editor

local {C} = {}
{C}.__index = {C}

-- Serializable fields that appear in the inspector
{C}.SerializableFields = {
    speed = { type = "float", default = 5.0 },
    enabled = { type = "bool", default = true },
    -- Reference fields
    -- target = { type = "GameObject", default = nil },
    -- rigidBody = { type = "RigidBodyComponent", default = nil },
}

function {C}:OnAttach()
    -- Initialize component here
end

function {C}:OnUpdate()
    -- Update logic here
end

function {C}:OnFixedUpdate()
    -- Physics update logic here
end

function {C}:OnDestroy()
    -- Cleanup here
end

return {C}
)";
    }

    std::string EditorServer::FillTemplate(std::string templ, const std::string &className)
    {
        size_t pos = 0;
        while ((pos = templ.find("{C}", pos)) != std::string::npos)
        {
            templ.replace(pos, 3, className);
            pos += className.size();
        }
        return templ;
    }

    void EditorServer::HandleRescanAssets(int clientSocket)
    {
        IO::ResourceLoader::Instance().RescanAssets();

        BufferWriter response;
        WriteOk(response);
        SendResponse(clientSocket, {response.Data().begin(), response.Data().end()});
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

    bool EditorServer::Receive(int socket, void *data, size_t size)
    {
        size_t received = 0;
        auto *bytes = static_cast<char*>(data);

        while (received < size)
        {
            if (!WaitUntilReady(socket, false)) return false;
            int result = recv(socket, bytes + received, static_cast<int>(size - received), 0);
            if (result <= 0) return false;
            received += result;
        }
        return true;
    }

    bool EditorServer::WaitUntilReady(int sock, bool forWrite)
    {
        const auto nativeSocket = static_cast<NativeSocket>(sock);
        while (_running)
        {
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
