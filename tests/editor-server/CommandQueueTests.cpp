#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include <editor-server/CommandQueue.hpp>

using namespace N2Engine::Editor;
using namespace std::chrono_literals;

TEST(CommandQueueTest, WorkEnqueuedFromAnotherThreadRunsOnTheDrainingThread)
{
    CommandQueue queue;
    std::thread::id ranOn;
    std::future<CommandQueue::Response> result;

    std::thread producer([&]
    {
        result = queue.Enqueue([&ranOn]
        {
            ranOn = std::this_thread::get_id();
            return CommandQueue::Response{1, 2, 3};
        });
    });
    producer.join();

    // Nothing runs until the owner drains
    EXPECT_EQ(result.wait_for(0ms), std::future_status::timeout);

    EXPECT_EQ(queue.Drain(), 1u);
    EXPECT_EQ(ranOn, std::this_thread::get_id());
    EXPECT_EQ(result.get(), (CommandQueue::Response{1, 2, 3}));
}

TEST(CommandQueueTest, ProducerBlockedOnTheResultIsAnsweredByDrain)
{
    // The editor server's pattern: the network thread waits on the future while the main loop drains
    CommandQueue queue;
    std::atomic<bool> answered{false};
    CommandQueue::Response received;

    std::thread producer([&]
    {
        received = queue.Enqueue([] { return CommandQueue::Response{42}; }).get();
        answered = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!answered && std::chrono::steady_clock::now() < deadline)
    {
        queue.Drain(10ms);
    }
    producer.join();

    ASSERT_TRUE(answered);
    EXPECT_EQ(received, CommandQueue::Response{42});
}

TEST(CommandQueueTest, DrainRunsEverythingInOrder)
{
    CommandQueue queue;
    std::vector<int> order;
    for (int i = 0; i < 3; ++i)
    {
        (void)queue.Enqueue([&order, i]
        {
            order.push_back(i);
            return CommandQueue::Response{};
        });
    }

    EXPECT_EQ(queue.Drain(), 3u);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(queue.Drain(), 0u);
}

TEST(CommandQueueTest, DrainWaitsForWorkUpToTheTimeout)
{
    CommandQueue queue;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(queue.Drain(20ms), 0u);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 15ms);
}

TEST(CommandQueueTest, ExceptionFromWorkReachesTheFuture)
{
    CommandQueue queue;
    auto result = queue.Enqueue([]() -> CommandQueue::Response { throw std::runtime_error("boom"); });

    EXPECT_NO_THROW(queue.Drain());
    EXPECT_THROW(result.get(), std::runtime_error);
}

TEST(CommandQueueTest, CloseBreaksPendingAndLaterWork)
{
    CommandQueue queue;
    bool ran = false;
    auto pending = queue.Enqueue([&ran]
    {
        ran = true;
        return CommandQueue::Response{};
    });

    queue.Close();
    EXPECT_TRUE(queue.IsClosed());
    EXPECT_THROW(pending.get(), std::future_error);

    auto afterClose = queue.Enqueue([] { return CommandQueue::Response{}; });
    EXPECT_THROW(afterClose.get(), std::future_error);

    EXPECT_EQ(queue.Drain(), 0u);
    EXPECT_FALSE(ran);

    queue.Reopen();
    auto reopened = queue.Enqueue([] { return CommandQueue::Response{9}; });
    EXPECT_EQ(queue.Drain(), 1u);
    EXPECT_EQ(reopened.get(), CommandQueue::Response{9});
}

TEST(CommandQueueTest, CloseWakesAProducerWaitingOnItsResult)
{
    CommandQueue queue;
    std::promise<bool> outcome;
    auto outcomeFuture = outcome.get_future();

    std::thread producer([&]
    {
        try
        {
            queue.Enqueue([] { return CommandQueue::Response{}; }).get();
            outcome.set_value(false);
        }
        catch (const std::future_error &)
        {
            outcome.set_value(true);
        }
    });

    // Whether Close lands before or after the enqueue, the producer must wake with broken_promise
    std::this_thread::sleep_for(20ms);
    queue.Close();

    ASSERT_EQ(outcomeFuture.wait_for(10s), std::future_status::ready);
    producer.join();
    EXPECT_TRUE(outcomeFuture.get());
}
