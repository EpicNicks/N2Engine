#include <gtest/gtest.h>

#include <generator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/scheduling/CoroutineWait.hpp"

using namespace N2Engine;
using namespace N2Engine::Scheduling;

namespace
{
    // The current test frame, shared by every trace (so coroutines started with their own trace
    // record the same frame numbers)
    int g_frame = 0;

    // Records the frame on which each step of a coroutine ran
    struct Trace
    {
        std::vector<int> steps;
        void Mark() { steps.push_back(g_frame); }
    };

    std::generator<ICoroutineWait> NextFrameTwice(std::shared_ptr<Trace> trace)
    {
        trace->Mark();
        co_yield WaitForNextFrame{};
        trace->Mark();
        co_yield WaitForNextFrame{};
        trace->Mark();
    }

    std::generator<ICoroutineWait> WaitTwoFrames(std::shared_ptr<Trace> trace)
    {
        trace->Mark();
        co_yield WaitForFrames{2};
        trace->Mark();
    }

    std::generator<ICoroutineWait> Forever(std::shared_ptr<Trace> trace)
    {
        trace->Mark();
        co_yield WaitForever{};
        trace->Mark();
    }

    std::generator<ICoroutineWait> Seconds(std::shared_ptr<Trace> trace, float seconds)
    {
        trace->Mark();
        co_yield WaitForSeconds{seconds};
        trace->Mark();
    }
}

class CoroutineTest : public ::testing::Test
{
protected:
    Scene *_scene = nullptr;
    GameObject::Ptr _go;
    std::shared_ptr<Trace> _trace = std::make_shared<Trace>();

    void SetUp() override
    {
        SceneManager::AddScene(Scene::Create(std::string("Coroutine_") +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
        _go = GameObject::Create("Runner");
        _scene->AddRootGameObject(_go);
        g_frame = 0;
    }

    // One frame of coroutine updates (Application::Run calls AdvanceCoroutines once per frame)
    void Frame()
    {
        ++g_frame;
        _scene->AdvanceCoroutines();
    }

    void Frames(const int count)
    {
        for (int i = 0; i < count; ++i)
        {
            Frame();
        }
    }
};

TEST_F(CoroutineTest, RunsToFirstYieldThenOneStepPerFrame)
{
    ASSERT_NE(_go->StartCoroutine(NextFrameTwice(_trace)), nullptr);

    Frames(5);

    // Used to be erased before its first step (inverted cleanup), or ended at its first yield
    EXPECT_EQ(_trace->steps, (std::vector<int>{1, 2, 3}));
}

TEST_F(CoroutineTest, WaitForFramesWaitsThatManyFrames)
{
    _go->StartCoroutine(WaitTwoFrames(_trace));

    Frames(5);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1, 3}));
}

TEST_F(CoroutineTest, WaitForeverNeverResumes)
{
    _go->StartCoroutine(Forever(_trace));

    Frames(10);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1}));
}

TEST_F(CoroutineTest, WaitForSecondsKeepsWaitingUntilTimePasses)
{
    // Tests never advance the engine clock (Application owns it), so the frame delta stays zero:
    // a correct WaitForSeconds keeps the coroutine suspended. It used to end the coroutine at the yield.
    Coroutine *routine = _go->StartCoroutine(Seconds(_trace, 0.1f));

    Frames(20);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1}));
    EXPECT_TRUE(_go->StopCoroutine(routine)) << "the coroutine should still be alive, waiting";
}

TEST_F(CoroutineTest, StopCoroutineStopsIt)
{
    Coroutine *routine = _go->StartCoroutine(NextFrameTwice(_trace));
    Frame();

    EXPECT_TRUE(_go->StopCoroutine(routine));
    Frames(3);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1}));
    EXPECT_FALSE(_go->StopCoroutine(routine)) << "already stopped";
}

TEST_F(CoroutineTest, StopAllCoroutinesStopsEveryOne)
{
    auto other = std::make_shared<Trace>();
    _go->StartCoroutine(NextFrameTwice(_trace));
    _go->StartCoroutine(NextFrameTwice(other));
    Frame();

    _go->StopAllCoroutines();
    Frames(3);

    EXPECT_EQ(_trace->steps.size(), 1u);
    EXPECT_EQ(other->steps.size(), 1u);
}

namespace
{
    std::generator<ICoroutineWait> StopsItself(GameObject *go, Coroutine **self, std::shared_ptr<Trace> trace)
    {
        trace->Mark();
        co_yield WaitForNextFrame{};
        go->StopCoroutine(*self); // destroying it here, mid-body, would be use-after-free
        trace->Mark();
        co_yield WaitForNextFrame{};
        trace->Mark();
    }

    std::generator<ICoroutineWait> StartsAnother(GameObject *go, std::shared_ptr<Trace> childTrace)
    {
        go->StartCoroutine(NextFrameTwice(childTrace)); // inserts while the scheduler is iterating
        co_yield WaitForNextFrame{};
    }

    std::generator<ICoroutineWait> Throws(std::shared_ptr<Trace> trace)
    {
        trace->Mark();
        co_yield WaitForNextFrame{};
        throw std::runtime_error("coroutine failure");
    }
}

TEST_F(CoroutineTest, CoroutineCanStopItself)
{
    Coroutine *self = nullptr;
    self = _go->StartCoroutine(StopsItself(_go.get(), &self, _trace));

    Frames(4);

    // The rest of the current step runs; nothing after the next yield
    EXPECT_EQ(_trace->steps, (std::vector<int>{1, 2}));
}

TEST_F(CoroutineTest, CoroutineCanStartAnother)
{
    auto child = std::make_shared<Trace>();
    _go->StartCoroutine(StartsAnother(_go.get(), child));

    Frames(5);

    // Started during frame 1's update, so it runs from frame 2
    EXPECT_EQ(child->steps, (std::vector<int>{2, 3, 4}));
}

TEST_F(CoroutineTest, DeactivatingObjectStopsItsCoroutines)
{
    _go->StartCoroutine(NextFrameTwice(_trace));
    Frame();

    _go->SetActive(false);
    Frame();
    _go->SetActive(true);
    Frames(3);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1})) << "stopped coroutines don't resume on reactivation";
    EXPECT_NE(_go->StartCoroutine(NextFrameTwice(_trace)), nullptr) << "a reactivated object can start new ones";
}

TEST_F(CoroutineTest, InactiveObjectCannotStart)
{
    _go->SetActive(false);
    EXPECT_EQ(_go->StartCoroutine(NextFrameTwice(_trace)), nullptr);
}

TEST_F(CoroutineTest, DestroyedObjectsCoroutinesStop)
{
    _go->StartCoroutine(NextFrameTwice(_trace));
    Frame();

    _go->Destroy();
    _scene->ProcessDestroyed();
    Frames(3);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1}));
}

TEST_F(CoroutineTest, ThrowingCoroutineIsStoppedAndOthersContinue)
{
    auto healthy = std::make_shared<Trace>();
    _go->StartCoroutine(Throws(_trace));
    _go->StartCoroutine(NextFrameTwice(healthy));

    EXPECT_NO_THROW(Frames(4));

    EXPECT_EQ(_trace->steps, (std::vector<int>{1}));
    EXPECT_EQ(healthy->steps, (std::vector<int>{1, 2, 3}));
}

TEST_F(CoroutineTest, ObjectLeavingSceneStopsItsCoroutines)
{
    _go->StartCoroutine(NextFrameTwice(_trace));
    Frame();

    _scene->RemoveRootGameObject(_go);
    Frames(3);

    EXPECT_EQ(_trace->steps, (std::vector<int>{1}));
}
