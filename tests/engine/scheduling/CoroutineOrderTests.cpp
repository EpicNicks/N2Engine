#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <generator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scheduling/Coroutine.hpp"
#include "engine/scheduling/CoroutineWait.hpp"

// Coroutines run in creation order (the order each object's first coroutine started), never in the
// pointer-hash order the scheduler's unordered_map used to give. Backend independent: no physics.

using namespace N2Engine;
using namespace N2Engine::Scheduling;

namespace
{
    std::vector<int> g_order;

    // Records its id once per frame, forever
    std::generator<ICoroutineWait> RecordEveryFrame(const int id)
    {
        while (true)
        {
            g_order.push_back(id);
            co_yield WaitForNextFrame{};
        }
    }

    class CoroutineOrderTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;

        void SetUp() override
        {
            SceneManager::AddScene(Scene::Create(std::string("CoroutineOrder_") +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
            g_order.clear();
        }

        GameObject::Ptr SpawnRunner(const int id)
        {
            const auto go = GameObject::Create("Runner" + std::to_string(id));
            _scene->AddRootGameObject(go);
            go->StartCoroutine(RecordEveryFrame(id));
            return go;
        }

        std::vector<int> Frame()
        {
            g_order.clear();
            _scene->AdvanceCoroutines();
            return g_order;
        }
    };
}

TEST_F(CoroutineOrderTest, RunsInCreationOrder)
{
    std::vector<GameObject::Ptr> objects;
    std::vector<int> expected;
    for (int i = 0; i < 40; ++i)
    {
        objects.push_back(SpawnRunner(i));
        expected.push_back(i);
    }

    EXPECT_EQ(Frame(), expected);
    EXPECT_EQ(Frame(), expected) << "the order must not change from frame to frame";
}

TEST_F(CoroutineOrderTest, SeveralCoroutinesOnOneObjectRunInStartOrderAtTheObjectsPlace)
{
    const auto first = SpawnRunner(0);
    const auto second = SpawnRunner(1);
    first->StartCoroutine(RecordEveryFrame(2)); // a later coroutine on the first object

    // The first object's coroutines are together, in start order, ahead of the second object's
    EXPECT_EQ(Frame(), (std::vector<int>{0, 2, 1}));
}

TEST_F(CoroutineOrderTest, OrderIsIndependentOfAllocationAddressesAcross100Sequences)
{
    constexpr int kObjects = 24;
    constexpr int kSequences = 100;

    // Survivors (ids not 1 mod 3) in creation order, then the replacements, which start last
    std::vector<int> expected;
    for (int i = 0; i < kObjects; ++i)
    {
        if (i % 3 != 1)
        {
            expected.push_back(i);
        }
    }
    for (int i = kObjects; i < kObjects + 4; ++i)
    {
        expected.push_back(i);
    }

    for (int seed = 0; seed < kSequences; ++seed)
    {
        // A fresh scene per sequence, so no state carries over
        SceneManager::AddScene(Scene::Create("CoroutineOrder_Seq" + std::to_string(seed)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();

        std::mt19937 rng(static_cast<std::mt19937::result_type>(seed) * 7919u + 13u);
        std::vector<std::unique_ptr<char[]>> junk; // scrambles the heap so addresses differ per sequence
        const auto scramble = [&]
        {
            const int allocations = static_cast<int>(rng() % 6u);
            for (int a = 0; a < allocations; ++a)
            {
                junk.push_back(std::make_unique<char[]>(16u + rng() % 600u));
            }
            if (!junk.empty() && rng() % 2u == 0u)
            {
                junk.erase(junk.begin() + static_cast<std::ptrdiff_t>(rng() % junk.size()));
            }
        };

        std::vector<GameObject::Ptr> objects;
        for (int i = 0; i < kObjects; ++i)
        {
            scramble();
            objects.push_back(SpawnRunner(i));
        }

        // Destroy some in a seed-dependent order, so freed addresses are reused by the replacements
        std::vector<int> doomed;
        for (int i = 0; i < kObjects; ++i)
        {
            if (i % 3 == 1)
            {
                doomed.push_back(i);
            }
        }
        std::shuffle(doomed.begin(), doomed.end(), rng);
        for (const int id : doomed)
        {
            scramble();
            objects[static_cast<std::size_t>(id)]->Destroy();
        }
        _scene->ProcessDestroyed();
        for (const int id : doomed)
        {
            objects[static_cast<std::size_t>(id)].reset();
        }

        for (int i = kObjects; i < kObjects + 4; ++i)
        {
            scramble();
            objects.push_back(SpawnRunner(i));
        }

        EXPECT_EQ(Frame(), expected) << "sequence " << seed;
        EXPECT_EQ(Frame(), expected) << "sequence " << seed << ", second frame";
    }
}

TEST_F(CoroutineOrderTest, StoppingAndRestartingMovesAnObjectToTheEnd)
{
    const auto a = SpawnRunner(0);
    const auto b = SpawnRunner(1);
    const auto c = SpawnRunner(2);
    ASSERT_EQ(Frame(), (std::vector<int>{0, 1, 2}));

    a->StopAllCoroutines();
    a->StartCoroutine(RecordEveryFrame(0)); // a fresh start: it takes a new place at the end

    EXPECT_EQ(Frame(), (std::vector<int>{1, 2, 0}));
}
