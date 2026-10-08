#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourcePath.hpp"

#include "editor-server/CommandQueue.hpp"
#include "editor-server/EditHistory.hpp"
#include "editor-server/EventRing.hpp"

namespace Renderer::Common
{
    class IRenderer;
}

namespace N2Engine
{
    class Scene;
}

namespace N2Engine::Editor
{
    /// The open scene, as OpenScene, NewScene, SaveSceneToFile and GetOpenScene report it (SceneInfo)
    struct OpenSceneInfo
    {
        /// Its file, as a res:// path; empty when it has none (a new scene not saved yet, or one LoadScene sent)
        std::string path;
        std::string name;
        /// The scene's UUID: its file's asset UUID (ResourceUUID::FromPath) when it has a file
        std::string uuid;
        /// The scene revision (see EditorServer::GetSceneRevision)
        uint32_t revision = 0;
        /// The revision when the scene was last opened from or saved to its file; the scene has unsaved changes
        /// exactly when revision != savedRevision
        uint32_t savedRevision = 0;
    };

    /// A scene path a client sent, resolved: the res:// path (normalised) and the file it names
    struct ResolvedScenePath
    {
        IO::ResourcePath resourcePath;
        std::filesystem::path file;
    };

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
        /// The most entityIds a sceneChanged event carries; a change that touched more carries none, so one event
        /// stays small however large the subtree that was deleted or duplicated
        static constexpr size_t MaxEventEntityIds = 256;
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

        // ==================== Project and scene files (main thread) ====================

        /**
         * The project the host opened, at root (its absolute folder). Call it once ResourceUUID and ResourceLoader are
         * initialised for the project (RunHost does: the projectId namespace, ResourceLoader::Initialize(root, the
         * project's user data folder)). Enables GetProjectInfo, SetProjectSettings, SetStartupScene and the scene-file
         * commands; without a project they answer Error. Hello's projectLoaded reports it.
         */
        void SetProject(std::filesystem::path root, IO::ProjectFile project);
        [[nodiscard]] bool HasProject() const { return _project.has_value(); }
        /// The project file as last saved, or nullptr without a project
        [[nodiscard]] const IO::ProjectFile *GetProject() const { return _project ? &_project->file : nullptr; }
        [[nodiscard]] std::filesystem::path GetProjectRoot() const { return _project ? _project->root : std::filesystem::path{}; }

        /**
         * What OpenScene runs: reads the .scene file at path (a res:// path), builds it (Scene::FromJSON, validated;
         * edit mode: no component is attached) and makes it the loaded scene, with the file's asset UUID. The scene
         * revision moves on and the scene is saved at it. An Error message when there is no project, the path isn't a
         * res:// .scene path inside the assets folder, or the file can't be read or isn't a valid scene; the loaded
         * scene is kept then.
         */
        std::expected<OpenSceneInfo, std::string> OpenSceneFile(const std::string &path);
        /**
         * What NewScene runs: an empty scene named name (empty: the file's stem, or "Untitled") becomes the loaded
         * scene. With a path (a res:// .scene path that doesn't exist yet) the scene is written there first and has
         * that file; with an empty path it has none until SaveSceneToFile names one. Either way it starts saved
         * (nothing to lose). Needs a project when a path is given.
         */
        std::expected<OpenSceneInfo, std::string> NewSceneFile(const std::string &path, const std::string &name);
        /**
         * What SaveSceneToFile runs: writes the loaded scene's JSON (Scene::Serialize, indented) to path, or to the
         * scene's own file when path is empty (an Error when it has none), replacing the file atomically; also stores
         * it in SceneManager as SaveScene does. A path other than the scene's own ("save as") becomes its file, and
         * the scene takes that file's UUID. The file is indexed (its .meta written) and the saved revision becomes
         * the current one.
         */
        std::expected<OpenSceneInfo, std::string> SaveSceneFile(const std::string &path);
        /// What GetOpenScene answers; an Error message when no scene is loaded
        [[nodiscard]] std::expected<OpenSceneInfo, std::string> GetOpenSceneInfo() const;

        /**
         * The scene revision: 0 until a scene is loaded, then moved on by every command that changes the loaded scene
         * or loads another (OpenScene, NewScene, LoadScene, CreateEntity, CreateEntityEx, DestroyEntity,
         * SetEntityTransform, SetLocalTransform, SetEntityParent, SetEntityProperties, DuplicateEntity, AddComponent,
         * RemoveComponent, SetComponentFields, and Undo and Redo). It only grows, so a client refetches what it shows
         * (GetHierarchy, GetEntity) whenever it changes; undoing is a change like any other, so the revision after an
         * undo is not the revision before the edit. Each change, and each save, pushes a sceneChanged event
         * {revision, savedRevision, path}; a change adds entityIds (the objects it touched, when there are few enough)
         * or full (another scene was loaded, or a step was undone or redone by restoring a snapshot of the scene:
         * every id is invalid).
         */
        [[nodiscard]] uint32_t GetSceneRevision() const { return _sceneRevision; }
        /// The revision the loaded scene was last opened or saved at (see OpenSceneInfo::savedRevision). Undoing or
        /// redoing back to the state that was saved makes it the current revision again (no unsaved changes).
        [[nodiscard]] uint32_t GetSavedRevision() const { return _savedRevision; }

        // ==================== Undo, redo and autosave (#78, E6) ====================

        /**
         * The undo history. Every editing command records what it did (a step, or part of the open edit group), Undo
         * and Redo walk it, and opening, creating or loading a scene clears it; saving doesn't. Exposed for the host
         * and tests: a client only reaches it through the commands.
         */
        [[nodiscard]] EditHistory &GetHistory() { return _history; }
        [[nodiscard]] const EditHistory &GetHistory() const { return _history; }

        /// How often at most the autosave is written while edits keep coming (the last one waits for the host's loop,
        /// ProcessCommands). Zero writes it after every step. 2 seconds by default.
        static constexpr std::chrono::milliseconds DefaultAutosaveInterval{2000};
        void SetAutosaveInterval(const std::chrono::milliseconds interval) { _autosaveInterval = interval; }

        /// Where the autosave of the open scene goes: <project>/.n2/autosave/ then the scene's path under assets/ (or
        /// ".untitled.scene" for a scene with no file). Empty without a project. Never the scene's own file.
        [[nodiscard]] std::filesystem::path GetAutosaveFile() const;
        /// An autosave found when a scene was opened is kept, and none is written over it, until RestoreAutosave,
        /// DiscardAutosave or a save: a client that finds it after a crash can offer it first
        [[nodiscard]] bool IsAutosaveProtected() const { return _autosaveProtected; }

        /**
         * A scene path a client sent, checked: "res://" then a path that stays inside assetsRoot (lexically, and with
         * symlinks resolved for the part that exists), whose file name ends in ".scene" (any case). The file needn't
         * exist. An Error message otherwise.
         */
        [[nodiscard]] static std::expected<ResolvedScenePath, std::string> ResolveScenePath(
            const std::filesystem::path &assetsRoot, const std::string &path);

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
        /// GetEngineHealth, GetHierarchy, GetEntity.
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

        void HandleGetHierarchy(int clientSocket);
        void HandleCreateEntityEx(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityParent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityProperties(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDuplicateEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetLocalTransform(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleGetComponentTypes(int clientSocket);
        void HandleAddComponent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleRemoveComponent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetComponentFields(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetComponent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetLuaFields(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleRescanAssets(int clientSocket);

        void HandleOpenScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSaveSceneToFile(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleNewScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetOpenScene(int clientSocket);
        void HandleGetProjectInfo(int clientSocket);

        void HandleUndo(int clientSocket);
        void HandleRedo(int clientSocket);
        void HandleBeginEditGroup(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleEndEditGroup(int clientSocket);
        void HandleGetHistory(int clientSocket);
        void HandleGetAutosave(int clientSocket);
        void HandleRestoreAutosave(int clientSocket);
        void HandleDiscardAutosave(int clientSocket);
        void HandleSetProjectSettings(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetStartupScene(int clientSocket, const std::vector<uint8_t> &payload);

        /// Answers a scene-file operation: SceneInfo, or Error with its message
        void SendSceneInfo(int clientSocket, const std::expected<OpenSceneInfo, std::string> &info);
        /// ProjectInfo for the project (there must be one)
        void SendProjectInfo(int clientSocket);
        /// The Error a project command answers without a project
        void SendNoProject(int clientSocket);
        /// Saves the project file with this change, then keeps it; an Error message when it can't be saved
        std::expected<void, std::string> SaveProject(IO::ProjectFile changed);

        /// The loaded scene changed (or another was loaded): moves the revision on and pushes sceneChanged.
        /// entityIds: the objects the change touched (what a client refetches); no more than MaxEventEntityIds of them
        /// go into the event, and without any (or with more) it carries none, which a client reads as "any may have
        /// changed". full: another scene was loaded, so every id a client holds is invalid.
        void MarkSceneChanged(std::vector<std::string> entityIds = {}, bool full = false);
        /// Pushes sceneChanged with the current revisions and path, and entityIds and full as MarkSceneChanged takes them
        void PushSceneChanged(std::vector<std::string> entityIds = {}, bool full = false);
        /// Answers with an Error
        void SendError(int clientSocket, const std::string &message);

        // Undo and redo
        /// Undo (true) or Redo: runs the step, moves the revision on, and answers EditResult
        void UndoOrRedo(int clientSocket, bool undo);
        /// Records an edit that was just made: a step of its own, merged into the step before it, or part of the open
        /// group (see EditHistory::Record). Pushes historyChanged for a new step, and notes that a step ended.
        void RecordEdit(std::string label, EditOp op);
        /// Pushes historyChanged {canUndo, canRedo, label, redoLabel, undoCount, redoCount}
        void PushHistoryChanged();
        /// Ends every open edit group (a client that went away without ending its group)
        void CloseEditGroups();
        /// The loaded scene rebuilt from a snapshot (Scene::Serialize text), keeping its UUID, file and edit mode:
        /// what undoing a destroy or a component removal does, so every reference is resolved again by UUID. The
        /// effect is full.
        EditOutcome RestoreSceneSnapshot(const std::string &snapshot);
        /// A snapshot of the loaded scene, for RestoreSceneSnapshot
        [[nodiscard]] static std::string SnapshotScene(const Scene &scene);

        // Autosave
        /// An edit step ended (or was undone): the autosave is due, written now unless the interval says to wait
        void NoteEditStepEnded();
        /// Writes the autosave when one is due and the interval allows (or force). Removes it when the scene has no
        /// unsaved changes any more.
        void FlushAutosave(bool force = false);
        void RemoveAutosaveFile(const std::filesystem::path &file);
        /// A scene was loaded: no autosave is due, and one the file system has from before is protected
        void ResetAutosaveState();
        /// The res:// path of the loaded scene's file, or "" when it has none (or the loaded scene isn't the one
        /// that was opened from it)
        [[nodiscard]] std::string OpenScenePath() const;
        /// Makes scene the loaded scene, with this file (empty for none), at a new revision that counts as saved
        void LoadOpenedScene(std::unique_ptr<Scene> scene, std::string path);

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

        struct ProjectState
        {
            std::filesystem::path root;
            IO::ProjectFile file;
        };
        std::optional<ProjectState> _project;

        // The loaded scene's file (a res:// path, "" for none) and the Scene it was opened into: once another scene is
        // loaded (LoadScene, or anything else that switches scenes), the file is no longer the loaded scene's
        std::string _openScenePath;
        const Scene *_openScene{nullptr};
        uint32_t _sceneRevision{0};
        uint32_t _savedRevision{0};

        EditHistory _history;
        std::chrono::milliseconds _autosaveInterval{DefaultAutosaveInterval};
        std::optional<std::chrono::steady_clock::time_point> _lastAutosave;
        bool _autosavePending{false};
        bool _autosaveProtected{false};
        bool _autosaveWarned{false};

        int _viewportWidth{1280};
        int _viewportHeight{720};
        std::vector<uint8_t> _frameBuffer;

        // UpdateAudio's clock; unset until its first call
        std::optional<std::chrono::steady_clock::time_point> _lastAudioUpdate;
    };
}
