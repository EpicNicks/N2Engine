#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

namespace N2Engine::Tests
{
    /// Ends the test program if the current test has not finished within a time limit. For tests that run a loop
    /// only something under test stops (a Lua `while true do end` that the instruction budget must abort): if that
    /// is broken, the program names the test and exits with a failure instead of spinning until the CI job's limit.
    /// Create it first in SetUp and destroy it last in TearDown.
    class HangWatchdog
    {
        std::mutex _mutex;
        std::condition_variable _finished;
        bool _done = false;
        std::thread _thread; // last: it uses the members above

    public:
        explicit HangWatchdog(const std::chrono::seconds limit = std::chrono::seconds(60))
        {
            const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
            std::string name = info != nullptr
                                   ? std::string(info->test_suite_name()) + "." + info->name()
                                   : std::string("(no test)");
            _thread = std::thread([this, limit, name = std::move(name)]
            {
                std::unique_lock lock(_mutex);
                if (!_finished.wait_for(lock, limit, [this] { return _done; }))
                {
                    std::fflush(stdout);
                    std::fprintf(stderr, "\n[ HANG ] %s did not finish within %lld s; ending the test program\n",
                                 name.c_str(), static_cast<long long>(limit.count()));
                    std::fflush(stderr);
                    std::_Exit(3);
                }
            });
        }

        ~HangWatchdog()
        {
            {
                const std::lock_guard lock(_mutex);
                _done = true;
            }
            _finished.notify_all();
            _thread.join();
        }

        HangWatchdog(const HangWatchdog &) = delete;
        HangWatchdog &operator=(const HangWatchdog &) = delete;
    };
}
