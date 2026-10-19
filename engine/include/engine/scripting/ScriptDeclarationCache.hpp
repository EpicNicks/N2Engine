#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/io/ResourcePath.hpp"
#include "engine/scripting/DeclarationEvaluator.hpp"

namespace N2Engine::Scripting
{
    /// What an editor host that never runs project Lua (LuaExecutionPolicy::DeclarationsOnly) knows of a script: the
    /// DeclarationEvaluator's reading of it, kept per resource path and keyed by a hash of the SOURCE TEXT, never the
    /// file's time (a write inside the file system's time granularity must not serve an old declaration).
    ///
    /// The evaluation is never run on the thread that asks. Lua's garbage collector can spend quadratic time inside one C
    /// call (weak-key tables) that neither the instruction hook nor the allocator cap can interrupt, which would stall a
    /// host that runs commands on one thread. So each evaluation runs on a worker thread under a wall-clock deadline;
    /// the caller waits at most that long and, past it, ABANDONS the call and answers a failed declaration (a timeout):
    ///   - the worker owns everything it touches (a copy of the source, its own throwaway Lua state, a shared result
    ///     slot); after abandonment it never reads or writes the cache or any host data, and its result is dropped;
    ///   - the cache records the timeout under that source's hash, so the same text is not evaluated again in a loop
    ///     (an edit changes the hash, which is a new attempt);
    ///   - at most MaxWorkers workers exist at a time, abandoned ones included; when they are all busy a further
    ///     evaluation answers a failed declaration at once and is not recorded (a later attempt may succeed);
    ///   - Shutdown stops new evaluations and waits a bounded time for the running workers. A thread cannot be
    ///     interrupted, so one still running after that is left (detached) and the process may exit: everything it
    ///     touches is heap state that is never destroyed (the cache itself and the worker bookkeeping are leaked on
    ///     purpose), so it cannot touch a destroyed object, and it holds no host data.
    ///
    /// A build with no threads (the default web artifact, N2ENGINE_SINGLE_THREADED) evaluates on the calling thread: there
    /// is nothing to abandon, only the evaluator's own wall-clock check bounds it. The edit host is not a web target.
    ///
    /// Thread-safe: any method may be called from any thread. The singleton is made on first use and never destroyed.
    class ScriptDeclarationCache
    {
    public:
        using Declaration = std::shared_ptr<const DeclarationResult>;
        /// What an evaluation runs: the DeclarationEvaluator by default; a test swaps it to block or count
        using EvaluateFunction =
            std::function<DeclarationResult(std::string_view source, const std::string &chunkName,
                                            const DeclarationLimits &limits)>;

        /// The wall-clock time one evaluation may take before the caller abandons it (the evaluator's own time limit is
        /// this too, so a script that merely runs long is stopped by the evaluator first and reports a timeout)
        static constexpr std::uint64_t DefaultDeadlineMillis = 2000;
        /// How long past the deadline the caller waits, so an evaluator that stops on its own at the deadline gets to
        /// report it before the call is abandoned
        static constexpr std::uint64_t GraceMillis = 250;
        /// Worker threads that may exist at once, abandoned ones included
        static constexpr int MaxWorkers = 4;
        /// Scripts kept; a cache that would grow past this starts over
        static constexpr std::size_t MaxEntries = 1024;
        /// How long Shutdown waits for running workers by default
        static constexpr std::chrono::milliseconds DefaultShutdownWait{1000};

        /// What happened since the last ResetForTest (or the start of the process)
        struct Stats
        {
            std::uint64_t lookups = 0;
            std::uint64_t hits = 0;
            /// Evaluations started (a miss that got a worker or ran inline)
            std::uint64_t evaluations = 0;
            /// Evaluations whose caller gave up at the deadline
            std::uint64_t abandoned = 0;
            /// Evaluations refused with no worker free, or after Shutdown
            std::uint64_t refused = 0;
        };

        [[nodiscard]] static ScriptDeclarationCache &Instance();

        /// The declarations of `source`, the text of the script at `path`: from the cache when this path's entry has the
        /// same source hash, else evaluated (off this thread) and kept. Never null; every failure is data in the result
        /// (declared false, errors saying why, timedOut set for an abandoned one). Blocks at most the deadline plus the
        /// grace.
        [[nodiscard]] Declaration Get(const IO::ResourcePath &path, std::string_view source);

        /// The cache's entry for this path and source if there is one, without evaluating (null otherwise)
        [[nodiscard]] Declaration Peek(const IO::ResourcePath &path, std::string_view source) const;

        /// The script at `path` now has this source: entries of the path with another source's hash are dropped (the
        /// entry for this one stays, and is not made here: the next Get does, when something needs it)
        void Refresh(const IO::ResourcePath &path, std::string_view source);
        /// Drops everything kept for a path
        void Forget(const IO::ResourcePath &path);
        /// Drops every entry
        void Clear();
        [[nodiscard]] std::size_t EntryCount() const;

        // ---- Components that read their declarations from here are told when a script is reloaded ----

        /// `callback` runs on NotifyReloaded(moduleName); `owner` identifies the registrant for Unsubscribe (may be null)
        void Subscribe(const std::string &moduleName, std::function<void()> callback, const void *owner);
        /// Removes every subscription of the owner, for any module
        void Unsubscribe(const void *owner);
        /// Runs the module's callbacks (a copy of the list: a callback may subscribe or unsubscribe)
        void NotifyReloaded(const std::string &moduleName);

        // ---- Shutdown ----

        /// Refuses new evaluations from now on and waits up to `maxWait` for the running workers (abandoned ones too).
        /// Returns how many were still running when it gave up (0: every worker has finished). Idempotent; call it again
        /// to wait for more.
        int Shutdown(std::chrono::milliseconds maxWait = DefaultShutdownWait);
        /// Worker threads running right now (abandoned ones included)
        [[nodiscard]] int LiveWorkers() const;

        // ---- Tests ----

        [[nodiscard]] Stats GetStats() const;
        /// The deadline the next evaluation uses (0 restores the default)
        void SetDeadlineMillis(std::uint64_t millis);
        [[nodiscard]] std::uint64_t GetDeadlineMillis() const;
        /// Replaces what an evaluation runs (an empty function restores the DeclarationEvaluator)
        void SetEvaluatorForTest(EvaluateFunction evaluate);
        /// Empties the cache and the subscriptions, zeroes the stats, restores the deadline and the evaluator and allows
        /// evaluations again (undoing Shutdown). Does not stop running workers.
        void ResetForTest();

        /// The content hash a source is kept under: two independent 64-bit hashes and the length
        [[nodiscard]] static std::string HashSource(std::string_view source);

    private:
        ScriptDeclarationCache() = default;
        ~ScriptDeclarationCache() = default;

        struct Entry
        {
            std::string hash;
            Declaration declaration;
        };
        struct Subscription
        {
            const void *owner;
            std::function<void()> callback;
        };

        /// Runs one evaluation off this thread (or inline on a build with no threads); `transient` is set when the answer
        /// is a refusal that must not be remembered
        Declaration Evaluate(std::string_view source, const std::string &chunkName, bool &transient);

        mutable std::mutex _mutex;
        std::unordered_map<std::string, Entry> _entries; // by resource path text
        std::uint64_t _deadlineMillis = DefaultDeadlineMillis;
        std::shared_ptr<const EvaluateFunction> _evaluator;
        Stats _stats;

        std::mutex _subscriptionMutex;
        std::unordered_map<std::string, std::vector<Subscription>> _subscriptions;
    };
}
