#pragma once

// Shared by the two test programs of the world-protocol loopback test (tests/smoke/n2server_net.py): the test host
// (NetHost.cpp, N2Server's RunServer with hooks) and the test client (NetClient.cpp, a headless NetworkClient).

#include <atomic>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "engine/GameObjectScene.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/prefabs/PrefabManager.hpp"

namespace N2Engine::NetTest
{
    /// The prefab both programs register: one object with one NetworkObject on its root. Both must register the same
    /// prefabs, or the fingerprint check refuses the join.
    inline constexpr const char *kCratePrefab = "Crate";

    inline bool RegisterCratePrefab()
    {
        const GameObject::Ptr root = GameObject::Create("Crate");
        root->AddComponent<Network::NetworkObject>();
        return PrefabManager::RegisterPrefab(kCratePrefab, root);
    }

    /// A line without its end of line characters
    inline std::string StripLineEnd(std::string line)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.pop_back();
        }
        return line;
    }

    /// Lines read from stdin by a thread of their own, taken by the program's main thread
    class CommandQueue
    {
    public:
        void Push(std::string line)
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            _lines.push_back(std::move(line));
        }

        bool Pop(std::string &line)
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            if (_lines.empty())
            {
                return false;
            }
            line = std::move(_lines.front());
            _lines.pop_front();
            return true;
        }

        void Close() { _closed = true; }
        [[nodiscard]] bool Closed() const { return _closed; }

        /// Starts a detached thread that pushes every line of stdin and then closes the queue. The queue must outlive the
        /// process's end (make it static): the thread may still be blocked reading when main returns.
        void StartReadingStdin()
        {
            std::thread reader(
                [this]
                {
                    std::string line;
                    while (std::getline(std::cin, line))
                    {
                        Push(StripLineEnd(line));
                    }
                    Close();
                });
            reader.detach();
        }

    private:
        std::mutex _mutex;
        std::deque<std::string> _lines;
        std::atomic<bool> _closed{false};
    };
}
