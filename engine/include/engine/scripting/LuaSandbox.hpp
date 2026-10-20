#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace N2Engine::Scripting
{
    /// Which libraries and environment a sandboxed state gets.
    enum class SandboxProfile
    {
        /// What the declaration evaluator (DeclarationEvaluator, Lua edit-host option B) builds its throwaway state
        /// from: the base, math, string and table libraries, in full, plus the budget sandbox (pcall and xpcall hand
        /// the budget error on, setmetatable refuses __gc). The evaluator then removes and replaces what it does not
        /// want with its own environment script; the builder does not.
        Declarations,
        /// The state of a Lua editor extension (design: lua-editor-extensions 6.4). The base library without load,
        /// loadfile, dofile, collectgarbage, print and warn; math; table; utf8; and string without find, match,
        /// gmatch and gsub (pattern matching: their C-level cost is invisible to the instruction budget) and without
        /// dump. No io, os, package, debug or coroutine library, no require, no engine binding and no editor API
        /// (the `Editor` namespace comes with the host that installs it). string.rep of an empty string answers ""
        /// without a C loop. table.move, insert and remove are redone in Lua (stock semantics), so a huge range or a
        /// huge border costs counted instructions and ends as a budget trip; concat, unpack and sort are the stock C
        /// functions (design 6.4), so one over a huge range or length (a table whose `__len` lies, or whose border
        /// is huge) still loops in C, where the hook cannot stop it: the caller's abandon deadline (see LuaSandbox)
        /// is what bounds that.
        EditorExtension,
    };

    /// What one entry into a state (one chunk, later one callback) may spend: VM instructions and wall-clock time.
    /// 0 in either field means the sandbox's load budget value (SandboxOptions::loadBudget), never unlimited.
    struct SandboxBudget
    {
        std::uint64_t instructions = 0;
        std::uint64_t maxMillis = 0;
    };

    /// The editor-extension defaults of the design (decision D7), as named constants.
    namespace SandboxDefaults
    {
        /// Running a file's top-level chunk (and a prelude)
        inline constexpr SandboxBudget LoadBudget{10'000'000, 1000};
        /// One invocation of an extension's menu item or button
        inline constexpr SandboxBudget InvocationBudget{200'000'000, 5000};
        /// One hook call
        inline constexpr SandboxBudget HookBudget{20'000'000, 500};
        /// Bytes one state may hold
        inline constexpr std::size_t MaxBytes = std::size_t{64} * 1024 * 1024;
        /// The prelude each profile runs in a fresh state: empty in E-X1 (the design names no contents; pure-Lua
        /// Vector3 and Quaternion helpers come with the Editor.Scene values, E-X3)
        inline constexpr std::string_view EditorExtensionPrelude{};
        /// The smallest cap a state is given: the libraries and the sandbox take well under it, and a state needs
        /// about 100 KiB to exist at all. A smaller request is raised to this.
        inline constexpr std::size_t MinBytes = std::size_t{1024} * 1024;
        /// Nested commands one invocation may send to the host. Declared here for the host PRs that enforce it
        /// (E-X3); nothing in this builder uses it.
        inline constexpr std::uint32_t MaxNestedCommands = 10'000;
    }

    /// How an entry into a state ended. Every way it can go wrong is data here; none throws.
    struct SandboxOutcome
    {
        bool ok = true;
        /// The instruction budget or the wall clock ended it (also set with timedOut)
        bool budgetExceeded = false;
        /// The wall clock, not the instruction count, ended it
        bool timedOut = false;
        /// The state's memory cap was reached
        bool memoryExhausted = false;
        /// A short, sanitised description (empty when ok). The budget and the memory cap are read from the state's
        /// own counters, not from this text; do not parse it (the error object can be displaced on MSVC Debug and
        /// the web build).
        std::string error;
    };

    struct SandboxOptions
    {
        /// The state's memory cap in bytes; 0 means SandboxDefaults::MaxBytes. Never unlimited, never below
        /// SandboxDefaults::MinBytes.
        std::size_t maxBytes = 0;
        /// The wall-clock and instruction budget the prelude runs under (default: SandboxDefaults::LoadBudget)
        SandboxBudget loadBudget = SandboxDefaults::LoadBudget;
        /// Text of the prelude to run in the fresh state, after the libraries and before Create returns. When unset
        /// the profile's own prelude runs: the empty one for both profiles in E-X1 (nothing runs). Tests set it.
        std::optional<std::string> prelude;
    };

    /// The counters of a sandboxed state; they stay readable after Close.
    struct SandboxCounters
    {
        /// Bytes the state holds now (what was not freed once it is closed: 0 means nothing leaked)
        std::size_t usedBytes = 0;
        /// The most bytes it held at once
        std::size_t peakBytes = 0;
        /// The memory cap in force (0 while lifted for a host's own reading of an answer)
        std::size_t capBytes = 0;
        /// The instruction limit of the entry that last ran
        std::uint64_t instructionLimit = 0;
        /// The last entry ran out of instructions or time (exhausted), and which of the two it was (timedOut)
        bool budgetExhausted = false;
        bool timedOut = false;
    };

    /// A fresh, owned lua_State for code that is not the game's: its own allocator with a byte cap, its own
    /// instruction and wall-clock hook, the libraries of a SandboxProfile and nothing else. It never touches the
    /// game's state (LuaRuntime::GetState()), any engine object or any singleton, so it can be made and used on any
    /// thread, several at once, with or without a LuaRuntime. The builder and its state are not thread safe: one
    /// thread at a time uses one LuaSandbox. The destructor closes the state on every path.
    ///
    /// Not wired into any host feature yet: the declaration evaluator is its one user, and the editor-extension
    /// profile is built only by tests until the host (E-X2) installs it.
    ///
    /// Precondition for every caller: run it where it can be abandoned. The entry's instruction count and clock are
    /// checked by a hook between VM instructions, so a single call into C that does not return cannot be
    /// interrupted. One is known and not fixed here: the collector's atomic phase traverses a table with weak keys
    /// (`__mode = "k"`) again until nothing new is marked, which an estimate from the Lua 5.4.8 source (not measured:
    /// see docs/scripting-lua.html) puts at quadratic time in one C call, with no hook and no allocation to refuse.
    /// Vendored Lua is not patched. ScriptDeclarationCache runs
    /// the evaluator on a worker with an abandon deadline for this reason; a host that runs extension code must do
    /// the same (a worker the command waits on up to a deadline), never on a thread it cannot do without.
    class LuaSandbox
    {
    public:
        /// Builds the state for a profile and runs the prelude (under options.loadBudget). Throws only when the
        /// process is out of memory (the state cannot be allocated at all). A failing prelude is not thrown: it is
        /// in PreludeOutcome, and the sandbox must then not be used.
        [[nodiscard]] static std::unique_ptr<LuaSandbox> Create(SandboxProfile profile,
                                                                 const SandboxOptions &options = {});

        /// Closes the state if still open
        ~LuaSandbox();

        LuaSandbox(const LuaSandbox &) = delete;
        LuaSandbox &operator=(const LuaSandbox &) = delete;
        LuaSandbox(LuaSandbox &&) = delete;
        LuaSandbox &operator=(LuaSandbox &&) = delete;

        [[nodiscard]] SandboxProfile Profile() const;

        /// The state (nullptr after Close). The host's own raw-API code runs on it.
        [[nodiscard]] lua_State *State() const;

        /// How the prelude ended (ok when it was empty and nothing ran)
        [[nodiscard]] const SandboxOutcome &PreludeOutcome() const;

        /// Starts a budget for the entry that follows: from now on the hook counts instructions and checks the clock
        /// until Disarm (or the next Arm, which refills both)
        void Arm(const SandboxBudget &budget);
        /// Removes the hook, so C++ code that calls into the state between entries is not counted
        void Disarm();

        /// Runs a text chunk (a precompiled chunk is refused, as everywhere in the engine) in a protected call under
        /// the budget, then disarms. On success `resultCount` return values are left on the stack (LUA_MULTRET is not
        /// allowed: it is treated as 0); on failure nothing is. The stack must be empty on entry (a raised error that
        /// reached the panic function resets the thread, so values below would be lost).
        [[nodiscard]] SandboxOutcome RunChunk(std::string_view source, const std::string &chunkName,
                                              const SandboxBudget &budget, int resultCount = 0);

        /// The outcome of a failed protected call or load (status from lua_pcall or luaL_loadbufferx, the error
        /// object on top of the stack and left there), read from the state's own counters. `phase` prefixes an
        /// ordinary error.
        [[nodiscard]] SandboxOutcome FailureOutcome(int status, const char *phase);

        [[nodiscard]] SandboxCounters Counters() const;

        /// Closes the state now (idempotent). The counters stay.
        void Close();

        /// States that exist right now (made by Create, not yet closed). 0 whenever every sandbox is closed. For tests.
        [[nodiscard]] static int LiveStates();

    private:
        struct Impl;
        explicit LuaSandbox(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
