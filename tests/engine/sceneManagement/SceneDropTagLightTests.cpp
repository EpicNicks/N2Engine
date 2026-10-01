#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/rendering/Light.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;

namespace
{
    // Counts live outside the component so they can be read after it's freed
    struct DropCounts
    {
        int attach = 0;
        int disable = 0;
        int destroy = 0;
    };

    class DropProbe : public Component
    {
    public:
        explicit DropProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "DropProbe"; }

        void OnAttach() override { ++counts->attach; }
        void OnDisable() override { ++counts->disable; }
        void OnDestroy() override
        {
            ++counts->destroy;
            if (onDestroy)
            {
                onDestroy();
            }
        }

        std::shared_ptr<DropCounts> counts = std::make_shared<DropCounts>();
        std::function<void()> onDestroy;
    };

    // A Light that records the per-frame callbacks it gets
    struct LightCounts
    {
        int update = 0;
        int fixedUpdate = 0;
        int lateUpdate = 0;
        int quit = 0;
        int destroy = 0;
    };

    class CountingLight final : public Rendering::Light
    {
    public:
        explicit CountingLight(GameObject &gameObject) : Light(gameObject) {}

        void OnUpdate() override { ++counts->update; }
        void OnFixedUpdate() override { ++counts->fixedUpdate; }
        void OnLateUpdate() override { ++counts->lateUpdate; }
        void OnApplicationQuit() override { ++counts->quit; }
        void OnDestroy() override { ++counts->destroy; }

        std::shared_ptr<LightCounts> counts = std::make_shared<LightCounts>();
    };

    Scene &LoadFreshDropScene(const std::string &name)
    {
        SceneManager::AddScene(Scene::Create(name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return SceneManager::GetCurSceneRef();
    }

    // A scene holding one object whose probe has attached, and one whose probe is still queued
    struct HalfAttachedScene
    {
        std::unique_ptr<Scene> scene;
        std::shared_ptr<DropCounts> attached;
        std::shared_ptr<DropCounts> queued;
        GameObject::Ptr attachedObject;
        GameObject::Ptr queuedObject;
    };

    HalfAttachedScene MakeHalfAttachedScene(const std::string &name)
    {
        HalfAttachedScene result;
        result.scene = Scene::Create(name);

        const auto first = GameObject::Create("Attached");
        result.attached = first->AddComponent<DropProbe>()->counts;
        result.scene->AddRootGameObject(first);
        result.scene->ProcessAttachQueue();

        const auto second = GameObject::Create("Queued");
        result.queued = second->AddComponent<DropProbe>()->counts;
        result.scene->AddRootGameObject(second);
        result.attachedObject = first;
        result.queuedObject = second;
        return result;
    }
}

// ============================================================================
// Scenes dropped without being loaded
// ============================================================================

TEST(DroppedSceneTeardownTest, NeverAttachedSceneGetsNoCallbacks)
{
    auto scene = Scene::Create("DroppedScene_NeverAttached");
    const auto go = GameObject::Create("Idle");
    const auto counts = go->AddComponent<DropProbe>()->counts;
    scene->AddRootGameObject(go);

    scene.reset();

    EXPECT_EQ(counts->attach, 0);
    EXPECT_EQ(counts->disable, 0);
    EXPECT_EQ(counts->destroy, 0) << "a component that never attached has nothing to tear down";
    EXPECT_EQ(go->GetScene(), nullptr) << "the object must not keep pointing at the freed scene";
}

TEST(DroppedSceneTeardownTest, AttachedComponentsAreTornDownOnDrop)
{
    HalfAttachedScene dropped = MakeHalfAttachedScene("DroppedScene_HalfAttached");
    ASSERT_EQ(dropped.attached->attach, 1);
    ASSERT_EQ(dropped.queued->attach, 0);

    dropped.scene.reset();

    EXPECT_EQ(dropped.attached->disable, 1);
    EXPECT_EQ(dropped.attached->destroy, 1) << "an attached component of a dropped scene was not torn down";
    EXPECT_EQ(dropped.queued->destroy, 0) << "OnDestroy without OnAttach";
    EXPECT_EQ(dropped.queued->disable, 0);

    // Only the object whose component attached is torn down; the other is released as in a scene where
    // nothing attached (it does not depend on its siblings)
    EXPECT_TRUE(dropped.attachedObject->IsDestroyed());
    EXPECT_TRUE(dropped.attachedObject->IsTornDown());
    EXPECT_FALSE(dropped.queuedObject->IsDestroyed());
    EXPECT_FALSE(dropped.queuedObject->IsTornDown());
    EXPECT_TRUE(dropped.queuedObject->IsActiveInHierarchy());
    EXPECT_EQ(dropped.queuedObject->GetScene(), nullptr);
}

TEST(DroppedSceneTeardownTest, ClearedSceneIsNotTornDownTwice)
{
    HalfAttachedScene dropped = MakeHalfAttachedScene("DroppedScene_Cleared");
    dropped.scene->Clear();
    ASSERT_EQ(dropped.attached->destroy, 1);

    dropped.scene.reset();

    EXPECT_EQ(dropped.attached->destroy, 1);
}

TEST(DroppedSceneTeardownTest, AddSceneWithoutLoadTearsDownAttachedComponents)
{
    HalfAttachedScene dropped = MakeHalfAttachedScene("DroppedScene_AddWithoutLoad");

    SceneManager::AddScene(std::move(dropped.scene), false);

    EXPECT_EQ(dropped.attached->destroy, 1);
    EXPECT_EQ(dropped.queued->destroy, 0);
    EXPECT_TRUE(SceneManager::HasScene("DroppedScene_AddWithoutLoad")) << "the snapshot is still stored";
}

TEST(DroppedSceneTeardownTest, SupersededPendingSceneTearsDownAttachedComponents)
{
    LoadFreshDropScene("DroppedScene_Current");
    SceneManager::AddScene(Scene::Create("DroppedScene_Next"), false);

    HalfAttachedScene pending = MakeHalfAttachedScene("DroppedScene_Pending");
    const std::shared_ptr<DropCounts> attached = pending.attached;
    const std::shared_ptr<DropCounts> queued = pending.queued;
    SceneManager::AddScene(std::move(pending.scene), true);
    ASSERT_EQ(attached->destroy, 0) << "pending, not dropped yet";

    // Another load request supersedes the pending instance, which is dropped right here
    SceneManager::LoadScene("DroppedScene_Next");
    EXPECT_EQ(attached->destroy, 1);
    EXPECT_EQ(queued->destroy, 0);

    SceneManager::ProcessAnyPendingSceneChange();
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "DroppedScene_Next");
    EXPECT_EQ(attached->destroy, 1);
}

// A dropped scene's OnDestroy that calls LoadScene runs after SceneManager's own change is complete, so it
// is just a later request (the last one wins) and never corrupts the pending state
TEST(DroppedSceneTeardownTest, LoadSceneFromADroppedPendingScenesOnDestroyIsALaterRequest)
{
    LoadFreshDropScene("DropReentry_Current");
    SceneManager::AddScene(Scene::Create("DropReentry_Other"), false);

    auto pending = Scene::Create("DropReentry_Pending");
    const auto go = GameObject::Create("Reentrant");
    auto *probe = go->AddComponent<DropProbe>();
    const auto counts = probe->counts;
    probe->onDestroy = [] { SceneManager::LoadScene("DropReentry_Other"); };
    pending->AddRootGameObject(go);
    pending->ProcessAttachQueue();
    SceneManager::AddScene(std::move(pending), true);

    // Supersedes the pending scene, whose teardown then requests another load
    SceneManager::AddScene(Scene::Create("DropReentry_S"), true);
    EXPECT_EQ(counts->destroy, 1);

    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "DropReentry_Other");
    EXPECT_EQ(SceneManager::GetCurSceneIndex(), SceneManager::GetSceneIndex("DropReentry_Other"));
    EXPECT_TRUE(SceneManager::HasScene("DropReentry_S"));
}

TEST(DroppedSceneTeardownTest, ReentrantLoadOfTheNewSceneKeepsTheCallersInstance)
{
    LoadFreshDropScene("DropReentry2_Current");

    auto pending = Scene::Create("DropReentry2_Pending");
    const auto go = GameObject::Create("Reentrant");
    auto *probe = go->AddComponent<DropProbe>();
    probe->onDestroy = [] { SceneManager::LoadScene("DropReentry2_S"); };
    pending->AddRootGameObject(go);
    pending->ProcessAttachQueue();
    SceneManager::AddScene(std::move(pending), true);

    auto next = Scene::Create("DropReentry2_S");
    const auto kept = GameObject::Create("Kept");
    next->AddRootGameObject(kept);
    SceneManager::AddScene(std::move(next), true);

    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "DropReentry2_S");
    EXPECT_EQ(kept->GetScene(), SceneManager::GetCurScene()) << "the caller's own S object is the loaded scene";
}

TEST(DroppedSceneTeardownTest, GetSceneCopyAttachedByCallerIsTornDownOnDrop)
{
    auto source = Scene::Create("DroppedScene_Copied");
    source->AddRootGameObject(GameObject::Create("Plain"));
    SceneManager::AddScene(std::move(source), false);

    std::unique_ptr<Scene> copy = SceneManager::GetScene("DroppedScene_Copied");
    ASSERT_NE(copy, nullptr);
    const auto go = GameObject::Create("Added");
    const auto counts = go->AddComponent<DropProbe>()->counts;
    copy->AddRootGameObject(go);
    copy->ProcessAttachQueue();
    ASSERT_EQ(counts->attach, 1);

    copy.reset();

    EXPECT_EQ(counts->destroy, 1);
    EXPECT_TRUE(go->IsDestroyed());
    EXPECT_EQ(go->GetScene(), nullptr);
}

TEST(DroppedSceneTeardownTest, LoadedSceneSwitchStillTearsDownQueuedComponents)
{
    // Unchanged: Clear on a scene switch tears down every component, attached or still queued
    Scene &scene = LoadFreshDropScene("DroppedScene_SwitchFrom");
    const auto go = GameObject::Create("JustAdded");
    const auto counts = go->AddComponent<DropProbe>()->counts;
    scene.AddRootGameObject(go);

    LoadFreshDropScene("DroppedScene_SwitchTo");

    EXPECT_EQ(counts->attach, 0);
    EXPECT_EQ(counts->destroy, 1);
}

// ============================================================================
// Lights get the per-frame callbacks
// ============================================================================

TEST(LightCallbacksTest, LightReceivesFrameCallbacks)
{
    Scene &scene = LoadFreshDropScene("LightCallbacks_Frame");
    const auto go = GameObject::Create("Lamp");
    auto *light = go->AddComponent<CountingLight>();
    const auto counts = light->counts;
    scene.AddRootGameObject(go);

    scene.ProcessAttachQueue();
    scene.FixedUpdate();
    scene.Update();
    scene.LateUpdate();
    scene.OnApplicationQuit();

    EXPECT_EQ(counts->fixedUpdate, 1);
    EXPECT_EQ(counts->update, 1);
    EXPECT_EQ(counts->lateUpdate, 1);
    EXPECT_EQ(counts->quit, 1);
}

TEST(LightCallbacksTest, LightStaysInTheLightListAndTypeLookups)
{
    Scene &scene = LoadFreshDropScene("LightCallbacks_Lookups");
    const auto go = GameObject::Create("Lamp");
    auto *light = go->AddComponent<CountingLight>();
    scene.AddRootGameObject(go);
    scene.ProcessAttachQueue();
    scene.ProcessAttachQueue(); // registration stays idempotent

    EXPECT_EQ(scene.FindObjectByType<Rendering::Light>(), light);
    EXPECT_EQ(scene.FindObjectsByType<Rendering::Light>().size(), 1u);
    EXPECT_EQ(scene.FindObjectsByType<CountingLight>().size(), 1u);
    EXPECT_EQ(scene.FindObjectsByType<Component>().size(), 1u);
    EXPECT_EQ(scene.CollectLighting().directionalLights.size(), 1u);

    scene.Update();
    EXPECT_EQ(light->counts->update, 1) << "updated once, not once per registration";
}

TEST(LightCallbacksTest, DestroyedLightStopsUpdating)
{
    Scene &scene = LoadFreshDropScene("LightCallbacks_Destroy");
    const auto go = GameObject::Create("Lamp");
    const auto counts = go->AddComponent<CountingLight>()->counts;
    scene.AddRootGameObject(go);
    scene.ProcessAttachQueue();
    scene.Update();

    go->Destroy();
    scene.ProcessDestroyed();
    scene.Update();

    EXPECT_EQ(counts->update, 1);
    EXPECT_EQ(counts->destroy, 1);
    EXPECT_EQ(scene.FindObjectByType<Rendering::Light>(), nullptr);
    EXPECT_TRUE(scene.FindObjectsByType<Component>().empty());
}

TEST(LightCallbacksTest, InactiveLightGetsNoUpdate)
{
    Scene &scene = LoadFreshDropScene("LightCallbacks_Inactive");
    const auto go = GameObject::Create("Lamp");
    const auto counts = go->AddComponent<CountingLight>()->counts;
    scene.AddRootGameObject(go);
    scene.ProcessAttachQueue();

    go->SetActive(false);
    scene.Update();

    EXPECT_EQ(counts->update, 0);
}

// ============================================================================
// Tags
// ============================================================================

TEST(GameObjectTagTest, DefaultsToUntagged)
{
    const auto go = GameObject::Create("Plain");
    EXPECT_EQ(go->GetTag(), "Untagged");
    EXPECT_EQ(go->GetTag(), GameObject::DefaultTag);
    EXPECT_TRUE(go->CompareTag("Untagged"));
}

TEST(GameObjectTagTest, SetTagAndCompareTag)
{
    const auto go = GameObject::Create("Hero");
    go->SetTag("Player");

    EXPECT_EQ(go->GetTag(), "Player");
    EXPECT_TRUE(go->CompareTag("Player"));
    EXPECT_FALSE(go->CompareTag("player")) << "tags compare case-sensitively";
    EXPECT_FALSE(go->CompareTag("Untagged"));
}

TEST(GameObjectTagTest, SceneFindsTaggedObjectsAnywhereInTheHierarchy)
{
    Scene &scene = LoadFreshDropScene("Tags_Find");
    const auto root = GameObject::Create("Root");
    const auto child = GameObject::Create("Child");
    const auto inactive = GameObject::Create("Inactive");
    const auto other = GameObject::Create("Other");
    child->SetTag("Enemy");
    inactive->SetTag("Enemy");
    inactive->SetActive(false);
    other->SetTag("Pickup");
    root->AddChild(child);
    scene.AddRootGameObjects({root, inactive, other});

    EXPECT_EQ(scene.FindGameObjectWithTag("Enemy"), child);
    EXPECT_EQ(scene.FindGameObjectWithTag("Pickup"), other);
    EXPECT_EQ(scene.FindGameObjectWithTag("Missing"), nullptr);

    const auto enemies = scene.FindGameObjectsWithTag("Enemy");
    ASSERT_EQ(enemies.size(), 2u);
    EXPECT_EQ(enemies[0], child);
    EXPECT_EQ(enemies[1], inactive) << "inactive objects are included";
    EXPECT_EQ(scene.FindGameObjectsByTag("Enemy"), enemies) << "the older name does the same";
    EXPECT_EQ(scene.FindGameObjectsWithTag("Untagged").size(), 1u);
    EXPECT_TRUE(scene.FindGameObjectsWithTag("Missing").empty());
}

TEST(GameObjectTagTest, StaticHelpersUseTheGivenScene)
{
    Scene &scene = LoadFreshDropScene("Tags_Static");
    const auto go = GameObject::Create("Named");
    go->SetTag("Marker");
    scene.AddRootGameObject(go);

    EXPECT_EQ(GameObject::FindGameObjectWithTag("Marker", &scene), go);
    EXPECT_EQ(GameObject::FindGameObjectsWithTag("Marker", &scene).size(), 1u);
    EXPECT_EQ(GameObject::FindGameObjectsByTag("Marker", &scene).size(), 1u);
    EXPECT_EQ(GameObject::FindGameObjectByName("Named", &scene), go);

    EXPECT_EQ(GameObject::FindGameObjectWithTag("Marker", nullptr), nullptr);
    EXPECT_TRUE(GameObject::FindGameObjectsWithTag("Marker", nullptr).empty());
    EXPECT_EQ(GameObject::FindGameObjectByName("Named", nullptr), nullptr);
}

TEST(GameObjectTagTest, TagRoundTripsThroughSerialization)
{
    const auto go = GameObject::Create("Saved");
    go->SetTag("Respawn");
    const nlohmann::json j = go->Serialize();
    EXPECT_EQ(j.at("tag").get<std::string>(), "Respawn");

    const auto loaded = GameObject::Deserialize(j);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetTag(), "Respawn");
}

TEST(GameObjectTagTest, DataWithoutTagLoadsUntagged)
{
    nlohmann::json j = GameObject::Create("Old")->Serialize();
    j.erase("tag");

    const auto loaded = GameObject::Deserialize(j);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetTag(), "Untagged");
}

TEST(GameObjectTagTest, TaggedSceneRoundTrips)
{
    auto scene = Scene::Create("Tags_RoundTrip");
    const auto go = GameObject::Create("Tagged");
    go->SetTag("Goal");
    scene->AddRootGameObject(go);

    const auto rebuilt = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(rebuilt, nullptr);
    const auto found = rebuilt->FindGameObjectWithTag("Goal");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->GetName(), "Tagged");
}
