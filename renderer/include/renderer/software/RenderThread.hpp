#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>

#ifdef N2ENGINE_SINGLE_THREADED

// The single-threaded fallback (the web baseline: N2ENGINE_SINGLE_THREADED, see cmake/N2EngineWeb.cmake): there is no
// second thread, so a submitted frame runs at once on the calling thread. The commands run in submission order and none
// is dropped, exactly the work the threaded class below hands to its thread; by the time SubmitFrame returns the frame
// is done, so WaitForFrame has nothing to wait for. The output is the same: the commands do not depend on which thread
// runs them (the software renderer's goldens prove it, headless).
class RenderThread
{
public:
    RenderThread() = default;
    RenderThread(const RenderThread &) = delete;
    RenderThread &operator=(const RenderThread &) = delete;
    ~RenderThread() = default;

    void Start() { m_running = true; }

    void Stop() { m_running = false; }

    // Dropped when not running (never started, or stopped), like the threaded class
    void SubmitFrame(std::vector<std::function<void()>> commands)
    {
        if (!m_running) return;
        for (auto &cmd : commands) cmd();
    }

    void WaitForFrame() {}

private:
    bool m_running{false};
};

#else

class RenderThread
{
public:
    RenderThread() = default;
    RenderThread(const RenderThread &) = delete;
    RenderThread &operator=(const RenderThread &) = delete;
    // A std::thread destroyed while joinable terminates the program; stop it instead
    ~RenderThread() { Stop(); }

    void Start()
    {
        m_running = true;
        m_thread = std::thread(&RenderThread::ThreadFunc, this);
    }

    void Stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_running = false;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();

        // A frame submitted but never picked up is dropped, so WaitForFrame can't wait for it forever
        {
            std::lock_guard lock(m_mutex);
            m_commands.clear();
            m_busy = false;
        }
        m_cv.notify_all(); // wake anyone still waiting on the dropped frame
    }

    // Called from main thread — queues a frame's worth of work. Dropped when the thread isn't running
    // (never started, or stopped): nothing would ever run it.
    void SubmitFrame(std::vector<std::function<void()>> commands)
    {
        if (!m_running) return;
        std::unique_lock lock(m_mutex);
        // Wait if the render thread is still processing the last frame
        m_cv.wait(lock, [this]{ return !m_busy; });
        m_commands = std::move(commands);
        m_busy = true;
        lock.unlock();
        m_cv.notify_all();
    }

    // Called from main thread — wait for render thread to finish current frame
    void WaitForFrame()
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]{ return !m_busy; });
    }

private:
    void ThreadFunc()
    {
        while (true)
        {
            std::vector<std::function<void()>> commands;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this]{ return !m_running || m_busy; });
                if (!m_running) break;
                commands = std::move(m_commands);
            }

            for (auto& cmd : commands) cmd();

            {
                std::lock_guard lock(m_mutex);
                m_busy = false;
            }
            m_cv.notify_all();
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::function<void()>> m_commands;
    std::atomic<bool> m_running{false};
    bool m_busy{false};
};

#endif // N2ENGINE_SINGLE_THREADED