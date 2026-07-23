#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>

class RenderThread
{
public:
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
        m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }

    // Called from main thread — queues a frame's worth of work
    void SubmitFrame(std::vector<std::function<void()>> commands)
    {
        std::unique_lock lock(m_mutex);
        // Wait if the render thread is still processing the last frame
        m_cv.wait(lock, [this]{ return !m_busy; });
        m_commands = std::move(commands);
        m_busy = true;
        lock.unlock();
        m_cv.notify_one();
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
            m_cv.notify_one();
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::function<void()>> m_commands;
    std::atomic<bool> m_running{false};
    bool m_busy{false};
};