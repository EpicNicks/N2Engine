#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

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

// Guard: name lookups must never deserialize a stored scene (every object and component, with their
// physics/audio/script state), as GetSceneIndex originally did
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

namespace
{
    // Adds a scene holding one object and requests its load (not yet processed); returns the object
    GameObject::Ptr AddPendingSceneWithObject(const std::string &sceneName, const std::string &objectName)
    {
        auto scene = Scene::Create(sceneName);
        const auto go = GameObject::Create(objectName);
        scene->AddRootGameObject(go);
        SceneManager::AddScene(std::move(scene), true);
        return go;
    }

    nlohmann::json EmptySceneData(const std::string &sceneName)
    {
        return {{"name", sceneName}, {"rootGameObjects", nlohmann::json::array()}};
    }
}

// New data for a scene whose load is pending replaces the caller's instance: the load builds the new data
TEST(SceneStorageTest, UpdateSceneDropsAPendingInstance)
{
    const auto go = AddPendingSceneWithObject("SceneStorage_PendingUpdate", "FromInstance");
    nlohmann::json data = EmptySceneData("SceneStorage_PendingUpdate");
    SceneManager::UpdateScene("SceneStorage_PendingUpdate", data);
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "SceneStorage_PendingUpdate");
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("FromInstance"), nullptr);
    EXPECT_EQ(go->GetScene(), nullptr) << "the dropped instance let go of its objects";
}

TEST(SceneStorageTest, AddSceneJsonDropsAPendingInstance)
{
    const auto go = AddPendingSceneWithObject("SceneStorage_PendingJson", "FromInstance");
    nlohmann::json data = EmptySceneData("SceneStorage_PendingJson");
    SceneManager::AddScene(data);
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "SceneStorage_PendingJson");
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("FromInstance"), nullptr);
    EXPECT_EQ(go->GetScene(), nullptr);
}

TEST(SceneStorageTest, LoadingThePendingSceneAgainKeepsTheInstance)
{
    const auto go = AddPendingSceneWithObject("SceneStorage_LoadTwice", "Kept");
    SceneManager::LoadScene("SceneStorage_LoadTwice");
    SceneManager::LoadScene(SceneManager::GetSceneIndex("SceneStorage_LoadTwice"));
    SceneManager::ProcessAnyPendingSceneChange();

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(go->GetScene(), SceneManager::GetCurScene());
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("Kept"), go);
}

// Re-adding the loaded scene's name without loading replaces only the stored data
TEST(SceneStorageTest, ReAddingTheLoadedSceneWithoutLoadingLeavesItRunning)
{
    const auto go = AddPendingSceneWithObject("SceneStorage_Live2", "Live");
    SceneManager::ProcessAnyPendingSceneChange();
    const Scene *live = SceneManager::GetCurScene();
    ASSERT_NE(live, nullptr);

    SceneManager::AddScene(Scene::Create("SceneStorage_Live2"), false);
    SceneManager::ProcessAnyPendingSceneChange();

    EXPECT_EQ(SceneManager::GetCurScene(), live);
    EXPECT_EQ(go->GetScene(), live);
    const std::unique_ptr<Scene> stored = SceneManager::GetScene("SceneStorage_Live2");
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->FindGameObject("Live"), nullptr) << "the stored data is the re-added (empty) scene";
}

// Renaming a stored scene to another stored scene's name would make one unreachable by name
TEST(SceneStorageTest, UpdateSceneRefusesADuplicateName)
{
    SceneManager::AddScene(Scene::Create("SceneStorage_NameA"), false);
    SceneManager::AddScene(Scene::Create("SceneStorage_NameB"), false);
    const int indexA = SceneManager::GetSceneIndex("SceneStorage_NameA");
    const int indexB = SceneManager::GetSceneIndex("SceneStorage_NameB");

    nlohmann::json renamed = EmptySceneData("SceneStorage_NameA");
    SceneManager::UpdateScene(indexB, renamed);

    EXPECT_EQ(SceneManager::GetSceneIndex("SceneStorage_NameA"), indexA);
    EXPECT_EQ(SceneManager::GetSceneIndex("SceneStorage_NameB"), indexB);
    EXPECT_TRUE(SceneManager::HasScene("SceneStorage_NameB"));
    EXPECT_FALSE(SceneManager::HasScene("SceneStorage_NoSuchName"));
}
