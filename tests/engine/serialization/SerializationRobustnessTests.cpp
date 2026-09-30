#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

using namespace N2Engine;
using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Reference members
// ============================================================================

namespace
{
    class RefTarget final : public SerializableComponent
    {
    public:
        explicit RefTarget(GameObject &go) : SerializableComponent(go) {}
        [[nodiscard]] std::string GetTypeName() const override { return "RefTarget"; }
    };

    class RefHolder final : public SerializableComponent
    {
    public:
        explicit RefHolder(GameObject &go) : SerializableComponent(go)
        {
            RegisterGameObjectRef("target", target);
            RegisterComponentRef("component", component);
            RegisterComponentRefVector("components", components);
        }
        [[nodiscard]] std::string GetTypeName() const override { return "RefHolder"; }

        GameObject *target = nullptr;
        RefTarget *component = nullptr;
        std::vector<RefTarget *> components;
    };
}

TEST(SerializationReferencesTest, ReferencesRoundTripThroughTheResolver)
{
    const auto holderObject = GameObject::Create("Holder");
    const auto targetObject = GameObject::Create("Target");
    auto *holder = holderObject->AddComponent<RefHolder>();
    auto *target = targetObject->AddComponent<RefTarget>();
    holder->target = targetObject.get();
    holder->component = target;
    holder->components = {target, nullptr};

    const json data = holder->Serialize();

    const auto loadedObject = GameObject::Create("Loaded");
    auto *loaded = loadedObject->AddComponent<RefHolder>();
    ReferenceResolver resolver;
    resolver.RegisterGameObject(targetObject->GetUUID(), targetObject.get());
    resolver.RegisterComponent(target->GetUUID(), target);
    loaded->Deserialize(data, &resolver);
    resolver.ResolveAll();

    // The GameObject reference used to be captured by value, so loading wrote to a dead copy
    EXPECT_EQ(loaded->target, targetObject.get());
    EXPECT_EQ(loaded->component, target);
    ASSERT_EQ(loaded->components.size(), 2u);
    EXPECT_EQ(loaded->components[0], target);
    EXPECT_EQ(loaded->components[1], nullptr);
}

TEST(SerializationReferencesTest, NullReferencesStayNull)
{
    const auto holderObject = GameObject::Create("EmptyHolder");
    auto *holder = holderObject->AddComponent<RefHolder>();
    const json data = holder->Serialize();

    const auto loadedObject = GameObject::Create("EmptyLoaded");
    auto *loaded = loadedObject->AddComponent<RefHolder>();
    loaded->target = holderObject.get(); // must be cleared by loading a null reference
    ReferenceResolver resolver;
    loaded->Deserialize(data, &resolver);
    resolver.ResolveAll();

    EXPECT_EQ(loaded->target, nullptr);
    EXPECT_EQ(loaded->component, nullptr);
}

// ============================================================================
// Scene loading with malformed data
// ============================================================================

TEST(SceneLoadingTest, MalformedSceneFailsWithoutLosingTheCurrentOne)
{
    SceneManager::AddScene(Scene::Create("Robust_Good"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    ASSERT_EQ(SceneManager::GetCurSceneRef().sceneName, "Robust_Good");

    // A root object missing its uuid: const operator[] on it used to assert/abort
    SceneManager::AddScene(Scene::Create("Robust_Bad"), false);
    const int badIndex = SceneManager::GetSceneIndex("Robust_Bad");
    ASSERT_NE(badIndex, -1);
    json bad = {{"name", "Robust_Bad"}, {"rootGameObjects", json::array({json{{"name", "NoUuid"}}})}};
    SceneManager::UpdateScene(badIndex, bad);

    SceneManager::LoadScene(badIndex);
    EXPECT_NO_THROW(SceneManager::ProcessAnyPendingSceneChange());

    ASSERT_NE(SceneManager::GetCurScene(), nullptr) << "the failed load cleared the current scene";
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "Robust_Good");
}

TEST(SceneLoadingTest, FromJsonReturnsNullForMalformedData)
{
    EXPECT_EQ(Scene::FromJSON(json{{"name", "X"}, {"rootGameObjects", json::array({json{{"uuid", 5}}})}}), nullptr);
}

// ============================================================================
// ResourceLoader
// ============================================================================

class ResourceLoaderTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static fs::path Asset(const std::string &relative) { return s_root / "assets" / relative; }

    static void Write(const std::string &relative, const std::string &contents)
    {
        fs::create_directories(Asset(relative).parent_path());
        std::ofstream(Asset(relative), std::ios::binary) << contents;
    }

    void SetUp() override
    {
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_loader_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        Write("scripts/thing.lua", "return {}");
        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "ResourceLoaderTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }
};

TEST_F(ResourceLoaderTest, CorruptMetadataIsRegenerated)
{
    // Corrupt every .meta the first scan wrote, then rescan: it used to throw out of Initialize
    int corrupted = 0;
    for (const auto &entry : fs::recursive_directory_iterator(s_root))
    {
        if (entry.path().extension() == ".meta")
        {
            std::ofstream(entry.path(), std::ios::trunc) << "{ not json";
            ++corrupted;
        }
    }
    ASSERT_GT(corrupted, 0);

    EXPECT_NO_THROW(IO::ResourceLoader::Instance().Initialize(s_root));
    EXPECT_NE(IO::ResourceLoader::Instance().Load<LuaScript>(IO::ResourcePath("res://scripts/thing.lua")), nullptr);
}

TEST_F(ResourceLoaderTest, ReloadRefreshesMetadata)
{
    const IO::ResourcePath path("res://scripts/thing.lua");
    ASSERT_NE(IO::ResourceLoader::Instance().Load<LuaScript>(path), nullptr);

    Write("scripts/thing.lua", "return { changed = true }");
    fs::last_write_time(Asset("scripts/thing.lua"), fs::last_write_time(Asset("scripts/thing.lua")) + std::chrono::seconds(5));
    ASSERT_TRUE(IO::ResourceLoader::Instance().HasSourceChanged(path));

    ASSERT_TRUE(IO::ResourceLoader::Instance().Reload(path));

    // Reload used to discard the refreshed metadata, so this stayed true forever
    EXPECT_FALSE(IO::ResourceLoader::Instance().HasSourceChanged(path));
}

TEST(ResourcePathTest, EquivalentSpellingsAreOnePath)
{
    const IO::ResourcePath canonical("res://scripts/thing.lua");

    // Resolve accepted these but lookups keyed on the raw spelling reported "Resource not found"
    EXPECT_EQ(IO::ResourcePath("res://scripts/../scripts/thing.lua"), canonical);
    EXPECT_EQ(IO::ResourcePath("res://scripts//./thing.lua"), canonical);
    EXPECT_EQ(IO::ResourcePath("res://scripts\\thing.lua"), canonical);
    EXPECT_EQ(canonical.GetPath(), "scripts/thing.lua");
    EXPECT_EQ(IO::ResourcePath("res://../outside.txt").GetPath(), "../outside.txt"); // still rejected by Resolve
}

TEST(ResourcesTest, AssetRegisteredUnderTwoPathsIsEvicted)
{
    auto asset = GameObject::Create("TwoPaths");
    const std::weak_ptr<GameObject> watcher = asset;
    IO::Resources::Instance().RegisterAsset(asset, "two_paths_a.asset");
    IO::Resources::Instance().RegisterAsset(asset, "two_paths_b.asset");

    asset.reset();
    IO::Resources::Instance().RemoveUnused();

    // Two path entries counted as outside references, so it was never evicted
    EXPECT_TRUE(watcher.expired());
    EXPECT_EQ(IO::Resources::Instance().GetAsset<GameObject>(std::filesystem::path("two_paths_a.asset")), nullptr);
    EXPECT_EQ(IO::Resources::Instance().GetAsset<GameObject>(std::filesystem::path("two_paths_b.asset")), nullptr);
}

TEST_F(ResourceLoaderTest, RemoveUnusedEvictsOnlyUnreferencedAssets)
{
    const IO::ResourcePath path("res://scripts/thing.lua");
    auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);

    IO::ResourceLoader::Instance().RemoveUnused();
    EXPECT_NE(IO::ResourceLoader::Instance().GetCached<LuaScript>(path), nullptr) << "still referenced";

    script.reset();
    IO::ResourceLoader::Instance().RemoveUnused();
    // Both caches hold a reference, so use_count was never <= 1 and nothing was ever evicted
    EXPECT_EQ(IO::ResourceLoader::Instance().GetCached<LuaScript>(path), nullptr);
}

TEST_F(ResourceLoaderTest, ResourcePathsCannotEscapeTheAssetsRoot)
{
    auto &loader = IO::ResourceLoader::Instance();

    EXPECT_TRUE(loader.Resolve(IO::ResourcePath("res://../../outside.txt")).empty());
    EXPECT_TRUE(loader.Resolve(IO::ResourcePath("res://scripts/../../outside.txt")).empty());

    EXPECT_EQ(loader.Resolve(IO::ResourcePath("res://scripts/../scripts/thing.lua")),
              (s_root / "assets" / "scripts" / "thing.lua").lexically_normal());
}

// ============================================================================
// One asset registry: IO::Resources sees what IO::ResourceLoader loaded and vice versa
// ============================================================================

namespace
{
    class ScriptHolder final : public SerializableComponent
    {
    public:
        explicit ScriptHolder(GameObject &go) : SerializableComponent(go) { RegisterAssetRef("script", script); }
        [[nodiscard]] std::string GetTypeName() const override { return "ScriptHolder"; }

        std::shared_ptr<LuaScript> script;
    };
}

TEST_F(ResourceLoaderTest, ProjectAssetLoadedThroughResourceLoaderIsFoundThroughResources)
{
    const IO::ResourcePath path("res://scripts/thing.lua");
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);

    // The two registries used to be unconnected, so these all missed
    EXPECT_EQ(IO::Resources::Instance().GetAsset<LuaScript>(script->GetUUID()), script);
    EXPECT_EQ(IO::Resources::Instance().GetAsset<LuaScript>(fs::path("res://scripts/thing.lua")), script);
    EXPECT_EQ(IO::Resources::Instance().GetAsset<LuaScript>(Asset("scripts/thing.lua")), script);
}

TEST_F(ResourceLoaderTest, ProjectAssetLoadedThroughResourcesHasItsMetaUUID)
{
    const IO::ResourcePath path("res://scripts/thing.lua");
    const auto script = IO::Resources::Instance().Load<LuaScript>(Asset("scripts/thing.lua"));
    ASSERT_NE(script, nullptr);

    // It used to be a second copy under a random UUID that ResourceLoader knew nothing about
    EXPECT_EQ(script->GetUUID(), IO::ResourceLoader::Instance().GetUUID(path));
    EXPECT_EQ(IO::ResourceLoader::Instance().GetCached<LuaScript>(path), script);
    EXPECT_EQ(IO::ResourceLoader::Instance().LoadByUUID<LuaScript>(script->GetUUID()), script);
}

TEST_F(ResourceLoaderTest, AssetRefToAnAssetLoadedThroughResourcesResolvesInALaterRun)
{
    json saved;
    Math::UUID scriptUUID;
    {
        const auto script = IO::Resources::Instance().Load<LuaScript>(Asset("scripts/thing.lua"));
        ASSERT_NE(script, nullptr);
        scriptUUID = script->GetUUID();

        const auto go = GameObject::Create("ScriptHolderObject");
        auto *holder = go->AddComponent<ScriptHolder>();
        holder->script = script;
        saved = holder->Serialize();
    }

    // A later run: nothing is loaded, so the reference must resolve by the stable (meta) UUID
    IO::ResourceLoader::Instance().ClearCache();

    const auto go = GameObject::Create("ReloadedScriptHolder");
    auto *holder = go->AddComponent<ScriptHolder>();
    holder->Deserialize(saved, nullptr);

    ASSERT_NE(holder->script, nullptr);
    EXPECT_EQ(holder->script->GetUUID(), scriptUUID);
}

TEST(ResourcesTest, AssetRefResolvesAnAssetRegisteredAtRuntime)
{
    const auto script = std::make_shared<LuaScript>("return {}");
    IO::Resources::Instance().RegisterAsset(script);

    const auto go = GameObject::Create("RuntimeScriptHolder");
    auto *holder = go->AddComponent<ScriptHolder>();
    holder->script = script;
    const json saved = holder->Serialize();

    const auto loadedObject = GameObject::Create("LoadedRuntimeScriptHolder");
    auto *loaded = loadedObject->AddComponent<ScriptHolder>();
    loaded->Deserialize(saved, nullptr);

    EXPECT_EQ(loaded->script, script);
    IO::Resources::Instance().UnregisterAsset(script->GetUUID());
}

// ============================================================================
// Audio asset references across runs
// ============================================================================

namespace
{
    void WriteSilentWav(const fs::path &path, const int frames)
    {
        const auto dataSize = static_cast<std::uint32_t>(frames * 2);
        std::ofstream out(path, std::ios::binary);
        const auto le32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
        const auto le16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
        out.write("RIFF", 4);
        le32(36 + dataSize);
        out.write("WAVEfmt ", 8);
        le32(16);
        le16(1);         // PCM
        le16(1);         // mono
        le32(22050);     // sample rate
        le32(22050 * 2); // byte rate
        le16(2);         // block align
        le16(16);        // bits per sample
        out.write("data", 4);
        le32(dataSize);
        for (int i = 0; i < frames; ++i)
        {
            le16(0);
        }
    }
}

class AudioAssetReferenceTest : public ResourceLoaderTest
{
protected:
    void SetUp() override
    {
#ifdef _WIN32
        _putenv_s("ALSOFT_DRIVERS", "null");
#else
        setenv("ALSOFT_DRIVERS", "null", 1);
#endif
        ASSERT_TRUE(Audio::AudioSystem::Instance().Initialize());
        fs::create_directories(fs::temp_directory_path());
        ResourceLoaderTest::SetUp();
        fs::create_directories(Asset("sfx"));
        WriteSilentWav(Asset("sfx/beep.wav"), 2205);
        IO::ResourceLoader::Instance().RescanAssets();
    }

    void TearDown() override
    {
        ResourceLoaderTest::TearDown(); // releases cached clips while the context is alive
        Audio::AudioSystem::Instance().Shutdown();
    }
};

TEST_F(AudioAssetReferenceTest, AudioFilesLoadByExtension)
{
    // The audio loader was dropped at link time (nothing referenced its file) and only registered
    // with IO::Resources, so res:// audio never loaded
    const auto clip = IO::ResourceLoader::Instance().Load<Audio::AudioClip>(IO::ResourcePath("res://sfx/beep.wav"));
    ASSERT_NE(clip, nullptr);
    EXPECT_TRUE(clip->IsLoaded());
}

TEST_F(AudioAssetReferenceTest, AudioSourceClipResolvesInALaterRun)
{
    json saved;
    Math::UUID clipUUID;
    {
        const auto clip = IO::ResourceLoader::Instance().Load<Audio::AudioClip>(IO::ResourcePath("res://sfx/beep.wav"));
        ASSERT_NE(clip, nullptr);
        clipUUID = clip->GetUUID();

        const auto go = GameObject::Create("Speaker");
        auto *source = go->AddComponent<Audio::AudioSource>();
        source->SetClip(clip);
        saved = source->Serialize();
    }

    // A later run: nothing is cached; the clip must be found again by its stable (meta) UUID
    IO::ResourceLoader::Instance().ClearCache();

    const auto go = GameObject::Create("Reloaded Speaker");
    auto *source = go->AddComponent<Audio::AudioSource>();
    source->Deserialize(saved, nullptr);

    ASSERT_NE(source->GetClip(), nullptr) << "looked up in IO::Resources, whose UUIDs are random per run";
    EXPECT_EQ(source->GetClip()->GetUUID(), clipUUID);
}
