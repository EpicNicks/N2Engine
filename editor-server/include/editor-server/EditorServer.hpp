#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>
#include <cstdint>
#include <string>

#include "editor-server/CommandQueue.hpp"

namespace Renderer::Common
{
    class IRenderer;
}

namespace N2Engine::Editor
{
    /// TCP host for the editor client.
    ///
    /// Threading model: a single network thread accepts one client at a time and reads its requests, but
    /// never touches engine state (scenes, Lua, GLFW/GL, the Logger). Each request is enqueued on a
    /// CommandQueue and the network thread blocks until the main thread has run it: the host's main loop
    /// calls ProcessCommands() every iteration, which executes the queued commands (handlers, rendering
    /// and logging all happen there) and hands each response frame back for the network thread to send.
    /// Shutdown is the one command answered on the network thread, since it only stops the server.
    ///
    /// Security: there is no authentication, so the server binds to loopback by default. Binding to any
    /// other address lets anything that can reach it drive the host (including deleting scene files).
    class EditorServer
    {
    public:
        static constexpr const char *DefaultBindAddress = "127.0.0.1";
        /// Requests declaring a larger payload are refused (and the connection closed) before allocating
        static constexpr uint32_t MaxPayloadBytes = 64u * 1024u * 1024u;
        /// Largest accepted viewport width/height; the frame buffer is width * height * 4 bytes
        static constexpr int32_t MaxViewportDimension = 4096;
        /// DeleteScene only removes files with this extension (any case) inside the scenes directory
        static constexpr const char *SceneFileExtension = ".json";

        EditorServer() = default;
        ~EditorServer();

        EditorServer(const EditorServer &) = delete;
        EditorServer& operator=(const EditorServer &) = delete;

        /// Main thread. False if the socket couldn't be set up (the server isn't running then).
        bool Start(int port, const std::string &bindAddress = DefaultBindAddress);
        /// Main thread. Disconnects any client and joins the network thread (which polls for stop
        /// between short select() waits, so this returns within ~100 ms).
        void Stop();
        [[nodiscard]] bool IsRunning() const { return _running; }
        /// The port listened on (useful after Start(0)); 0 when not started
        [[nodiscard]] int GetPort() const { return _port; }

        /// Main thread: runs every queued request, waiting up to maxWait for one to arrive.
        size_t ProcessCommands(std::chrono::milliseconds maxWait = std::chrono::milliseconds::zero());

        /// Main thread, once per main-loop iteration: paces the headless engine's loopback audio by the real
        /// time since the previous call (AudioSystem::AdvanceStream, then Update to recycle finished one-shots).
        /// Runs whether or not a client is connected, so sounds finish; the stream buffer it fills is bounded,
        /// and GetAudio drains it. No-op unless audio was initialized with a loopback device.
        void UpdateAudio();

        /// Main thread: runs one request and returns its response frame. Never throws: a failing
        /// request (malformed payload, engine exception) produces an Error response.
        std::vector<uint8_t> ExecuteCommand(uint8_t commandType, const std::vector<uint8_t> &payload);

        /// Where DeleteScene may delete scene files (usually <project>/scenes); empty disables it.
        void SetScenesDirectory(std::filesystem::path scenesDirectory) { _scenesDirectory = std::move(scenesDirectory); }
        [[nodiscard]] const std::filesystem::path& GetScenesDirectory() const { return _scenesDirectory; }

        [[nodiscard]] static bool IsPayloadLengthAllowed(uint32_t payloadLength) { return payloadLength <= MaxPayloadBytes; }
        [[nodiscard]] static bool IsViewportSizeValid(int32_t width, int32_t height);

        /// RenderFrame's pixels: reads the renderer's last frame (IRenderer::ReadFramebuffer, RGBA bottom row
        /// first on every backend) into `pixels` as width x height RGBA8 with the top row first and alpha 255,
        /// what FrameData carries. `pixels` is resized to width * height * 4 bytes (empty unless both are
        /// positive), not cleared: a backend that writes nothing (Vulkan's stub) leaves what was there, made
        /// opaque (black for a buffer only ever used for such a backend, as new bytes are zero).
        static void ReadFrame(const Renderer::Common::IRenderer &renderer, int width, int height,
                              std::vector<uint8_t> &pixels);
        /// The Error SetViewportSize and RenderFrame answer when the renderer can't render at the viewport size
        /// (Window::SetRenderSize failed): "Couldn't create a WxH render target"
        [[nodiscard]] static std::string RenderTargetError(int width, int height);

        /// The scene file a DeleteScene request names: sceneName relative to scenesDirectory, with
        /// SceneFileExtension appended unless it already ends with it (case-insensitively). nullopt unless
        /// its parent resolves (following symlinks) to scenesDirectory or a directory inside it. The file
        /// name itself isn't resolved, so a symlinked scene file means the link, not its target.
        [[nodiscard]] static std::optional<std::filesystem::path> ResolveSceneFile(
            const std::filesystem::path &scenesDirectory, const std::string &sceneName);

        /// True for a command a client is expected to poll: RenderFrame, GetAudio, and (ahead of the planned editor,
        /// #6, which refreshes them continuously) GetAllEntities, GetEntityTransform, GetCameraPosition, GetEngineHealth.
        /// Rule: a command a client polls never logs per call, or its lines would drown everything else. Such a
        /// command still logs when it fails (ExecuteCommand's error line). A new polled command belongs here.
        [[nodiscard]] static bool IsPolledCommand(uint8_t commandType);

    private:
        void ServerLoop(int listenSocket);
        void HandleClient(int clientSocket);
        void ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload);
        /// Network thread: the Logger isn't thread-safe, so log lines are posted to the main thread
        void PostLog(std::string message, bool isWarning = false);

        // Command handlers
        void HandleRenderFrame(int clientSocket);
        void HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAudio(int clientSocket);

        void HandleSetCameraPosition(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetCameraPosition(int clientSocket);

        void HandleLoadScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSaveScene(int clientSocket);
        void HandleCreateScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDeleteScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetCurrentScene(int clientSocket);

        void HandleCreateEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDestroyEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAllEntities(int clientSocket);
        void HandleGetEngineHealth(int clientSocket);
        void HandleGetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload);
        std::string GenerateScriptTemplate(const std::string& scriptName);
        void HandleRescanAssets(int clientSocket);

        // Script Generation
        static std::string FillTemplate(std::string templ, const std::string &className);
        static std::string GetScriptTemplate();

        // Network helpers
        bool Send(int socket, const void *data, size_t size);
        bool Receive(int socket, void *data, size_t size);
        /// Network thread: waits (in short slices) until the socket is readable/writable; false once the
        /// server is stopping or on error. Keeps every socket call non-blocking, so Stop() can't hang.
        bool WaitUntilReady(int socket, bool forWrite);
        /// Handlers run on the main thread and don't touch the socket: this records the response frame,
        /// which ExecuteCommand returns for the network thread to send
        void SendResponse(int clientSocket, std::vector<uint8_t> data);

        std::atomic<bool> _running{false};
        std::thread _serverThread;
        int _listenSocket{-1};
        int _port{0};
        bool _socketsInitialized{false};

        CommandQueue _commands;
        // Main-thread state below
        std::vector<uint8_t> _response;
        std::filesystem::path _scenesDirectory;

        int _viewportWidth{1280};
        int _viewportHeight{720};
        std::vector<uint8_t> _frameBuffer;

        // UpdateAudio's clock; unset until its first call
        std::optional<std::chrono::steady_clock::time_point> _lastAudioUpdate;
    };
}
