#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;

namespace
{
    // Counts live outside the component so they can be read after it's freed
    struct Counts
    {
        int attach = 0;
        int update = 0;
        int enable = 0;
        int disable = 0;
        int destroy = 0;
    };

    class Hooked : public Component
    {
    public:
        explicit Hooked(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "Hooked"; }

        void OnAttach() override { ++counts->attach; }
        void OnUpdate() override { ++counts->update; }
        void OnEnable() override { ++counts->enable; }
        void OnDisable() override
        {
            ++counts->disable;
            if (onDisable)
            {
                onDisable(*this);
            }
        }
        void OnDestroy() override
        {
            ++counts->destroy;
            if (onDestroy)
            {
                onDestroy(*this);
            }
        }

        std::shared_ptr<Counts> counts = std::make_shared<Counts>();
        std::function<void(Hooked &)> onDisable;
        std::function<void(Hooked &)> onDestroy;
    };

    class Plain final : public Component
    {
    public:
        explicit Plain(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "Plain"; }
    };

    // Removes (frees) itself as the last thing OnAttach does
    class RemovesItselfOnAttach final : public Component
    {
    public:
        explicit RemovesItselfOnAttach(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "RemovesItselfOnAttach"; }

        void OnAttach() override
        {
            const auto keep = counts;
            ++keep->attach;
            GetGameObject().RemoveComponent(this);
        }

        std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    };

    Scene &LoadFreshScene(const std::string &name)
    {
        SceneManager::AddScene(Scene::Create(name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return SceneManager::GetCurSceneRef();
    }

    void Frame(Scene &scene)
    {
        scene.ProcessAttachQueue();
        scene.Update();
        scene.LateUpdate();
        scene.ProcessDestroyed();
    }

    bool IsRoot(const Scene &scene, const GameObject::Ptr &go)
    {
        return std::ranges::find(scene.GetRootGameObjects(), go) != scene.GetRootGameObjects().end();
    }
}

// ============================================================================
// RemoveChild, SetParent, AddRootGameObject
// ============================================================================

TEST(HierarchyLifetimeTest, RemoveChildMakesTheChildARootOfItsScene)
{
    Scene &scene = LoadFreshScene("Hierarchy_RemoveChild");
    const auto parent = GameObject::Create("Parent");
    const auto child = GameObject::Create("Child");
    const auto counts = child->AddComponent<Hooked>()->counts;
    parent->AddChild(child);
    scene.AddRootGameObject(parent);
    Frame(scene);

    parent->RemoveChild(child);

    EXPECT_EQ(child->GetParent(), nullptr);
    EXPECT_TRUE(parent->GetChildren().empty());
    EXPECT_TRUE(IsRoot(scene, child)) << "it used to be in the scene without being under any root";
    EXPECT_EQ(child->GetScene(), &scene);

    Frame(scene);
    EXPECT_EQ(counts->attach, 1) << "same scene, so no second OnAttach";
    EXPECT_EQ(counts->update, 2);
}

TEST(HierarchyLifetimeTest, RemovedChildIsTornDownBySceneSwitch)
{
    Scene &scene = LoadFreshScene("Hierarchy_RemoveThenSwitch");
    const auto parent = GameObject::Create("Parent");
    auto child = GameObject::Create("Child");
    const auto counts = child->AddComponent<Hooked>()->counts;
    parent->AddChild(child);
    scene.AddRootGameObject(parent);
    Frame(scene);

    parent->RemoveChild(child);
    LoadFreshScene("Hierarchy_RemoveThenSwitch_Next");

    // Scene::Clear only walks the roots, so it used to miss the detached child, which kept a pointer to
    // the freed scene for AddComponent and ~GameObject to use
    EXPECT_EQ(counts->destroy, 1);
    EXPECT_EQ(child->GetScene(), nullptr);
    EXPECT_NE(child->AddComponent<Plain>(), nullptr);
    EXPECT_NO_FATAL_FAILURE(child.reset());
}

TEST(HierarchyLifetimeTest, RemoveChildOutsideASceneOnlyDetaches)
{
    const auto parent = GameObject::Create("Parent");
    const auto child = GameObject::Create("Child");
    parent->AddChild(child);

    parent->RemoveChild(child);

    EXPECT_EQ(child->GetParent(), nullptr);
    EXPECT_EQ(child->GetScene(), nullptr);
    EXPECT_TRUE(parent->GetChildren().empty());
}

TEST(HierarchyLifetimeTest, RemoveChildIgnoresAnotherObjectsChild)
{
    const auto parent = GameObject::Create("Parent");
    const auto stranger = GameObject::Create("Stranger");
    const auto child = GameObject::Create("Child");
    parent->AddChild(child);

    stranger->RemoveChild(child);

    EXPECT_EQ(child->GetParent(), parent);
}

TEST(HierarchyLifetimeTest, AddRootGameObjectMovesARootBetweenScenes)
{
    const auto first = Scene::Create("Hierarchy_FirstScene");
    const auto second = Scene::Create("Hierarchy_SecondScene");
    const auto go = GameObject::Create("Mover");
    first->AddRootGameObject(go);

    second->AddRootGameObject(go);

    EXPECT_FALSE(IsRoot(*first, go)) << "it used to stay a root of the scene it left";
    EXPECT_TRUE(IsRoot(*second, go));
    EXPECT_EQ(go->GetScene(), second.get());
}

TEST(HierarchyLifetimeTest, SetParentToADescendantIsRefused)
{
    Scene &scene = LoadFreshScene("Hierarchy_SetParentCycle");
    const auto top = GameObject::Create("Top");
    const auto middle = GameObject::Create("Middle");
    const auto bottom = GameObject::Create("Bottom");
    top->AddChild(middle);
    middle->AddChild(bottom);
    scene.AddRootGameObject(top);

    top->SetParent(bottom);    // a root under its grandchild
    middle->SetParent(bottom); // a child under its own child

    // middle used to leave top before AddChild refused the cycle, so it was orphaned (parentless, not a root)
    EXPECT_EQ(top->GetParent(), nullptr);
    EXPECT_TRUE(IsRoot(scene, top));
    EXPECT_EQ(middle->GetParent(), top);
    EXPECT_EQ(bottom->GetParent(), middle);
    EXPECT_TRUE(bottom->GetChildren().empty());
}

TEST(HierarchyLifetimeTest, DestroyGameObjectRefusesAnotherScenesObject)
{
    Scene &scene = LoadFreshScene("Hierarchy_DestroyMembership");
    const auto other = Scene::Create("Hierarchy_DestroyMembership_Other");
    const auto stranger = GameObject::Create("Stranger");
    other->AddRootGameObject(stranger);

    EXPECT_FALSE(scene.DestroyGameObject(stranger));
    scene.ProcessDestroyed();

    EXPECT_FALSE(stranger->IsDestroyed());
    EXPECT_TRUE(IsRoot(*other, stranger));
}

// ============================================================================
// Callbacks that change the components being iterated
// ============================================================================

TEST(HierarchyLifetimeTest, ComponentRemovingItselfFromOnDisableIsRemovedOnce)
{
    Scene &scene = LoadFreshScene("Hierarchy_SelfRemoveOnDisable");
    const auto go = GameObject::Create("Go");
    auto *hooked = go->AddComponent<Hooked>();
    const auto counts = hooked->counts;
    hooked->onDisable = [](Hooked &self) { self.GetGameObject().RemoveComponent(&self); };
    scene.AddRootGameObject(go);
    Frame(scene);

    EXPECT_TRUE(go->RemoveComponent(hooked));

    EXPECT_EQ(counts->disable, 1);
    EXPECT_EQ(counts->destroy, 1);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(HierarchyLifetimeTest, ComponentRemovingItselfFromOnDestroyIsRemovedOnce)
{
    Scene &scene = LoadFreshScene("Hierarchy_SelfRemoveOnDestroy");
    const auto go = GameObject::Create("Go");
    auto *hooked = go->AddComponent<Hooked>();
    const auto counts = hooked->counts;
    hooked->onDestroy = [](Hooked &self) { self.GetGameObject().RemoveComponent(&self); };
    scene.AddRootGameObject(go);
    Frame(scene);

    // Used to recurse: each nested RemoveComponent started OnDisable/OnDestroy again
    EXPECT_TRUE(go->RemoveComponent(hooked));

    EXPECT_EQ(counts->destroy, 1);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(HierarchyLifetimeTest, ComponentRemovingItselfWhileItsObjectIsDestroyed)
{
    Scene &scene = LoadFreshScene("Hierarchy_SelfRemoveDuringDestroy");
    const auto go = GameObject::Create("Go");
    auto *hooked = go->AddComponent<Hooked>();
    const auto counts = hooked->counts;
    hooked->onDestroy = [](Hooked &self) { self.GetGameObject().RemoveComponent(&self); };
    scene.AddRootGameObject(go);
    Frame(scene);

    go->Destroy();
    scene.ProcessDestroyed();

    EXPECT_EQ(counts->destroy, 1);
    EXPECT_TRUE(go->IsDestroyed());
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(HierarchyLifetimeTest, RemoveAllComponentsFromOnDestroyWhileTheObjectIsDestroyed)
{
    Scene &scene = LoadFreshScene("Hierarchy_RemoveAllDuringDestroy");
    const auto go = GameObject::Create("Go");
    auto *first = go->AddComponent<Hooked>();
    const auto firstCounts = first->counts;
    const auto secondCounts = go->AddComponent<Hooked>()->counts;
    const auto thirdCounts = go->AddComponent<Hooked>()->counts;
    first->onDestroy = [](Hooked &self) { self.GetGameObject().RemoveAllComponents(); };
    scene.AddRootGameObject(go);
    Frame(scene);

    // The destroy pass iterated the live component vector, which the nested call emptied
    go->Destroy();
    scene.ProcessDestroyed();

    EXPECT_EQ(firstCounts->destroy, 1);
    EXPECT_EQ(secondCounts->destroy, 1);
    EXPECT_EQ(thirdCounts->destroy, 1);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(HierarchyLifetimeTest, AddComponentFromOnDestroy)
{
    Scene &scene = LoadFreshScene("Hierarchy_AddDuringDestroy");
    const auto go = GameObject::Create("Go");
    auto *hooked = go->AddComponent<Hooked>();
    const auto counts = hooked->counts;
    hooked->onDestroy = [](Hooked &self)
    {
        for (int i = 0; i < 16; ++i) // enough to reallocate the vector being iterated
        {
            self.GetGameObject().AddComponent<Plain>();
        }
    };
    scene.AddRootGameObject(go);
    Frame(scene);

    go->Destroy();
    scene.ProcessDestroyed();
    Frame(scene); // the attach queue must not hold the components added (and since freed)

    EXPECT_EQ(counts->destroy, 1);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(HierarchyLifetimeTest, AddComponentFromOnDestroyDuringRemoveAllComponents)
{
    Scene &scene = LoadFreshScene("Hierarchy_AddDuringRemoveAll");
    const auto go = GameObject::Create("Go");
    auto *hooked = go->AddComponent<Hooked>();
    const auto counts = hooked->counts;
    hooked->onDestroy = [](Hooked &self)
    {
        for (int i = 0; i < 16; ++i)
        {
            self.GetGameObject().AddComponent<Plain>();
        }
    };
    scene.AddRootGameObject(go);
    Frame(scene);

    go->RemoveAllComponents();
    Frame(scene);

    EXPECT_EQ(counts->destroy, 1);
    EXPECT_EQ(go->GetComponentCount(), 0u);
}

TEST(HierarchyLifetimeTest, AddComponentFromOnDisableWhenDeactivated)
{
    Scene &scene = LoadFreshScene("Hierarchy_AddDuringDisable");
    const auto go = GameObject::Create("Go");
    auto *hooked = go->AddComponent<Hooked>();
    const auto counts = hooked->counts;
    hooked->onDisable = [](Hooked &self)
    {
        for (int i = 0; i < 16; ++i)
        {
            self.GetGameObject().AddComponent<Plain>();
        }
    };
    scene.AddRootGameObject(go);
    Frame(scene);

    go->SetActive(false);

    EXPECT_EQ(counts->disable, 1);
    EXPECT_EQ(go->GetComponentCount(), 17u);
}

TEST(HierarchyLifetimeTest, ComponentRemovingItselfFromOnAttach)
{
    Scene &scene = LoadFreshScene("Hierarchy_SelfRemoveOnAttach");
    const auto go = GameObject::Create("Go");
    const auto counts = go->AddComponent<RemovesItselfOnAttach>()->counts;
    scene.AddRootGameObject(go);

    // ProcessAttachQueue used to register the freed component after its OnAttach returned
    Frame(scene);
    Frame(scene);

    EXPECT_EQ(counts->attach, 1);
    EXPECT_EQ(go->GetComponentCount(), 0u);
    EXPECT_TRUE(scene.FindObjectsByType<RemovesItselfOnAttach>().empty());
}
