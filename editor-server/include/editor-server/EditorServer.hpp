#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>
#include <vector>
#include <cstdint>
#include <string>
#include <string_view>

#include "editor-server/CommandQueue.hpp"
#include "editor-server/EventRing.hpp"

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
    /// Sessions: a client opens one with Hello (protocol version, access token), and it lasts as long as the
    /// connection, so a client that reconnects sends Hello again. Hello is optional unless the server has an access
    /// token (SetAccessToken): then the network thread refuses every other command on a connection, Shutdown included,
    /// until a Hello on that connection has succeeded. Since one connection is served at a time, a connection that
    /// hasn't succeeded a Hello can't be allowed to hold the server: it is closed after a refused command or a failed
    /// Hello, and when no Hello has succeeded within the Hello timeout of accepting it. Without an access token none
    /// of this applies.
    ///
    /// Events: the server keeps an EventRing that PollEvents reads. From construction to destruction it is subscribed to
    /// the Logger, so every line logged in the process (the server's own included) becomes a log event; a host that
    /// constructs the server before Application::Init keeps the engine's startup lines for a client that connects later.
    /// Nothing the ring or PollEvents does logs, so polling an idle server finds no events.
    ///
    /// Security: the server binds to loopback by default. Without an access token there is no authentication, so
    /// any local process can drive the host (including deleting scene files); with --bind, anything that can reach it.
    class EditorServer
    {
    public:
        static constexpr const char *DefaultBindAddress = "127.0.0.1";
        /// Requests declaring a larger payload are refused (and the connection closed) before allocating
        static constexpr uint32_t MaxPayloadBytes = 64u * 1024u * 1024u;
        /// The same, on a connection to a server with an access token that hasn't yet succeeded a Hello (far more
        /// than a Hello's three strings need), so an unauthorised client can't make the server allocate much
        static constexpr uint32_t MaxPayloadBytesBeforeHello = 64u * 1024u;
        /// Largest accepted viewport width/height; the frame buffer is width * height * 4 bytes
        static constexpr int32_t MaxViewportDimension = 4096;
        /// DeleteScene only removes files with this extension (any case) inside the scenes directory
        static constexpr const char *SceneFileExtension = ".json";

        /// Subscribes the event ring to the Logger (see Events above)
        EditorServer();
        /// Stops the server, then unsubscribes the event ring
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

        /// Before Start (the network thread reads it): a non-empty token makes every connection send Hello with this
        /// token before any other command. Empty (the default) leaves Hello optional and accepts any token in it.
        /// False, changing nothing, while the server is running.
        bool SetAccessToken(std::string token);
        [[nodiscard]] bool RequiresAccessToken() const { return !_accessToken.empty(); }

        /// The engine version Hello reports (CMake's project version)
        [[nodiscard]] static std::string_view EngineVersion();
        /// The optional features Hello reports, beyond what the protocol version implies. None are defined yet.
        [[nodiscard]] static std::vector<std::string> Capabilities();
        /// Whether a token matches the access token, comparing every byte (the time taken doesn't depend on where
        /// they first differ, so it doesn't reveal how much of a guess was right)
        [[nodiscard]] static bool TokensMatch(std::string_view accessToken, std::string_view token);
        /// The only command a server with an access token answers before a successful Hello: Hello itself
        [[nodiscard]] static bool IsAllowedBeforeHello(uint8_t commandType);
        /// The Error a server with an access token answers any other command with before a successful Hello
        static constexpr const char *HelloRequiredError =
            "Not authorized: this host requires Hello with its access token before any other command";
        /// How long a server with an access token waits, from accepting a connection, for a Hello to succeed on it
        static constexpr std::chrono::milliseconds DefaultHelloTimeout{5000};
        /// Before Start, like SetAccessToken; false, changing nothing, while running (or for a non-positive timeout)
        bool SetHelloTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] std::chrono::milliseconds GetHelloTimeout() const { return _helloTimeout; }

        /// Before Start, like SetAccessToken (false, changing nothing, while running): when set, the server stops (as
        /// on a Shutdown command, so IsRunning() turns false) once a client's session ends, i.e. its connection
        /// closes for any reason after the session opened. With an access token a session opens with a successful
        /// Hello, so a connection that never authenticated can't stop the server; without one, every connection is
        /// a session. A host launched by an editor sets it (N2EditorHost --exit-on-disconnect), so it doesn't
        /// outlive an editor that crashed or was killed. Off by default: the server goes back to accepting.
        bool SetStopOnDisconnect(bool stop);
        [[nodiscard]] bool StopsOnDisconnect() const { return _stopOnDisconnect; }

        /// Client-supplied text made safe for one log line: control characters (newlines included) become '?', and
        /// text over maxBytes is cut at a UTF-8 character boundary, with "..." appended
        [[nodiscard]] static std::string SanitizeForLog(std::string_view text, size_t maxBytes = 100);

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

        /// The events PollEvents reads: log lines (the Logger subscription), and whatever a handler pushes. Push is
        /// thread-safe; see EventRing.
        [[nodiscard]] EventRing &GetEvents() { return _events; }
        [[nodiscard]] const EventRing &GetEvents() const { return _events; }

        /// True for a command a client is expected to poll: RenderFrame, GetAudio, PollEvents, and (ahead of the planned
        /// editor, #6, which refreshes them continuously) GetAllEntities, GetEntityTransform, GetCameraPosition,
        /// GetEngineHealth.
        /// Rule: a command a client polls never logs per call, or its lines would drown everything else (and, since
        /// every line is an event, PollEvents would always find one). Such a command still logs when it fails
        /// (ExecuteCommand's error line). No command logs a generic "issued" line any more, so this lists the handlers
        /// that must not add a per-call line of their own; a new polled command belongs here.
        [[nodiscard]] static bool IsPolledCommand(uint8_t commandType);

    private:
        void ServerLoop(int listenSocket);
        /// Serves one connection until it closes. sessionOpened is set (and stays set) once the connection has a
        /// session: at once without an access token, at its first successful Hello with one.
        void HandleClient(int clientSocket, bool &sessionOpened);
        void ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload);
        /// Network thread: the Logger isn't thread-safe, so log lines are posted to the main thread
        void PostLog(std::string message, bool isWarning = false);

        // Command handlers
        void HandleHello(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleRenderFrame(int clientSocket);
        void HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAudio(int clientSocket);
        void HandlePollEvents(int clientSocket, const std::vector<uint8_t> &payload);

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
        /// False on disconnect, error, a stopping server, or (when given) once the deadline has passed
        bool Receive(int socket, void *data, size_t size,
                     std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
        /// Network thread: waits (in short slices) until the socket is readable/writable; false once the
        /// server is stopping, on error, or once the deadline (if any) has passed. Keeps every socket call
        /// non-blocking, so Stop() can't hang.
        bool WaitUntilReady(int socket, bool forWrite,
                            std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
        /// Handlers run on the main thread and don't touch the socket: this records the response frame,
        /// which ExecuteCommand returns for the network thread to send
        void SendResponse(int clientSocket, std::vector<uint8_t> data);

        std::atomic<bool> _running{false};
        std::thread _serverThread;
        int _listenSocket{-1};
        int _port{0};
        bool _socketsInitialized{false};
        // Set before Start only, so both threads read it without a lock
        std::string _accessToken;
        std::chrono::milliseconds _helloTimeout{DefaultHelloTimeout};
        // Atomic: a client Shutdown clears _running on the network thread, so the setter's _running check alone
        // doesn't keep it from racing ServerLoop's read
        std::atomic<bool> _stopOnDisconnect{false};

        CommandQueue _commands;
        // Thread-safe (its own mutex): the Logger subscription pushes from whichever thread logged
        EventRing _events;
        size_t _logSubscription{0};
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
