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
     * What this PR has of the `Editor` namespace is `Editor.Extension` and nothing else: no invocation, no
     * `Editor.Scene` (E-X3). The state a loaded file keeps is therefore idle until then.
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
     * Calls are made from the command thread (Load, Reload, the readers); the registry is also guarded by a mutex, so a
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
        /// GetEditorUi's answer: {"uiRevision", "menuItems": [...], "inspectorSections": []}. The items of every loaded or
        /// faulted extension, in the files' sorted order then as declared. Reads the registry only.
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
        /// The state of the extension with this id; Failed for an unknown one
        [[nodiscard]] ExtensionState StateOf(std::string_view id) const;

        /// Stops new evaluations and waits up to `maxWait` for this host's running workers (abandoned ones too).
        /// Returns how many are still running when it gives up (0: all finished). Idempotent.
        int Shutdown(std::chrono::milliseconds maxWait = DefaultShutdownWait);

        /// The sha256 of `data` as 64 lowercase hex digits (what the registry shows for a source)
        [[nodiscard]] static std::string Sha256Hex(std::string_view data);

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
