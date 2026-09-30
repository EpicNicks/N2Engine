#include <gtest/gtest.h>

#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

using namespace N2Engine;

namespace
{
    // Counts how often scene data is built into objects: every build constructs one per stored instance
    class ConstructionCounter final : public SerializableComponent
    {
    public:
        static inline int constructed = 0;

        explicit ConstructionCounter(GameObject &gameObject) : SerializableComponent(gameObject) { ++constructed; }
        [[nodiscard]] std::string GetTypeName() const override { return "SceneStorage_ConstructionCounter"; }
    };

    void RegisterConstructionCounter()
    {
        ComponentRegistry::Instance().Register(
            "SceneStorage_ConstructionCounter",
            [](GameObject &gameObject) -> std::unique_ptr<Component>
            {
                return std::make_unique<ConstructionCounter>(gameObject);
            });
    }
}

// GetSceneIndex used to deserialize every stored scene (every object and component, with their
// physics/audio/script state) just to compare names
TEST(SceneStorageTest, GetSceneIndexDoesNotBuildScenes)
{
    RegisterConstructionCounter();
    auto scene = Scene::Create("SceneStorage_Counted");
    const auto go = GameObject::Create("Counted");
    go->AddComponent<ConstructionCounter>();
    scene->AddRootGameObject(go);
    SceneManager::AddScene(std::move(scene), false);

    const int before = ConstructionCounter::constructed;
    EXPECT_NE(SceneManager::GetSceneIndex("SceneStorage_Counted"), -1);
    EXPECT_EQ(SceneManager::GetSceneIndex("SceneStorage_NoSuchScene"), -1);
    EXPECT_EQ(ConstructionCounter::constructed, before) << "a name lookup built a scene";

    // Building the stored scene does construct it, so the counter would have seen a build above
    EXPECT_NE(SceneManager::GetScene("SceneStorage_Counted"), nullptr);
    EXPECT_EQ(ConstructionCounter::constructed, before + 1);
}

// AddScene used to serialize the scene and drop it, then load a copy, so objects the caller kept
// belonged to an orphaned scene
TEST(SceneStorageTest, AddedAndLoadedSceneIsTheCallersObject)
{
    auto scene = Scene::Create("SceneStorage_Live");
    const Scene *added = scene.get();
    const auto go = GameObject::Create("Kept");
    scene->AddRootGameObject(go);

    SceneManager::AddScene(std::move(scene), true);
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_EQ(SceneManager::GetCurScene(), added);
    EXPECT_EQ(go->GetScene(), added);
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("Kept"), go);
}

TEST(SceneStorageTest, LoadingAStoredSceneAgainRebuildsItFromItsSnapshot)
{
    auto scene = Scene::Create("SceneStorage_Reload");
    scene->AddRootGameObject(GameObject::Create("Snapshotted"));
    SceneManager::AddScene(std::move(scene), true);
    SceneManager::ProcessAnyPendingSceneChange();
    // Not part of the snapshot taken by AddScene
    SceneManager::GetCurSceneRef().AddRootGameObject(GameObject::Create("AddedAfterSnapshot"));

    SceneManager::AddScene(Scene::Create("SceneStorage_Elsewhere"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_EQ(SceneManager::GetCurSceneRef().sceneName, "SceneStorage_Elsewhere");

    SceneManager::LoadScene("SceneStorage_Reload");
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "SceneStorage_Reload");
    EXPECT_NE(SceneManager::GetCurSceneRef().FindGameObject("Snapshotted"), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("AddedAfterSnapshot"), nullptr);
}

// A second entry with the same name could never be found by name, so re-adding replaces the first
TEST(SceneStorageTest, ReAddingASceneNameReplacesIt)
{
    SceneManager::AddScene(Scene::Create("SceneStorage_Replaced"), false);
    const int index = SceneManager::GetSceneIndex("SceneStorage_Replaced");
    ASSERT_NE(index, -1);

    auto replacement = Scene::Create("SceneStorage_Replaced");
    const Scene *replacementScene = replacement.get();
    SceneManager::AddScene(std::move(replacement), true);
    EXPECT_EQ(SceneManager::GetSceneIndex("SceneStorage_Replaced"), index);
    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_EQ(SceneManager::GetCurScene(), replacementScene);
    EXPECT_EQ(SceneManager::GetCurSceneIndex(), index);

    // Re-adding the loaded scene's name (and loading it) replaces the loaded scene as well
    auto third = Scene::Create("SceneStorage_Replaced");
    const auto go = GameObject::Create("InThird");
    third->AddRootGameObject(go);
    const Scene *thirdScene = third.get();
    SceneManager::AddScene(std::move(third), true);
    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_EQ(SceneManager::GetCurScene(), thirdScene);
    EXPECT_EQ(SceneManager::GetCurSceneIndex(), index);
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("InThird"), go);
}

// Another load requested before the change is processed wins; the superseded scene's snapshot stays
TEST(SceneStorageTest, SupersededAddedSceneIsStillStored)
{
    SceneManager::AddScene(Scene::Create("SceneStorage_Target"), false);
    SceneManager::AddScene(Scene::Create("SceneStorage_Superseded"), true);
    SceneManager::LoadScene("SceneStorage_Target");
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "SceneStorage_Target");
    EXPECT_NE(SceneManager::GetSceneIndex("SceneStorage_Superseded"), -1);
}
