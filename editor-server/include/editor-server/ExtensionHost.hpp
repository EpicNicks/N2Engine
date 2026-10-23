#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/scripting/LuaSandbox.hpp"

struct lua_State;

namespace N2Engine::Editor
{
    /// One file's evaluation on a worker thread (ExtensionHost.cpp)
    struct ExtensionLoadJob;
    /// One invocation of a menu item on a worker thread (ExtensionHost.cpp)
    struct ExtensionInvokeJob;

    /// How an extension file stands (design lua-editor-extensions 5.2). `Failed` covers every way a file does not load:
    /// it could not be read, it is too large, its chunk raised an error or ran out of budget or time, its declaration
    /// has a bad shape, or its apiVersion is not one this host speaks.
    enum class ExtensionState
    {
        Loaded,
        Failed,
        /// Loaded, then too many failures in a short time (NoteFailure): disabled until the next reload
        Faulted,
    };

    /// "loaded", "failed" or "faulted" (the wire spelling)
    [[nodiscard]] std::string_view ExtensionStateName(ExtensionState state);

    /**
     * Lua editor extensions, the host side of E-X2 (design: lua-editor-extensions sections 4 to 7). It finds the
     * extension files of a project (`<project>/editor/**` `/*.lua`, outside `assets/`, so no protocol write, `require`
     * or play host can reach them), runs each in a sandbox state of its own (SandboxProfile::EditorExtension, the load
     * budget, 64 MiB), reads the `Editor.Extension{...}` it returns, checks its shape, and keeps a registry that the
     * read commands (ListEditorExtensions, GetEditorUi) answer from. Nothing runs Lua when those commands are served:
     * the registry holds plain data captured at load time.
     *
     * The `Editor` namespace of a state is `Editor.Extension`, `Editor.Log` (and `print`, which writes to it),
     * `Editor.Inspector.Register` (E-X5: inspector sections for a component type, callable only while the file loads)
     * and, for the capabilities the file declared and the host granted, `Editor.Scene`: scene.read installs the
     * readers, scene.write the writers, and a state without a capability does not have the functions at all (absent,
     * not refused). A state is idle between invocations; Invoke (E-X3) runs one menu item's `run(ctx, params)` in it,
     * InvokeAction (E-X5) one inspector button's.
     *
     * Lifecycle: Load at host start (only when the host was started with --editor-extensions) and Reload (the
     * ReloadEditorExtensions command), and nowhere else: opening or loading a scene never touches an extension. A broken
     * file only marks itself failed; the others load.
     *
     * Threading: each file's evaluation (state, chunk, declaration) runs on a WORKER thread under a wall-clock
     * abandon deadline, exactly as the ScriptDeclarationCache does and for the same reason: concat, unpack and sort are
     * stock C functions and the collector can spend long inside one C call that neither the instruction hook nor the
     * allocator cap can interrupt (see docs/scripting-lua.html). The caller (the command thread) waits at most
     * AbandonMillis(); past it the worker is ABANDONED: it owns a copy of everything it touches and a shared result slot,
     * never reads or writes the registry again, and its state is closed by whichever side lets go last. At most
     * MaxWorkers workers exist at once, abandoned ones included; a file that finds none free fails at once (it is tried
     * again at the next reload), and so does a file whose abandoned worker with the same source is still running.
     * Shutdown stops new evaluations and waits a bounded time for the running workers; a thread cannot be interrupted,
     * so one still running after that is left, holding nothing of the host.
     *
     * An invocation (Invoke) runs the same way: the item's function runs on a WORKER thread under the invocation budget
     * (SandboxDefaults::InvocationBudget: 200 million instructions and 5 s of wall clock) while the command thread
     * waits. The worker never touches the editor: every `Editor.Scene` and `Editor.Log` call of the script is a request
     * the worker hands to the command thread, which answers it through the handler it was given (the EditorServer's
     * nested dispatch) and wakes the worker with the answer, so every scene change is made on the command thread, one at
     * a time, through the same command layer as any client. A worker that does not ask or finish within its wall budget
     * plus InvokeGraceMillis is ABANDONED as a load worker is (it owns a copy of what it touches, the state moves into
     * its job and is closed by whichever side lets go last, and the extension is faulted until the next reload).
     *
     * Calls are made from the command thread (Load, Reload, Invoke, the readers); the registry is also guarded by a mutex, so a
     * reader on another thread sees either the old or the new registry, never a half-built one. Every read of the
     * registry, the log lines of Load included, happens under that mutex; only the logging itself and the changed
     * callback run after it is released.
     *
     * Limits of one Load: each state is capped at SandboxDefaults::MaxBytes, all the states together at MaxTotalBytes,
     * and the whole Load at ReloadBudgetMillis of wall-clock time. The new states are made before the old ones are
     * closed (the swap is atomic), so during a reload both sets exist at once.
     */
    class ExtensionHost
    {
    public:
        /// The folder of the project the files are found in
        static constexpr std::string_view FolderName = "editor";
        /// Per file (design 5.2); a larger file fails without being read
        static constexpr std::uintmax_t MaxFileBytes = std::uintmax_t{1} * 1024 * 1024;
        /// Files loaded; later ones (in sorted order) are not, and the host logs how many
        static constexpr std::size_t MaxFiles = 64;
        /// Directory levels below editor/ that are searched
        static constexpr int MaxDirectoryDepth = 8;
        /// Directory entries visited in the walk, whatever they are
        static constexpr std::size_t MaxVisitedEntries = 4096;
        /// The apiVersion this host speaks (design 5.2: a set; v1 is {1})
        static constexpr int SupportedApiVersion = 1;
        /// Worker threads that may exist at once, abandoned ones included
        static constexpr int MaxWorkers = 4;
        /// How long past two load budgets (the chunk, then the declaration read) the caller waits before it abandons a
        /// worker: the two budgets stop a script that runs Lua; this is for one that is stuck in C
        static constexpr std::uint64_t GraceMillis = 1000;
        /// Bytes all the extension states of a Load may hold together (the sum of what each loaded state held at its
        /// most while it loaded: LuaSandbox's peak counter). A state gets the smaller of SandboxDefaults::MaxBytes and
        /// what is left of this as its own cap; a file that finds less than SandboxDefaults::MinBytes left, or that
        /// needs more than is left, fails as data ("extension memory budget exhausted") and the others still load. The
        /// files are taken in their sorted order, so which ones fit is the same on every run.
        static constexpr std::size_t MaxTotalBytes = std::size_t{256} * 1024 * 1024;
        /// The wall-clock budget of one Load (and so of a Reload and of the host's start): a file still to be loaded
        /// when it is spent fails as data ("reload time budget exhausted") and is tried again at the next reload, and a
        /// file whose wait would pass it is abandoned at it
        static constexpr std::uint64_t ReloadBudgetMillis = 10000;
        /// How long Shutdown waits for running workers by default
        static constexpr std::chrono::milliseconds DefaultShutdownWait{1000};
        /// Failures (NoteFailure) within FaultWindow that make an extension faulted (design 5.2: 3 in 10 minutes)
        static constexpr int FaultThreshold = 3;
        static constexpr std::chrono::minutes FaultWindow{10};

        // ---- Invocation limits (design 6.4, decision D7) ----

        /// Commands one invocation may send to the host (Editor.Scene calls; Editor.Log lines are not counted)
        static constexpr std::uint32_t MaxNestedCommands = Scripting::SandboxDefaults::MaxNestedCommands;
        /// Bytes of requests and answers one invocation may move between its script and the host, together
        static constexpr std::uint64_t MaxInvocationBytes = std::uint64_t{64} * 1024 * 1024;
        /// Editor.Log lines one invocation may write; later ones are dropped (one line says so)
        static constexpr std::uint32_t MaxLogLines = 1000;
        /// A line's text is cut to this many bytes
        static constexpr std::size_t MaxLogLineBytes = 1024;
        /// Selection entries in a result, warnings in a result, and the bytes of a text in one
        static constexpr std::size_t MaxResultSelect = 4096;
        static constexpr std::size_t MaxResultWarnings = 100;
        static constexpr std::size_t MaxResultTextBytes = 1024;
        /// How long past its wall budget the command thread waits for a worker that has neither asked nor finished before it
        /// abandons it
        static constexpr std::uint64_t InvokeGraceMillis = 1000;

        // ---- Shape limits of Editor.Extension{...} (design 4.1, 4.4) ----

        static constexpr std::size_t MaxNameBytes = 64;
        static constexpr std::size_t MaxCapabilities = 8;
        static constexpr std::size_t MaxMenuItems = 64;
        static constexpr std::size_t MaxItemIdBytes = 64;
        static constexpr std::size_t MaxPathBytes = 200;
        static constexpr std::size_t MaxPathSegments = 6;
        static constexpr std::size_t MaxTitleBytes = 100;
        static constexpr std::size_t MaxConfirmBytes = 200;
        static constexpr std::size_t MaxTooltipBytes = 200;
        /// Fields of one form (a menu item's params)
        static constexpr std::size_t MaxFormFields = 32;
        static constexpr std::size_t MaxEnumOptions = 64;
        static constexpr std::size_t MaxFieldNameBytes = 64;

        // ---- Shape limits of Editor.Inspector.Register (design 4.2, E-X5) ----

        /// Calls of Editor.Inspector.Register one file may make while it loads
        static constexpr std::size_t MaxInspectorRegistrations = 64;
        /// Inspector sections of one extension, all its registrations together
        static constexpr std::size_t MaxInspectorSections = 32;
        /// Buttons of one section
        static constexpr std::size_t MaxSectionButtons = 16;
        /// A component type name (an identifier: letters, digits and '_', not starting with a digit)
        static constexpr std::size_t MaxComponentTypeBytes = 64;

        // ---- Hooks (design 4.5, E-X6) ----

        /// The hooks an extension may declare with `hooks = { OnSceneOpened = function(info) ... end, ... }`, in the order
        /// the host documents them
        static constexpr std::string_view HookNames[] = {"OnSceneOpened", "OnSceneSaved", "OnBeforePlay", "OnAssetsChanged"};
        /// Requests (Editor.Scene and Editor.Assets reads) one run of a hook may send: far under an invocation's cap,
        /// because a hook runs inside a save, an open, a play or an asset change
        static constexpr std::uint32_t MaxHookNestedCommands = 1000;
        /// Paths in each of the three lists OnAssetsChanged is given; a longer list is cut to this many
        static constexpr std::size_t MaxHookAssetPaths = 1000;
        /// The wall-clock time the hooks of one trigger (one save, one open, one play, one asset change) may take together,
        /// all extensions counted; once it is spent the extensions not yet run are skipped, and the host logs it
        static constexpr std::uint64_t HookTotalMillis = 2000;
        /// Entries (folders and assets together) Editor.Assets.List returns; a folder with more is an error of the call
        static constexpr std::size_t MaxAssetListEntries = 10000;

        /// What the host grants a state (v1; reserved for later: project.write, import.hooks)
        static constexpr std::string_view CapabilityNames[] = {"scene.read", "scene.write", "assets.read",
                                                               "assets.write"};

        /// `projectRoot` holds project.n2proj and assets/; the extension files are in its editor/ folder. An empty
        /// root has none (the host runs without --project).
        explicit ExtensionHost(std::filesystem::path projectRoot);
        /// Shutdown, then drops the registry (closing the idle states)
        ~ExtensionHost();

        ExtensionHost(const ExtensionHost &) = delete;
        ExtensionHost &operator=(const ExtensionHost &) = delete;

        /// Finds and loads every file, replacing the registry, and logs one line per extension ("name@sha8
        /// capabilities" for a loaded one, the reason for a failed one). Sets the uiRevision to 1 the first time and
        /// bumps it on every later call, which is what Reload is. Does not call the changed callback. Blocks the caller
        /// for the time the files take: each is bounded by AbandonMillis(), the whole call by ReloadBudget() (files not
        /// loaded when it ends fail at once as data), and once MaxWorkers workers are stuck the remaining files fail at
        /// once.
        void Load();
        /// Load, then tells the changed callback (the host pushes extensionsChanged)
        void Reload();

        /// Called with the new uiRevision after Reload and after NoteFailure makes an extension faulted, on the thread
        /// that made the change. Not under the registry's mutex.
        void SetChangedCallback(std::function<void(std::uint32_t uiRevision)> callback);

        /// ListEditorExtensions' answer: {"enabled": true, "uiRevision", "extensions": [{id, name, sha256, apiVersion,
        /// state, error?, capabilities, itemCount}]}, in the files' sorted order. Reads the registry only.
        [[nodiscard]] nlohmann::json DescribeJson() const;
        /// GetEditorUi's answer: {"uiRevision", "menuItems": [...], "inspectorSections": [...]}. The menu items and the
        /// inspector sections of every loaded or faulted extension, in the files' sorted order then as declared (the
        /// sections in the order of the file's Editor.Inspector.Register calls). Reads the registry only.
        [[nodiscard]] nlohmann::json UiJson() const;
        /// What a host without the flag answers ListEditorExtensions with: enabled false, revision 0, nothing listed
        [[nodiscard]] static nlohmann::json DisabledDescribeJson();
        /// What a host without the flag answers GetEditorUi with
        [[nodiscard]] static nlohmann::json DisabledUiJson();

        [[nodiscard]] std::uint32_t UiRevision() const;
        /// Extensions in the registry (any state) and how many of them loaded
        [[nodiscard]] std::size_t ExtensionCount() const;
        [[nodiscard]] std::size_t LoadedCount() const;

        /// Records a failure of the extension with this id (an error or a budget trip while it ran; E-X3 calls it).
        /// FaultThreshold failures within FaultWindow make it Faulted until the next reload, which tells the changed
        /// callback with a new uiRevision. Returns the state after. Unknown ids and extensions that are not Loaded are
        /// left alone (their state is returned, or Failed for an unknown id).
        ExtensionState NoteFailure(std::string_view id,
                                   std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /// What the loaded extension with this id was charged against MaxTotalBytes: the most its state held while it
        /// loaded. 0 for an unknown id and for one that failed (it keeps no state); a faulted one keeps its state and its charge.
        [[nodiscard]] std::size_t ChargedBytesOf(std::string_view id) const;
        /// The state of the extension with this id; Failed for an unknown one
        [[nodiscard]] ExtensionState StateOf(std::string_view id) const;

        /// Stops new evaluations and waits up to `maxWait` for this host's running workers (abandoned ones too).
        /// Returns how many are still running when it gives up (0: all finished). Idempotent.
        int Shutdown(std::chrono::milliseconds maxWait = DefaultShutdownWait);

        /// The sha256 of `data` as 64 lowercase hex digits (what the registry shows for a source)
        [[nodiscard]] static std::string Sha256Hex(std::string_view data);

        // ---- Invocation (E-X3) ----

        /// A menu item, or a button of an inspector section (E-X5), as the registry holds it, for the host that is asked to
        /// run it
        struct MenuItemRef
        {
            /// false: a menu item (InvokeMenuItem); true: an inspector section's button (InvokeInspectorAction), whose
            /// itemId is the button's id, title its label and componentType its section's
            bool inspectorAction = false;
            /// true: a hook of the extension (FindHooks, InvokeHook), whose itemId is the hook's name ("OnSceneSaved") and
            /// whose title is the same; it is read-only and runs under the hook budget
            bool hook = false;
            std::string componentType;
            std::string extensionId;
            std::string extensionName;
            /// The first 8 hex digits of the file's sha256 (what the audit line and the load log name)
            std::string sha8;
            /// Loaded or Faulted (a failed extension has no items)
            ExtensionState state = ExtensionState::Failed;
            std::vector<std::string> capabilities;
            std::string itemId;
            std::string title;
            /// "editor" or "admin": the least role that may invoke it
            std::string minRole;
            /// The item's form (MenuForm: {fields, defaults}); null for an item without params
            nlohmann::json form;
        };

        /// What the worker asks the command thread to do for a script (an Editor.Scene or Editor.Log call), and the answer
        struct NestedAnswer
        {
            bool ok = true;
            /// What the call returns to the script (null for nothing)
            nlohmann::json value;
            /// ok false: the text of the Lua error the script gets
            std::string error;
        };
        /// Answers one request of the script: `op` ("log", "scene.getEntity", ...) and its arguments as JSON. Called on the
        /// thread that called Invoke, one request at a time, while the worker waits. An exception it throws is an error of
        /// the call.
        using NestedHandler = std::function<NestedAnswer(const std::string &op, const nlohmann::json &args)>;

        /// How an invocation went wrong
        enum class InvokeFailure
        {
            None,
            /// The item raised a Lua error (or a command it sent that the host refused went uncaught)
            LuaError,
            /// The instruction budget or the wall clock ended it: the extension is faulted until the next reload
            BudgetExceeded,
            /// The state's memory cap was reached: faulted as well
            MemoryExhausted,
            /// The worker neither asked nor finished in time and was abandoned: faulted as well
            Abandoned,
            /// What the item returned is not a result ({message, select, warnings})
            BadResult,
            /// The host could not run it (no state, shutting down, no worker free, no thread): nothing is counted against
            /// the extension
            HostError,
        };

        struct InvokeOutcome
        {
            InvokeFailure failure = InvokeFailure::None;
            [[nodiscard]] bool Ok() const { return failure == InvokeFailure::None; }
            /// On success: {message?, select?, warnings?}, each left out when empty
            nlohmann::json result = nlohmann::json::object();
            /// On failure: what went wrong, printable and with no host path (without the extension's name)
            std::string error;
            /// The extension is Faulted after this call
            bool faulted = false;
            /// Requests of the script the handler answered, Editor.Log lines not counted
            std::uint32_t nestedCommands = 0;
        };

        /// The loaded or faulted extension's menu item with this id, or nullopt (ids are unique across the host)
        [[nodiscard]] std::optional<MenuItemRef> FindMenuItem(std::string_view itemId) const;
        /// The loaded or faulted extension's inspector button with this id (inspectorAction true), or nullopt (button ids
        /// are unique across the host, apart from the menu item ids)
        [[nodiscard]] std::optional<MenuItemRef> FindInspectorAction(std::string_view actionId) const;

        /// Whether `text` is a UUID in its written form (8-4-4-4-12 hex digits): what an entity id is
        [[nodiscard]] static bool IsUuidText(std::string_view text);
        /// The commands one invocation may send: MaxNestedCommands unless a test replaced it
        [[nodiscard]] static std::uint32_t MaxNested();

        /// Runs the item's run(ctx, params) on a worker (see the class comment) with the invocation budget, with
        /// ctx = {itemId, selection} and `params` (a JSON object the caller validated against the item's form). Blocks the
        /// caller until the item has finished or its worker was abandoned, answering each request of the script through
        /// `handler`. A failure counts against the extension (NoteFailure for an error or a bad result; a budget trip, a
        /// memory trip and an abandon fault it at once, FaultNow); the caller undoes what the handler did. The extension
        /// must be Loaded: a faulted one is refused with HostError, so the caller checks first.
        [[nodiscard]] InvokeOutcome Invoke(const MenuItemRef &item, const nlohmann::json &selection,
                                           const nlohmann::json &params, const NestedHandler &handler);
        /// The same for an inspector section's button (FindInspectorAction): its run(ctx, params) runs exactly as a menu
        /// item's does (the same worker, budget, abandon deadline and failure counting), with ctx = {actionId,
        /// componentId, entityId}: the component the client's inspector shows and the object that has it, which the
        /// caller found in the scene and checked is of the section's componentType.
        [[nodiscard]] InvokeOutcome InvokeAction(const MenuItemRef &action, const std::string &componentId,
                                                 const std::string &entityId, const nlohmann::json &params,
                                                 const NestedHandler &handler);

        // ---- Hooks (E-X6) ----

        /// Whether `name` is one of HookNames
        [[nodiscard]] static bool IsHookName(std::string_view name);
        /// Whether a script may send `op` while a hook runs: "log", the Editor.Scene readers and Editor.Assets.List and
        /// ReadText. Every other op (a write, an unknown one) is refused in the worker, before it reaches the host.
        [[nodiscard]] static bool IsReadOnlyOp(std::string_view op);
        /// The Loaded extensions that declared this hook, in the files' sorted order, each as a MenuItemRef with hook true
        /// (a Failed or Faulted extension runs none). Empty for a name that is not a hook.
        [[nodiscard]] std::vector<MenuItemRef> FindHooks(std::string_view hook) const;
        /// Runs the hook's function on a worker, as Invoke runs a menu item (the same worker slot, abandon deadline,
        /// state hand-over and failure counting: an error counts as a failure, a budget trip, a memory trip and an abandon
        /// fault the extension at once), under SandboxDefaults::HookBudget (20 million instructions, 0.5 s) and read-only:
        /// the worker refuses every op but IsReadOnlyOp's before it reaches `handler`, and at most MaxHookNestedCommands
        /// of them are sent. `info` is the hook's argument as JSON (an object; for OnAssetsChanged {added, removed,
        /// modified}, each a list of res:// paths). What the function returns is ignored, except for OnBeforePlay, whose
        /// result may be {warnings = {...}, message = "..."} (and nothing else): then outcome.result is that, shaped as a menu
        /// item's result is. The caller never fails its own work for a hook failure.
        [[nodiscard]] InvokeOutcome InvokeHook(const MenuItemRef &hook, const nlohmann::json &info,
                                               const NestedHandler &handler);

        // ---- The watcher (E-X6) ----

        /// What one ReloadChanged did
        struct WatchOutcome
        {
            /// Files whose content changed and that were loaded again (loaded or failed, as a load decides)
            std::size_t reloaded = 0;
            /// Files that are new since the last load, loaded now
            std::size_t added = 0;
            /// Extensions whose file is gone, dropped now
            std::size_t removed = 0;
            /// Files whose modification time or size moved but whose content hash did not: not reloaded
            std::size_t touched = 0;
            [[nodiscard]] bool Changed() const { return reloaded + added + removed > 0; }
        };
        /// The editor/ watcher's one check (design 5.2: --editor-extensions-watch): finds the files as Load does (the same
        /// walk, so nothing outside editor/ and no link is ever read), compares each one's modification time and size with
        /// what the registry recorded when it loaded the file, and for a file that moved reads it and compares its sha256:
        /// a file whose hash is the one the registry holds is only recorded (touched), a file whose hash changed is
        /// loaded again, alone, in a state of its own, and the others keep their states and their failure counts. A file
        /// that is new is loaded; an extension whose file is gone is dropped. A file that failed to load is not tried
        /// again until it changes (one that failed on an id clash is also tried again when the extension that held the id changes or goes). When anything was loaded again, added or removed the uiRevision moves once and the
        /// changed callback is told (extensionsChanged). Called from the command thread, like Load; blocks for the time the
        /// loads take (each file is bounded as in Load).
        WatchOutcome ReloadChanged();

        /// Disables the extension until the next reload, whatever its failure count (a budget trip, an abandoned worker).
        /// Moves uiRevision and tells the changed callback, as NoteFailure does when it makes one faulted. A no-op for an
        /// unknown id or one that is not Loaded. Returns the state after.
        ExtensionState FaultNow(std::string_view id, std::string_view reason);

        /// Worker threads of every ExtensionHost that are running right now (abandoned ones included)
        [[nodiscard]] static int LiveWorkers();
        /// The wall-clock time the caller waits for one file before abandoning its worker
        [[nodiscard]] static std::uint64_t AbandonMillis();

        // ---- Tests ----

        /// Replaces AbandonMillis() (0 restores the default)
        static void SetAbandonMillisForTest(std::uint64_t millis);
        /// Replaces ReloadBudgetMillis (0 restores the default)
        static void SetReloadBudgetMillisForTest(std::uint64_t millis);
        /// Replaces MaxTotalBytes (0 restores the default)
        static void SetMaxTotalBytesForTest(std::size_t bytes);
        /// The two limits in force now (the constants unless a test replaced them)
        [[nodiscard]] static std::uint64_t ReloadBudget();
        [[nodiscard]] static std::size_t TotalBytesBudget();
        /// Runs at the start of every worker with the file's id, before its state exists: a test blocks here to stand
        /// for a call into C that never returns. An empty function removes it.
        static void SetWorkerHookForTest(std::function<void(const std::string &id)> hook);
        /// Calls `visit` with the id and the Lua state of every extension that kept one, in the registry's order
        void ForEachStateForTest(const std::function<void(const std::string &id, lua_State *state)> &visit) const;
        /// Replaces the invocation budget (0 in both restores the default SandboxDefaults::InvocationBudget): a test runs a
        /// runaway script in milliseconds instead of seconds
        static void SetInvocationBudgetForTest(std::uint64_t instructions, std::uint64_t maxMillis);
        /// Replaces the hook budget (0 in both restores the default SandboxDefaults::HookBudget)
        static void SetHookBudgetForTest(std::uint64_t instructions, std::uint64_t maxMillis);
        /// Replaces InvokeGraceMillis (negative restores the default)
        static void SetInvokeGraceMillisForTest(std::int64_t millis);
        /// Replaces MaxNestedCommands (0 restores the default)
        static void SetMaxNestedCommandsForTest(std::uint32_t limit);
        /// Runs at the start of every invocation's worker with the extension's id, before any Lua: a test blocks here to
        /// stand for a call into C that never returns. An empty function removes it.
        static void SetInvokeHookForTest(std::function<void(const std::string &id)> hook);
        /// The wall-clock time Invoke waits for a worker that is silent before it abandons it (the budget's wall clock plus
        /// the grace)
        [[nodiscard]] static std::uint64_t InvokeAbandonMillis();

    private:
        struct Entry;
        struct Tracker;

        struct FoundFile
        {
            std::string id;
            std::filesystem::path path;
        };

        [[nodiscard]] std::vector<FoundFile> Discover() const;
        /// `memoryLeft` is what of the total budget the states loaded so far in this Load have not used; `deadline` is
        /// when the Load's time budget ends
        [[nodiscard]] Entry LoadOne(const FoundFile &file, std::size_t memoryLeft,
                                    std::chrono::steady_clock::time_point deadline);
        /// What the host logs for an entry: whether it loaded, and the line. Pure formatting of the entry it is given,
        /// so a caller holding the registry's mutex can take it and log after letting go.
        [[nodiscard]] static std::pair<bool, std::string> FormatLogLine(const Entry &entry);
        /// Invoke and InvokeAction: runs the item's or the button's function with `context` as its ctx
        [[nodiscard]] InvokeOutcome InvokeWith(const MenuItemRef &item, nlohmann::json context,
                                               const nlohmann::json &params, const NestedHandler &handler);

        std::filesystem::path _root;
        std::shared_ptr<Tracker> _tracker;

        mutable std::mutex _mutex;
        std::vector<Entry> _entries;
        std::uint32_t _uiRevision = 0;
        bool _stopping = false;
        std::function<void(std::uint32_t)> _changed;
        /// Jobs whose caller gave up, by file id: while one still runs and the file has the same source, it is not
        /// started a second time
        std::unordered_map<std::string, std::pair<std::string, std::weak_ptr<ExtensionLoadJob>>> _abandoned;
    };
}
