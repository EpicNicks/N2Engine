#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

namespace N2Engine::Editor
{
    /// Hands work from the editor server's network thread to the main thread.
    ///
    /// Engine state (scenes, Lua, GLFW/GL, the Logger) is only safe to touch from the main thread, so the
    /// network thread never runs a command itself: it Enqueue()s it and blocks on the returned future, and
    /// the main loop runs everything queued with Drain(). Thread-safe; Drain() must only be called from
    /// the thread that owns the engine.
    class CommandQueue
    {
    public:
        using Response = std::vector<uint8_t>;

        /// Any thread. The future gets the work's result (or its exception) once a Drain() runs it, or a
        /// std::future_error (broken_promise) if the queue is closed before then.
        template <typename Work>
        std::future<Response> Enqueue(Work &&work)
        {
            std::packaged_task<Response()> task(std::forward<Work>(work));
            std::future<Response> result = task.get_future();
            {
                std::lock_guard lock(_mutex);
                if (_closed)
                {
                    return result; // the task is destroyed unrun, which breaks its promise
                }
                _tasks.push_back(std::move(task));
            }
            _available.notify_one();
            return result;
        }

        /// Runs everything queued, on the calling thread. Waits up to maxWait for work to arrive when the
        /// queue is empty, so the main loop can use it in place of its idle sleep. Returns the number run.
        size_t Drain(std::chrono::milliseconds maxWait = std::chrono::milliseconds::zero())
        {
            std::deque<std::packaged_task<Response()>> tasks;
            {
                std::unique_lock lock(_mutex);
                if (maxWait > std::chrono::milliseconds::zero())
                {
                    _available.wait_for(lock, maxWait, [this] { return !_tasks.empty(); });
                }
                tasks.swap(_tasks);
            }

            // Run outside the lock so work can enqueue more work; packaged_task stores any exception
            for (auto &task : tasks)
            {
                task();
            }
            return tasks.size();
        }

        /// Drops everything queued (breaking those promises, which wakes any waiting network thread) and
        /// refuses new work until Reopen().
        void Close()
        {
            std::deque<std::packaged_task<Response()>> dropped;
            {
                std::lock_guard lock(_mutex);
                _closed = true;
                dropped.swap(_tasks);
            }
            _available.notify_all();
        }

        void Reopen()
        {
            std::lock_guard lock(_mutex);
            _closed = false;
        }

        [[nodiscard]] bool IsClosed() const
        {
            std::lock_guard lock(_mutex);
            return _closed;
        }

    private:
        mutable std::mutex _mutex;
        std::condition_variable _available;
        std::deque<std::packaged_task<Response()>> _tasks;
        bool _closed = false;
    };
}
