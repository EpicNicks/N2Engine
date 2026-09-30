#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;

namespace
{
    // Lifecycle counts live outside the component so they can be read after it's freed
    struct Counts
    {
        int attach = 0;
        int update = 0;
        int destroy = 0;
        int enable = 0;
        int disable = 0;
        std::vector<std::string> teardown; // order of OnDisable/OnDestroy
    };

    class Probe : public Component
    {
    public:
        explicit Probe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "Probe"; }

        void OnAttach() override { ++counts->attach; }
        void OnUpdate() override
        {
            ++counts->update;
            if (onUpdate)
            {
                onUpdate();
            }
        }
        void OnDestroy() override
        {
            ++counts->destroy;
            counts->teardown.emplace_back("destroy");
        }
        void OnEnable() override { ++counts->enable; }
        void OnDisable() override
        {
            ++counts->disable;
            counts->teardown.emplace_back("disable");
        }

        std::shared_ptr<Counts> counts = std::make_shared<Counts>();
        std::function<void()> onUpdate;
    };

    // A second type, since RemoveComponent<T> works by type
    class OtherProbe final : public Probe
    {
    public:
        using Probe::Probe;
        [[nodiscard]] std::string GetTypeName() const override { return "OtherProbe"; }
    };

    Scene &LoadFreshScene(const std::string &name)
    {
        SceneManager::AddScene(Scene::Create(name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return SceneManager::GetCurSceneRef();
    }

    // What Application::Run does each frame, minus physics and rendering
    void Frame(Scene &scene)
    {
        scene.ProcessAttachQueue();
        scene.Update();
        scene.LateUpdate();
        scene.ProcessDestroyed();
    }
}

// ============================================================================
// Destroy
// ============================================================================

TEST(LifecycleTest, DestroyRemovesObjectAtEndOfFrame)
{
    Scene &scene = LoadFreshScene("Lifecycle_Destroy");
    const auto go = GameObject::Create("Doomed");
    const auto counts = go->AddComponent<Probe>()->counts;
    scene.AddRootGameObject(go);

    Frame(scene);
    ASSERT_EQ(counts->attach, 1);
    ASSERT_EQ(counts->update, 1);

    go->Destroy();
    EXPECT_NE(scene.FindGameObject("Doomed"), nullptr) << "destroy is deferred to ProcessDestroyed";

    scene.ProcessDestroyed();
    EXPECT_EQ(counts->destroy, 1);
    EXPECT_TRUE(go->IsDestroyed());
    EXPECT_EQ(scene.FindGameObject("Doomed"), nullptr);
    EXPECT_TRUE(scene.Serialize()["rootGameObjects"].empty());
    EXPECT_TRUE(scene.FindObjectsByType<Probe>().empty());

    Frame(scene);
    EXPECT_EQ(counts->update, 1) << "a destroyed component must not keep updating";
}

TEST(LifecycleTest, DestroyTwiceRunsOnDestroyOnce)
{
    Scene &scene = LoadFreshScene("Lifecycle_DestroyTwice");
    const auto go = GameObject::Create("DoomedTwice");
    const auto counts = go->AddComponent<Probe>()->counts;
    scene.AddRootGameObject(go);
    Frame(scene);

    go->Destroy();
    go->Destroy();
    scene.ProcessDestroyed();
    scene.ProcessDestroyed();

    EXPECT_EQ(counts->destroy, 1);
}

TEST(LifecycleTest, DestroyingParentDestroysChildrenOnce)
{
    Scene &scene = LoadFreshScene("Lifecycle_DestroyHierarchy");
    const auto parent = GameObject::Create("Parent");
    const auto child = GameObject::Create("Child");
    const auto parentCounts = parent->AddComponent<Probe>()->counts;
    const auto childCounts = child->AddComponent<Probe>()->counts;
    parent->AddChild(child);
    scene.AddRootGameObject(parent);
    Frame(scene);

    parent->Destroy();
    scene.ProcessDestroyed();

    EXPECT_EQ(parentCounts->destroy, 1);
    EXPECT_EQ(childCounts->destroy, 1);
    EXPECT_TRUE(child->IsDestroyed());
    EXPECT_TRUE(scene.FindObjectsByType<Probe>().empty());
}

TEST(LifecycleTest, DestroyRunsOnDisableThenOnDestroy)
{
    Scene &scene = LoadFreshScene("Lifecycle_DisableBeforeDestroy");
    const auto go = GameObject::Create("Ordered");
    const auto counts = go->AddComponent<Probe>()->counts;
    scene.AddRootGameObject(go);
    Frame(scene);

    go->Destroy();
    scene.ProcessDestroyed();

    EXPECT_EQ(counts->teardown, (std::vector<std::string>{"disable", "destroy"}));
}

TEST(LifecycleTest, DestroyOfInactiveOrDisabledSkipsOnDisable)
{
    Scene &scene = LoadFreshScene("Lifecycle_NoDisableWhenInactive");
    const auto inactive = GameObject::Create("InactiveObject");
    const auto disabled = GameObject::Create("DisabledComponent");
    const auto inactiveCounts = inactive->AddComponent<Probe>()->counts;
    auto *disabledProbe = disabled->AddComponent<Probe>();
    const auto disabledCounts = disabledProbe->counts;
    scene.AddRootGameObjects({inactive, disabled});
    Frame(scene);

    inactive->SetActive(false); // OnDisable here, not again on destroy
    disabledProbe->SetActive(false);
    inactive->Destroy();
    disabled->Destroy();
    scene.ProcessDestroyed();

    EXPECT_EQ(inactiveCounts->teardown, (std::vector<std::string>{"disable", "destroy"}));
    EXPECT_EQ(disabledCounts->teardown, (std::vector<std::string>{"destroy"}));
}

TEST(LifecycleTest, RemoveComponentRunsOnDisableThenOnDestroy)
{
    Scene &scene = LoadFreshScene("Lifecycle_RemoveOrder");
    const auto go = GameObject::Create("RemoveOrdered");
    const auto counts = go->AddComponent<Probe>()->counts;
    scene.AddRootGameObject(go);
    Frame(scene);

    go->RemoveComponent<Probe>();

    EXPECT_EQ(counts->teardown, (std::vector<std::string>{"disable", "destroy"}));
}

TEST(LifecycleTest, DestroyWithoutSceneMarksDestroyed)
{
    const auto go = GameObject::Create("Sceneless");
    go->Destroy();
    EXPECT_TRUE(go->IsDestroyed());
}

// ============================================================================
// RemoveComponent
// ============================================================================

TEST(LifecycleTest, RemoveComponentDetachesFromScene)
{
    Scene &scene = LoadFreshScene("Lifecycle_RemoveComponent");
    const auto go = GameObject::Create("Remover");
    const auto counts = go->AddComponent<Probe>()->counts;
    scene.AddRootGameObject(go);
    Frame(scene);

    ASSERT_TRUE(go->RemoveComponent<Probe>());

    EXPECT_EQ(counts->destroy, 1);
    EXPECT_TRUE(scene.FindObjectsByType<Probe>().empty()) << "the scene still lists the freed component";
    Frame(scene);
    EXPECT_EQ(counts->update, 1);
}

TEST(LifecycleTest, RemoveComponentBeforeAttachLeavesNothingQueued)
{
    Scene &scene = LoadFreshScene("Lifecycle_RemoveBeforeAttach");
    const auto go = GameObject::Create("RemoverEarly");
    scene.AddRootGameObject(go);
    const auto counts = go->AddComponent<Probe>()->counts; // queued, not attached yet

    ASSERT_TRUE(go->RemoveComponent<Probe>());
    Frame(scene); // would call OnAttach on the freed component

    EXPECT_EQ(counts->attach, 0);
    EXPECT_EQ(counts->update, 0);
}

TEST(LifecycleTest, RemoveComponentDuringUpdateSkipsIt)
{
    Scene &scene = LoadFreshScene("Lifecycle_RemoveDuringUpdate");
    const auto go = GameObject::Create("SelfEditing");
    auto *remover = go->AddComponent<Probe>();
    const auto removedCounts = go->AddComponent<OtherProbe>()->counts;
    remover->onUpdate = [&go] { go->RemoveComponent<OtherProbe>(); };
    scene.AddRootGameObject(go);
    scene.ProcessAttachQueue();

    scene.Update(); // the first component removes the second mid-iteration
    scene.Update();

    EXPECT_EQ(remover->counts->update, 2);
    EXPECT_EQ(removedCounts->update, 0);
    EXPECT_EQ(removedCounts->destroy, 1);
}

TEST(LifecycleTest, ThrowingComponentDoesNotWedgeIteration)
{
    Scene &scene = LoadFreshScene("Lifecycle_Throw");
    const auto go = GameObject::Create("Thrower");
    auto *thrower = go->AddComponent<Probe>();
    const auto otherCounts = go->AddComponent<OtherProbe>()->counts;
    scene.AddRootGameObject(go);
    scene.ProcessAttachQueue();

    thrower->onUpdate = [] { throw std::runtime_error("component failure"); };
    EXPECT_THROW(scene.Update(), std::runtime_error);

    // The iteration scope unwound: removal and later frames behave normally
    thrower->onUpdate = nullptr;
    go->RemoveComponent<OtherProbe>();
    scene.Update();

    EXPECT_EQ(otherCounts->destroy, 1);
    EXPECT_EQ(thrower->counts->update, 2);
    EXPECT_TRUE(scene.FindObjectsByType<OtherProbe>().empty());
}

TEST(LifecycleTest, DestroyingObjectRemovesItsLight)
{
    Scene &scene = LoadFreshScene("Lifecycle_Light");
    const auto go = GameObject::Create("Lamp");
    go->AddComponent<Rendering::Light>();
    scene.AddRootGameObject(go);
    Frame(scene);
    ASSERT_NE(scene.FindObjectByType<Rendering::Light>(), nullptr);

    go->Destroy();
    scene.ProcessDestroyed();

    EXPECT_EQ(scene.FindObjectByType<Rendering::Light>(), nullptr);
    EXPECT_NO_FATAL_FAILURE((void)scene.CollectLighting()); // used to read the freed light
}

// ============================================================================
// Scene switch
// ============================================================================

TEST(LifecycleTest, SceneSwitchRunsOnDestroy)
{
    Scene &scene = LoadFreshScene("Lifecycle_SwitchFrom");
    const auto go = GameObject::Create("LeftBehind");
    const auto child = GameObject::Create("LeftBehindChild");
    const auto counts = go->AddComponent<Probe>()->counts;
    const auto childCounts = child->AddComponent<Probe>()->counts;
    go->AddChild(child);
    scene.AddRootGameObject(go);
    Frame(scene);

    LoadFreshScene("Lifecycle_SwitchTo");

    EXPECT_EQ(counts->destroy, 1) << "switching scenes must release what components hold";
    EXPECT_EQ(childCounts->destroy, 1);
    EXPECT_TRUE(go->IsDestroyed());
    EXPECT_EQ(go->GetScene(), nullptr);
}

TEST(LifecycleTest, ReparentingWithinSceneDoesNotAttachTwice)
{
    Scene &scene = LoadFreshScene("Lifecycle_Reparent");
    const auto first = GameObject::Create("FirstParent");
    const auto second = GameObject::Create("SecondParent");
    const auto child = GameObject::Create("MovingChild");
    const auto counts = child->AddComponent<Probe>()->counts;
    first->AddChild(child);
    scene.AddRootGameObjects({first, second});
    Frame(scene);

    second->AddChild(child);
    Frame(scene);

    EXPECT_EQ(counts->attach, 1);
    EXPECT_EQ(counts->update, 2) << "one update per frame, not one per registration";
}

// ============================================================================
// Active state
// ============================================================================

class ActiveHierarchyTest : public ::testing::Test
{
protected:
    GameObject::Ptr a, b, c;
    std::shared_ptr<Counts> cCounts;
    Probe *cProbe = nullptr;

    void SetUp() override
    {
        a = GameObject::Create("A");
        b = GameObject::Create("B");
        c = GameObject::Create("C");
        cProbe = c->AddComponent<Probe>();
        cCounts = cProbe->counts;
        a->AddChild(b);
        b->AddChild(c);

        // Every frame queries these, which fills each object's cache
        ASSERT_TRUE(a->IsActiveInHierarchy());
        ASSERT_TRUE(b->IsActiveInHierarchy());
        ASSERT_TRUE(c->IsActiveInHierarchy());
    }
};

TEST_F(ActiveHierarchyTest, DeactivatingRootDeactivatesGrandchild)
{
    a->SetActive(false);

    EXPECT_FALSE(c->IsActiveInHierarchy()) << "grandchild kept a stale cached answer";
    EXPECT_EQ(cCounts->disable, 1);

    a->SetActive(true);
    EXPECT_TRUE(c->IsActiveInHierarchy());
    EXPECT_EQ(cCounts->enable, 1);
}

TEST_F(ActiveHierarchyTest, ToggleKeepsComponentsOwnEnabledFlag)
{
    cProbe->SetActive(false);

    a->SetActive(false);
    a->SetActive(true);

    EXPECT_FALSE(cProbe->IsActive()) << "the component's own disable was overwritten";
    EXPECT_EQ(cCounts->enable, 0);
    EXPECT_EQ(cCounts->disable, 0);
}

TEST_F(ActiveHierarchyTest, InactiveChildStaysInactiveWhenParentToggles)
{
    b->SetActive(false);
    EXPECT_EQ(cCounts->disable, 1);

    a->SetActive(false);
    a->SetActive(true);

    EXPECT_FALSE(c->IsActiveInHierarchy());
    EXPECT_EQ(cCounts->enable, 0) << "C's branch is still switched off at B";
    EXPECT_EQ(cCounts->disable, 1);
}

TEST_F(ActiveHierarchyTest, SettingSameStateFiresNothing)
{
    a->SetActive(true);
    EXPECT_EQ(cCounts->enable, 0);
    EXPECT_EQ(cCounts->disable, 0);
}

TEST_F(ActiveHierarchyTest, ReparentingUnderInactiveParentDisables)
{
    const auto inactive = GameObject::Create("InactiveParent");
    inactive->SetActive(false);

    inactive->AddChild(c);
    EXPECT_FALSE(c->IsActiveInHierarchy());
    EXPECT_EQ(cCounts->disable, 1);

    b->AddChild(c);
    EXPECT_TRUE(c->IsActiveInHierarchy());
    EXPECT_EQ(cCounts->enable, 1);
}

// ============================================================================
// SceneManager
// ============================================================================

TEST(SceneManagerTest, DeleteSceneRemovesTheNamedScene)
{
    SceneManager::AddScene(Scene::Create("SM_DeleteMe"), false);
    SceneManager::AddScene(Scene::Create("SM_Keep"), false);
    LoadFreshScene("SM_Loaded");
    const int loadedIndex = SceneManager::GetCurSceneIndex();

    EXPECT_TRUE(SceneManager::DeleteScene("SM_DeleteMe"));

    EXPECT_EQ(SceneManager::GetSceneIndex("SM_DeleteMe"), -1);
    EXPECT_NE(SceneManager::GetSceneIndex("SM_Keep"), -1);
    EXPECT_EQ(SceneManager::GetCurSceneIndex(), loadedIndex - 1) << "indices after the deleted scene shift";
    EXPECT_EQ(SceneManager::GetSceneIndex("SM_Loaded"), SceneManager::GetCurSceneIndex());
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "SM_Loaded");
}

TEST(SceneManagerTest, DeleteSceneRefusesLoadedAndUnknown)
{
    LoadFreshScene("SM_LoadedNoDelete");

    EXPECT_FALSE(SceneManager::DeleteScene("SM_LoadedNoDelete"));
    EXPECT_FALSE(SceneManager::DeleteScene("SM_NeverAdded"));
    EXPECT_NE(SceneManager::GetSceneIndex("SM_LoadedNoDelete"), -1);
}

TEST(SceneManagerTest, GetSceneReturnsOwnedCopy)
{
    SceneManager::AddScene(Scene::Create("SM_GetScene"), false);

    const std::unique_ptr<Scene> scene = SceneManager::GetScene("SM_GetScene");

    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->sceneName, "SM_GetScene");
    EXPECT_EQ(SceneManager::GetScene("SM_NoSuchScene"), nullptr);
}
