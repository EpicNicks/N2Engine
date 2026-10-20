#include "engine/scripting/ScriptDeclarationCache.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>

// A build with no threads (the web baseline) has no worker, so it needs neither the thread nor the condition variable
#if !defined(N2ENGINE_SINGLE_THREADED)
#include <condition_variable>
#include <thread>
#endif

#include "engine/Logger.hpp"

namespace N2Engine::Scripting
{
    namespace
    {
        // What every worker thread shares. Made on first use and NEVER destroyed: a worker that is still running when the
        // process exits (abandoned in a long garbage-collection call) must not find it gone.
        struct Workers
        {
            std::mutex mutex;
#if !defined(N2ENGINE_SINGLE_THREADED)
            std::condition_variable changed;
#endif
            int live = 0;
            bool stopping = false;
        };

        Workers &SharedWorkers()
        {
            static Workers *const workers = new Workers();
            return *workers;
        }

        // One evaluation's inputs. On a threaded build it is owned by the caller and its worker through shared_ptrs:
        // whichever lets go last frees it, so an abandoned worker keeps everything it uses alive on its own (a copy of the
        // source, never the cache's)
        struct Job
        {
            std::string source;
            std::string chunkName;
            DeclarationLimits limits;
            std::shared_ptr<const ScriptDeclarationCache::EvaluateFunction> evaluate;

#if !defined(N2ENGINE_SINGLE_THREADED)
            std::mutex mutex;
            std::condition_variable finished;
            bool done = false;
            DeclarationResult result;
            // When its caller gives up on it for good: the deadline and the grace after the worker started
            std::chrono::steady_clock::time_point abandonAt{};
#endif
        };

#if !defined(N2ENGINE_SINGLE_THREADED)
        // Jobs a command's budget cut short, by resource path, with the hash of the text they read: the worker still runs
        // (it keeps its slot), so the next Get of the same text waits for THAT job instead of starting a second worker, and
        // takes its answer when it has one. One stalling text therefore holds one slot however often it is asked. Only
        // callers touch this (a worker never does). Made on first use and never destroyed, like the worker bookkeeping.
        struct CutShortJob
        {
            std::string hash;
            std::shared_ptr<Job> job;
        };
        struct CutShortJobs
        {
            std::mutex mutex;
            std::unordered_map<std::string, CutShortJob> byPath;
        };

        CutShortJobs &SharedCutShortJobs()
        {
            static CutShortJobs *const jobs = new CutShortJobs();
            return *jobs;
        }
#endif

        // The CommandBudget open on this thread, if any: when every evaluation it starts must have answered
        struct ThreadBudget
        {
            bool active = false;
            std::chrono::steady_clock::time_point end{};
        };
        thread_local ThreadBudget t_budget;

        constexpr const char *BudgetSpentText =
            "The evaluation budget for this command is exhausted: the script's declarations are read by a later command";

        std::string TimedOutText(const std::uint64_t deadline)
        {
            return std::format("Reading the script's declarations took longer than {} ms and was abandoned", deadline);
        }

        DeclarationResult Failed(std::string text, const bool timedOut = false)
        {
            DeclarationResult result;
            result.errors.push_back(std::move(text));
            if (timedOut)
            {
                result.budgetExceeded = true;
                result.timedOut = true;
            }
            return result;
        }

        DeclarationResult RunEvaluation(const Job &job)
        {
            try
            {
                if (job.evaluate != nullptr && *job.evaluate)
                {
                    return (*job.evaluate)(job.source, job.chunkName, job.limits);
                }
                return DeclarationEvaluator::Evaluate(job.source, job.chunkName, job.limits);
            }
            catch (...)
            {
                return Failed("The evaluation failed");
            }
        }

        // What a log line may show of a path or a message (control characters replaced)
        std::string Printable(std::string_view text)
        {
            std::string result(text.substr(0, 200));
            for (char &c : result)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20 || byte == 0x7F)
                {
                    c = '?';
                }
            }
            return result;
        }

        // Said once per evaluation (the cache keeps the answer): why a script's declarations are partial or missing
        void LogOutcome(const std::string &path, const DeclarationResult &result)
        {
            constexpr std::size_t MaxLines = 3;
            const std::string shown = Printable(path);
            const std::size_t errorLines = std::min(result.errors.size(), MaxLines);
            for (std::size_t i = 0; i < errorLines; ++i)
            {
                Logger::Warn(std::format("Script {}: {}", shown, Printable(result.errors[i])));
            }
            if (result.unevaluable)
            {
                Logger::Warn(std::format("Script {}: its fields are unknown here: {}", shown,
                                         Printable(result.unevaluableReason)));
            }
            std::size_t fieldLines = 0;
            for (const DeclaredField &field : result.fields)
            {
                if (field.unevaluable && fieldLines++ < MaxLines)
                {
                    Logger::Warn(std::format("Script {}: the field '{}' comes from the engine and has no value here: {}",
                                             shown, Printable(field.name), Printable(field.unevaluableReason)));
                }
            }
        }
    }

    ScriptDeclarationCache &ScriptDeclarationCache::Instance()
    {
        static ScriptDeclarationCache *const instance = new ScriptDeclarationCache();
        return *instance;
    }

    std::string ScriptDeclarationCache::HashSource(const std::string_view source)
    {
        std::uint64_t first = 0xcbf29ce484222325ULL;  // FNV-1a
        std::uint64_t second = 0x9E3779B97F4A7C15ULL; // a different mixing, so the two do not collide together
        for (const char c : source)
        {
            const auto byte = static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            first = (first ^ byte) * 0x100000001B3ULL;
            second = (second + byte + 1) * 0xff51afd7ed558ccdULL;
            second ^= second >> 29;
        }
        return std::format("{:016x}{:016x}{}", first, second, source.size());
    }

    ScriptDeclarationCache::Declaration ScriptDeclarationCache::Peek(const IO::ResourcePath &path,
                                                                     const std::string_view source) const
    {
        const std::string key = path.ToString();
        const std::string hash = HashSource(source);
        const std::lock_guard lock(_mutex);
        const auto found = _entries.find(key);
        return found != _entries.end() && found->second.hash == hash ? found->second.declaration : nullptr;
    }

    ScriptDeclarationCache::CommandBudget::CommandBudget()
        : CommandBudget(std::chrono::milliseconds(ScriptDeclarationCache::Instance().GetCommandBudgetMillis()))
    {
    }

    ScriptDeclarationCache::CommandBudget::CommandBudget(const std::chrono::milliseconds allowance)
        : _hadOuter(t_budget.active), _outerEnd(t_budget.end)
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + allowance;
        t_budget.end = _hadOuter ? std::min(_outerEnd, end) : end;
        t_budget.active = true;
    }

    ScriptDeclarationCache::CommandBudget::~CommandBudget()
    {
        t_budget.active = _hadOuter;
        t_budget.end = _outerEnd;
    }

    ScriptDeclarationCache::Declaration ScriptDeclarationCache::Get(const IO::ResourcePath &path,
                                                                    const std::string_view source)
    {
        bool refused = false;
        return Get(path, source, refused);
    }

    ScriptDeclarationCache::Declaration ScriptDeclarationCache::Get(const IO::ResourcePath &path,
                                                                    const std::string_view source, bool &refused)
    {
        refused = false;
        const std::string key = path.ToString();
        const std::string hash = HashSource(source);
        {
            const std::lock_guard lock(_mutex);
            ++_stats.lookups;
            if (const auto found = _entries.find(key); found != _entries.end() && found->second.hash == hash)
            {
                ++_stats.hits;
                return found->second.declaration;
            }
        }

        bool transient = false;
        Declaration declaration = Evaluate(source, key, transient);
        refused = transient;
        if (transient)
        {
            // A refusal says nothing of the script, and the caller asks again: the caller says it once
            return declaration;
        }
        LogOutcome(key, *declaration);

        {
            const std::lock_guard lock(_mutex);
            if (_entries.size() >= MaxEntries && !_entries.contains(key))
            {
                _entries.clear(); // a project with more scripts than that just evaluates again
            }
            _entries[key] = Entry{hash, declaration};
        }
        return declaration;
    }

    ScriptDeclarationCache::Declaration ScriptDeclarationCache::Evaluate(const std::string_view source,
                                                                         const std::string &chunkName,
                                                                         bool &transient)
    {
        std::uint64_t deadline = DefaultDeadlineMillis;
        auto job = std::make_shared<Job>();
        {
            const std::lock_guard lock(_mutex);
            deadline = _deadlineMillis == 0 ? DefaultDeadlineMillis : _deadlineMillis;
            job->evaluate = _evaluator;
        }
        job->source = std::string(source); // a copy: the worker may outlive the caller's buffer
        job->chunkName = chunkName;
        job->limits.maxMillis = deadline;

        // What is left of the command's budget (no scope open: only the deadline bounds the wait). Spent: refused before
        // anything starts.
        const bool budgeted = t_budget.active;
        const auto budgetEnd = t_budget.end;
        const auto budgetLeft = [&budgetEnd]
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(budgetEnd - std::chrono::steady_clock::now());
        };

#if !defined(N2ENGINE_SINGLE_THREADED)
        // The same text was cut short by an earlier command's budget and its worker may still be running: wait for that
        // one (never a second worker for it), and take its answer if it has one by now, budget or not
        {
            const std::string hash = HashSource(source);
            std::shared_ptr<Job> running;
            {
                CutShortJobs &cutShort = SharedCutShortJobs();
                const std::lock_guard lock(cutShort.mutex);
                if (const auto found = cutShort.byPath.find(chunkName); found != cutShort.byPath.end())
                {
                    if (found->second.hash == hash)
                    {
                        running = found->second.job;
                    }
                    else
                    {
                        cutShort.byPath.erase(found); // another text: that worker ends on its own, its answer is not wanted
                    }
                }
            }
            if (running != nullptr)
            {
                return AwaitCutShort(running, chunkName, budgeted, budgetEnd, transient);
            }
        }
#endif

        if (budgeted && budgetLeft().count() <= 0)
        {
            {
                const std::lock_guard stats(_mutex);
                ++_stats.refused;
                ++_stats.overBudget;
            }
            transient = true;
            return std::make_shared<const DeclarationResult>(Failed(BudgetSpentText));
        }

#if defined(N2ENGINE_SINGLE_THREADED)
        // No threads here: the evaluation runs on this one, bounded by the evaluator's own checks, which are given no
        // more than what is left of the budget
        bool cutByBudget = false;
        if (budgeted)
        {
            const auto leftCount = budgetLeft().count();
            const auto left = static_cast<std::uint64_t>(leftCount > 0 ? leftCount : 1);
            if (left < deadline)
            {
                job->limits.maxMillis = left;
                cutByBudget = true;
            }
        }
        {
            const std::lock_guard lock(_mutex);
            ++_stats.evaluations;
        }
        DeclarationResult result = RunEvaluation(*job);
        if (cutByBudget && result.timedOut)
        {
            // Stopped by the command's budget, not by its own deadline: not the script's answer
            {
                const std::lock_guard lock(_mutex);
                ++_stats.overBudget;
            }
            transient = true;
            return std::make_shared<const DeclarationResult>(Failed(BudgetSpentText));
        }
        return std::make_shared<const DeclarationResult>(std::move(result));
#else
        Workers &workers = SharedWorkers();
        {
            std::unique_lock lock(workers.mutex);
            if (workers.stopping || workers.live >= MaxWorkers)
            {
                const bool stopping = workers.stopping;
                const int live = workers.live;
                lock.unlock();
                {
                    const std::lock_guard stats(_mutex);
                    ++_stats.refused;
                }
                transient = true;
                return std::make_shared<const DeclarationResult>(Failed(
                    stopping ? std::string("The host is shutting down: the script's declarations are not read")
                             : std::format("{} script evaluations are still running (abandoned ones count): the "
                                           "script's declarations are not read now",
                                           live)));
            }
            ++workers.live;
        }

        job->abandonAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline + GraceMillis);
        try
        {
            std::thread([job, &workers]() mutable
            {
                DeclarationResult result = RunEvaluation(*job);
                // The evaluation is over, so the worker stops counting before anyone is told it has an answer: a caller
                // that has its result then finds this worker's slot free. What follows touches only the job, which this
                // thread owns a share of.
                {
                    const std::lock_guard lock(workers.mutex);
                    --workers.live;
                }
                workers.changed.notify_all();
                {
                    const std::lock_guard lock(job->mutex);
                    job->result = std::move(result);
                    job->done = true;
                }
                job->finished.notify_all();
            }).detach();
        }
        catch (...)
        {
            // Whatever failed (no thread to be had, std::bad_alloc for its state), the slot is given back
            {
                const std::lock_guard lock(workers.mutex);
                --workers.live;
            }
            workers.changed.notify_all();
            {
                const std::lock_guard stats(_mutex);
                ++_stats.refused;
            }
            transient = true;
            return std::make_shared<const DeclarationResult>(
                Failed("A thread could not be started to read the script's declarations"));
        }
        {
            const std::lock_guard lock(_mutex);
            ++_stats.evaluations;
        }

        // The deadline and the grace, or what is left of the command's budget if that is less
        auto wait = std::chrono::milliseconds(deadline + GraceMillis);
        bool cutByBudget = false;
        if (budgeted)
        {
            const auto left = budgetLeft();
            if (left < wait)
            {
                wait = left.count() > 0 ? left : std::chrono::milliseconds(0);
                cutByBudget = true;
            }
        }

        std::unique_lock lock(job->mutex);
        if (job->finished.wait_for(lock, wait, [&] { return job->done; }))
        {
            return std::make_shared<const DeclarationResult>(std::move(job->result));
        }
        lock.unlock();

        // Abandoned: the worker still holds the job and frees it when it ends; its result is never read
        {
            const std::lock_guard stats(_mutex);
            ++_stats.abandoned;
            if (cutByBudget)
            {
                ++_stats.overBudget;
            }
        }
        if (cutByBudget)
        {
            // The command ran out of time, not the script: a refusal, so a later command reads it again, from this same
            // job (its worker keeps running and keeps its slot; it is not started twice)
            {
                CutShortJobs &cutShort = SharedCutShortJobs();
                const std::lock_guard jobs(cutShort.mutex);
                if (cutShort.byPath.size() >= MaxEntries && !cutShort.byPath.contains(chunkName))
                {
                    cutShort.byPath.clear();
                }
                cutShort.byPath[chunkName] = CutShortJob{HashSource(source), job};
            }
            transient = true;
            return std::make_shared<const DeclarationResult>(Failed(BudgetSpentText));
        }
        return std::make_shared<const DeclarationResult>(Failed(TimedOutText(deadline), true));
#endif
    }

#if !defined(N2ENGINE_SINGLE_THREADED)
    ScriptDeclarationCache::Declaration ScriptDeclarationCache::AwaitCutShort(
        const std::shared_ptr<void> &runningJob, const std::string &chunkName, const bool budgeted,
        const std::chrono::steady_clock::time_point budgetEnd, bool &transient)
    {
        const auto job = std::static_pointer_cast<Job>(runningJob);
        const auto forget = [&]
        {
            CutShortJobs &cutShort = SharedCutShortJobs();
            const std::lock_guard lock(cutShort.mutex);
            const auto found = cutShort.byPath.find(chunkName);
            if (found != cutShort.byPath.end() && found->second.job == job)
            {
                cutShort.byPath.erase(found);
            }
        };

        // Until the job's own deadline and grace (counted from when its worker started), or the budget's end if sooner
        auto until = job->abandonAt;
        bool cutByBudget = false;
        if (budgeted && budgetEnd < until)
        {
            until = budgetEnd;
            cutByBudget = true;
        }

        std::unique_lock lock(job->mutex);
        if (job->finished.wait_until(lock, until, [&] { return job->done; }))
        {
            // Its answer, late for the command that started it, is the script's answer: recorded as any other
            DeclarationResult result = job->result; // a copy: the job is shared until the entry goes
            lock.unlock();
            forget();
            return std::make_shared<const DeclarationResult>(std::move(result));
        }
        lock.unlock();

        if (cutByBudget)
        {
            // Still running, and this command is out of time too: the entry stays for the next one
            {
                const std::lock_guard stats(_mutex);
                ++_stats.refused;
                ++_stats.overBudget;
            }
            transient = true;
            return std::make_shared<const DeclarationResult>(Failed(BudgetSpentText));
        }
        // Past its own deadline: a timeout, recorded under its hash like one waited out in a single command
        forget();
        return std::make_shared<const DeclarationResult>(Failed(TimedOutText(job->limits.maxMillis), true));
    }
#endif

    void ScriptDeclarationCache::Refresh(const IO::ResourcePath &path, const std::string_view source)
    {
        const std::string key = path.ToString();
        const std::string hash = HashSource(source);
        const std::lock_guard lock(_mutex);
        if (const auto found = _entries.find(key); found != _entries.end() && found->second.hash != hash)
        {
            _entries.erase(found);
        }
    }

    void ScriptDeclarationCache::Forget(const IO::ResourcePath &path)
    {
        const std::lock_guard lock(_mutex);
        _entries.erase(path.ToString());
    }

    void ScriptDeclarationCache::Clear()
    {
        const std::lock_guard lock(_mutex);
        _entries.clear();
    }

    std::size_t ScriptDeclarationCache::EntryCount() const
    {
        const std::lock_guard lock(_mutex);
        return _entries.size();
    }

    void ScriptDeclarationCache::Subscribe(const std::string &moduleName, std::function<void()> callback,
                                           const void *owner)
    {
        const std::lock_guard lock(_subscriptionMutex);
        _subscriptions[moduleName].push_back({owner, std::move(callback)});
    }

    void ScriptDeclarationCache::Unsubscribe(const void *owner)
    {
        if (owner == nullptr)
        {
            return;
        }
        const std::lock_guard lock(_subscriptionMutex);
        for (auto &subscriptions : _subscriptions)
        {
            std::erase_if(subscriptions.second, [owner](const Subscription &entry) { return entry.owner == owner; });
        }
    }

    void ScriptDeclarationCache::NotifyReloaded(const std::string &moduleName)
    {
        std::vector<Subscription> callbacks;
        {
            const std::lock_guard lock(_subscriptionMutex);
            if (const auto found = _subscriptions.find(moduleName); found != _subscriptions.end())
            {
                callbacks = found->second;
            }
        }
        for (const Subscription &entry : callbacks)
        {
            try
            {
                entry.callback();
            }
            catch (const std::exception &e)
            {
                Logger::Error(std::format("Reloading module {} failed: {}", Printable(moduleName), Printable(e.what())));
            }
            catch (...)
            {
                Logger::Error(std::format("Reloading module {} failed", Printable(moduleName)));
            }
        }
    }

    int ScriptDeclarationCache::Shutdown(const std::chrono::milliseconds maxWait)
    {
        Workers &workers = SharedWorkers();
        std::unique_lock lock(workers.mutex);
        workers.stopping = true;
#if !defined(N2ENGINE_SINGLE_THREADED)
        workers.changed.wait_for(lock, maxWait, [&] { return workers.live == 0; });
#else
        (void)maxWait; // no worker exists
#endif
        return workers.live;
    }

    int ScriptDeclarationCache::LiveWorkers() const
    {
        Workers &workers = SharedWorkers();
        const std::lock_guard lock(workers.mutex);
        return workers.live;
    }

    ScriptDeclarationCache::Stats ScriptDeclarationCache::GetStats() const
    {
        const std::lock_guard lock(_mutex);
        return _stats;
    }

    void ScriptDeclarationCache::SetDeadlineMillis(const std::uint64_t millis)
    {
        const std::lock_guard lock(_mutex);
        _deadlineMillis = millis == 0 ? DefaultDeadlineMillis : millis;
    }

    std::uint64_t ScriptDeclarationCache::GetDeadlineMillis() const
    {
        const std::lock_guard lock(_mutex);
        return _deadlineMillis;
    }

    void ScriptDeclarationCache::SetCommandBudgetMillis(const std::uint64_t millis)
    {
        const std::lock_guard lock(_mutex);
        _commandBudgetMillis = millis == 0 ? DefaultCommandBudgetMillis : millis;
    }

    std::uint64_t ScriptDeclarationCache::GetCommandBudgetMillis() const
    {
        const std::lock_guard lock(_mutex);
        return _commandBudgetMillis;
    }

    void ScriptDeclarationCache::SetEvaluatorForTest(EvaluateFunction evaluate)
    {
        const std::lock_guard lock(_mutex);
        _evaluator = evaluate ? std::make_shared<const EvaluateFunction>(std::move(evaluate)) : nullptr;
    }

    void ScriptDeclarationCache::ResetForTest()
    {
        {
            const std::lock_guard lock(_mutex);
            _entries.clear();
            _stats = {};
            _deadlineMillis = DefaultDeadlineMillis;
            _commandBudgetMillis = DefaultCommandBudgetMillis;
            _evaluator = nullptr;
        }
        {
            const std::lock_guard lock(_subscriptionMutex);
            _subscriptions.clear();
        }
#if !defined(N2ENGINE_SINGLE_THREADED)
        {
            CutShortJobs &cutShort = SharedCutShortJobs();
            const std::lock_guard lock(cutShort.mutex);
            cutShort.byPath.clear();
        }
#endif
        Workers &workers = SharedWorkers();
        const std::lock_guard lock(workers.mutex);
        workers.stopping = false;
    }
}
