#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;

namespace
{
    // Two distinct component types, so first-instance lookup can be checked per type
    class Marker : public Component
    {
    public:
        using Component::Component;
        [[nodiscard]] std::string GetTypeName() const override { return "Marker"; }
        int id = 0;
    };

    Scene *MakeScene(const std::string &name)
    {
        SceneManager::AddScene(Scene::Create(name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return SceneManager::GetCurScene();
    }

    GameObject::Ptr MakeObject(const std::string &name)
    {
        auto go = GameObject::Create(name);
        go->CreatePositionable();
        return go;
    }
}

// ----------------------------------------------------------------------------- hierarchy

TEST(CoreRobustnessTest, ParentingARootRemovesItFromTheRoots)
{
    Scene *scene = MakeScene("Core_RootReparent");
    const auto parent = MakeObject("Parent");
    const auto child = MakeObject("Child");
    scene->AddRootGameObjects({parent, child});
    ASSERT_EQ(scene->GetRootGameObjects().size(), 2u);

    parent->AddChild(child);

    // It used to stay a root too, so it was updated, rendered and serialized twice
    ASSERT_EQ(scene->GetRootGameObjects().size(), 1u);
    EXPECT_EQ(scene->GetRootGameObjects()[0], parent);
    EXPECT_EQ(child->GetParent(), parent);
}

TEST(CoreRobustnessTest, ParentingAnAncestorUnderItsDescendantIsRefused)
{
    const auto grandparent = MakeObject("Grandparent");
    const auto parent = MakeObject("Parent");
    const auto child = MakeObject("Child");
    grandparent->AddChild(parent);
    parent->AddChild(child);

    child->AddChild(grandparent); // would make a cycle

    EXPECT_EQ(grandparent->GetParent(), nullptr);
    EXPECT_TRUE(child->GetChildren().empty());
    EXPECT_EQ(child->GetParent(), parent);
}

// ----------------------------------------------------------------------------- components

TEST(CoreRobustnessTest, GetComponentReturnsTheFirstOfAType)
{
    const auto go = MakeObject("Markers");
    auto *first = go->AddComponent<Marker>();
    first->id = 1;
    auto *second = go->AddComponent<Marker>();
    second->id = 2;

    // Unity's rule; the newest used to win, leaving the first unreachable by GetComponent
    EXPECT_EQ(go->GetComponent<Marker>(), first);
}

TEST(CoreRobustnessTest, RemovingTheFirstComponentExposesTheNext)
{
    const auto go = MakeObject("Markers");
    auto *first = go->AddComponent<Marker>();
    auto *second = go->AddComponent<Marker>();

    ASSERT_TRUE(go->RemoveComponent(first));

    // The type used to vanish from lookup while a component of it was still attached
    EXPECT_EQ(go->GetComponent<Marker>(), second);
    ASSERT_TRUE(go->RemoveComponent(second));
    EXPECT_EQ(go->GetComponent<Marker>(), nullptr);
}

// ----------------------------------------------------------------------------- transforms

TEST(CoreRobustnessTest, SetScaleUnderAZeroParentAxisStaysFinite)
{
    const auto parent = MakeObject("Parent");
    const auto child = MakeObject("Child");
    parent->AddChild(child);
    parent->GetPositionable()->SetLocalScale(Math::Vector3(0.0f, 2.0f, 1.0f));

    child->GetPositionable()->SetScale(Math::Vector3(3.0f, 4.0f, 5.0f));

    const Math::Vector3 local = child->GetPositionable()->GetLocalScale();
    EXPECT_TRUE(std::isfinite(local.x)) << "divided by the zero parent axis";
    EXPECT_FLOAT_EQ(local.x, 1.0f); // unchanged on the collapsed axis
    EXPECT_FLOAT_EQ(local.y, 2.0f);
    EXPECT_FLOAT_EQ(local.z, 5.0f);
}

// ----------------------------------------------------------------------------- logger

TEST(CoreRobustnessTest, LoggerDeliversBacklogOnceWhenASubscriberLogs)
{
    if (Logger::logEvent.GetSubscriberCount() != 0)
    {
        GTEST_SKIP() << "another subscriber exists, so nothing is queued";
    }

    const bool previousBroadcast = Logger::broadcastUnbroadcastLogs;
    Logger::broadcastUnbroadcastLogs = true;
    Logger::Info("first");
    Logger::Info("second");

    std::vector<std::string> received;
    bool loggedFromHandler = false;
    const size_t id = Logger::logEvent += [&](std::string_view message, Logger::LogLevel)
    {
        received.emplace_back(message);
        if (!loggedFromHandler)
        {
            loggedFromHandler = true;
            Logger::Info("nested"); // re-enters Log while the backlog is being delivered
        }
    };

    Logger::Info("trigger");

    Logger::logEvent -= id;
    Logger::broadcastUnbroadcastLogs = previousBroadcast;

    // Each exactly once; the nested drain used to deliver "first" twice
    const std::vector<std::string> expected{"first", "nested", "second", "trigger"};
    EXPECT_EQ(received, expected);
}

// ----------------------------------------------------------------------------- application

TEST(CoreRobustnessTest, QuitIsARequestNotAnExit)
{
    // Used to std::exit(0) on the spot, which would end this test process
    Application::Quit();

    EXPECT_TRUE(Application::GetInstance().IsQuitRequested());
}
