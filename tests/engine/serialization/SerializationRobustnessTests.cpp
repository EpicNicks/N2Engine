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
#include "engine/prefabs/Prefab.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/JsonLimits.hpp"
#include "DeepSceneJson.hpp"
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

TEST(ResourcePathTest, AbsolutePathsKeepTheirRoot)
{
    // A POSIX path is only absolute on POSIX, so the type is given explicitly; the normalisation is the same
    const IO::ResourcePath posix(IO::PathType::Absolute, "/home/user/../user/game//save.json");
    EXPECT_EQ(posix.GetPath(), "/home/user/game/save.json") << "the leading '/' was stripped, making it relative";
    EXPECT_EQ(posix.ToString(), "/home/user/game/save.json");
    EXPECT_EQ(posix.GetParent().GetPath(), "/home/user/game");
    EXPECT_EQ(IO::ResourcePath(IO::PathType::Absolute, "/home/user/").GetPath(), "/home/user");
    EXPECT_EQ(IO::ResourcePath(IO::PathType::Absolute, "/").GetPath(), "/") << "a bare root lost its separator";

#ifdef _WIN32
    // A UNC share keeps both leading separators
    EXPECT_EQ(IO::ResourcePath(IO::PathType::Absolute, "\\\\server\\share\\file.txt").GetPath(), "//server/share/file.txt");
#endif

    // res:// and user:// paths still drop leading separators: they are relative to their root
    EXPECT_EQ(IO::ResourcePath("res:///scripts/thing.lua").GetPath(), "scripts/thing.lua");
    EXPECT_EQ(IO::ResourcePath("user:////save.json").GetPath(), "save.json");

    // A path that is absolute on this platform goes through the string constructor unchanged in form
    const std::string native = std::filesystem::absolute("save.json").generic_string();
    const IO::ResourcePath fromString(native);
    EXPECT_EQ(fromString.GetType(), IO::PathType::Absolute);
    EXPECT_EQ(fromString.GetPath(), std::filesystem::path(native).lexically_normal().generic_string());
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
    // Unregistered however the test ends, so no later test sees it
    const std::shared_ptr<void> unregister(nullptr, [&script](void *)
    {
        IO::Resources::Instance().UnregisterAsset(script->GetUUID());
    });

    const auto go = GameObject::Create("RuntimeScriptHolder");
    auto *holder = go->AddComponent<ScriptHolder>();
    holder->script = script;
    const json saved = holder->Serialize();

    const auto loadedObject = GameObject::Create("LoadedRuntimeScriptHolder");
    auto *loaded = loadedObject->AddComponent<ScriptHolder>();
    loaded->Deserialize(saved, nullptr);

    EXPECT_EQ(loaded->script, script);
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

// ============================================================================
// Nesting caps (JsonLimits.hpp): scene, prefab and snapshot data is untrusted
// ============================================================================

namespace
{
    using JsonLimits::MaxHierarchyDepth;
    using JsonLimits::MaxSceneJsonDepth;

    // The importers allow node trees 256 levels deep (assetimport kMaxNodeDepth), which a model places under a few
    // scene objects: the hierarchy cap keeps a margin above that, and the text cap follows from it
    static_assert(MaxHierarchyDepth >= 256 + 32);
    static_assert(MaxSceneJsonDepth == 2 * MaxHierarchyDepth + 64);

    // How many levels a GameObject chain has: walked down the first children, no recursion
    std::size_t ChainLevels(const GameObject::Ptr &root)
    {
        std::size_t levels = 0;
        for (GameObject::Ptr current = root; current != nullptr;
             current = current->GetChildCount() > 0 ? current->GetChild(0) : nullptr)
        {
            ++levels;
        }
        return levels;
    }

    // A chain of `levels` GameObjects made with AddChild (no JSON)
    GameObject::Ptr BuiltChain(const std::size_t levels, GameObject::Ptr *deepest = nullptr)
    {
        const GameObject::Ptr root = GameObject::Create("root");
        GameObject::Ptr current = root;
        for (std::size_t level = 1; level < levels; ++level)
        {
            const GameObject::Ptr child = GameObject::Create("n");
            current->AddChild(child, false);
            current = child;
        }
        if (deepest != nullptr)
        {
            *deepest = current;
        }
        return root;
    }

    // The 100 000-level chain of the audit (several MB of text), parsed (the parser is iterative)
    const json &VeryDeepChain()
    {
        static const json chain = json::parse(TestJson::ChainText(100000));
        return chain;
    }
}

TEST(SceneDepthTest, JsonScannerCountsNestingOutsideStringsOnly)
{
    EXPECT_FALSE(JsonLimits::NestsDeeperThan("[[[[1]]]]", 4));
    EXPECT_TRUE(JsonLimits::NestsDeeperThan("[[[[[1]]]]]", 4));
    // brackets, braces and escaped quotes inside strings do not count, and an escaped quote does not end the string
    EXPECT_FALSE(JsonLimits::NestsDeeperThan(R"({"a":"[[[[[[\"[[{{","b":[1]})", 2));
    EXPECT_FALSE(JsonLimits::NestsDeeperThan(R"(["\\", "[[[[", {"k":"}}}}"}])", 2));
    // nesting through component data counts as much as nesting through children
    const std::string inData = R"({"name":"s","rootGameObjects":[{"components":[{"type":"X","data":)" +
                               TestJson::NestedArrays(MaxSceneJsonDepth) + "}]}]}";
    EXPECT_TRUE(JsonLimits::SceneJsonTooDeep(inData));
    // the pre-scan of text and the walk of a parsed value agree
    EXPECT_TRUE(JsonLimits::SceneValueTooDeep(json::parse(inData)));
    EXPECT_EQ(JsonLimits::NestingDepth(json::parse(TestJson::NestedArrays(30))), 30u);
    EXPECT_EQ(JsonLimits::NestingDepth(json(5)), 0u);
}

TEST(SceneDepthTest, JsonTextAtTheCapIsKeptAndOnePastItIsRefusedBeforeParsing)
{
    EXPECT_FALSE(JsonLimits::SceneJsonTooDeep(TestJson::NestedArrays(MaxSceneJsonDepth)));
    EXPECT_TRUE(JsonLimits::SceneJsonTooDeep(TestJson::NestedArrays(MaxSceneJsonDepth + 1)));
    EXPECT_NE(JsonLimits::SceneJsonTooDeepMessage().find("the scene JSON nests more than 1088 levels deep"),
              std::string::npos);
    EXPECT_FALSE(JsonLimits::SceneJsonTooDeep(TestJson::SceneText(MaxHierarchyDepth)));
    EXPECT_TRUE(JsonLimits::SceneJsonTooDeep(TestJson::ChainText(100000)));
}

TEST(SceneDepthTest, HierarchyJustUnderTheCapDeserializesSerializesAndDumps)
{
    // The stack this runs on is the smallest the tests meet (MSVC Debug, 1 MiB for the main thread): Deserialize,
    // Serialize, dump, and the walks over the tree must all fit it at the deepest allowed tree
    for (const std::size_t levels : {MaxHierarchyDepth - 1, MaxHierarchyDepth})
    {
        const json data = json::parse(TestJson::ChainText(levels));
        const GameObject::Ptr root = GameObject::Deserialize(data);
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(ChainLevels(root), levels);
        EXPECT_NE(root->FindChildRecursive("leaf"), nullptr);
        EXPECT_EQ(root->GetChildrenRecursive().size(), levels - 1);

        const json saved = root->Serialize();
        const std::string text = saved.dump();
        EXPECT_EQ(JsonLimits::NestingDepth(json::parse(text)), 2 * levels);
        EXPECT_FALSE(JsonLimits::SceneJsonTooDeep(text));
        EXPECT_EQ(ChainLevels(GameObject::Deserialize(json::parse(text))), levels) << "it loads again from its own text";
    }
}

TEST(SceneDepthTest, HierarchyOnePastTheCapIsRefusedWithAnErrorNamingTheCap)
{
    const json data = json::parse(TestJson::ChainText(MaxHierarchyDepth + 1));
    try
    {
        (void)GameObject::Deserialize(data);
        FAIL() << "a hierarchy past the cap was built";
    }
    catch (const std::invalid_argument &error)
    {
        EXPECT_NE(std::string(error.what()).find("512"), std::string::npos) << error.what();
    }
}

TEST(SceneDepthTest, SceneFromJsonLoadsTheDeepestAllowedSceneAndRefusesOnePastIt)
{
    const auto allowed = Scene::FromJSON(json::parse(TestJson::SceneText(MaxHierarchyDepth)), true);
    ASSERT_NE(allowed, nullptr);
    ASSERT_EQ(allowed->GetRootGameObjectCount(), 1u);
    EXPECT_EQ(ChainLevels(allowed->GetRootGameObjects().front()), MaxHierarchyDepth);
    EXPECT_NE(allowed->FindGameObject("leaf"), nullptr);
    EXPECT_FALSE(allowed->Serialize().dump().empty());

    EXPECT_EQ(Scene::FromJSON(json::parse(TestJson::SceneText(MaxHierarchyDepth + 1)), true), nullptr);
    EXPECT_EQ(Scene::FromJSON(json::parse(TestJson::SceneText(MaxHierarchyDepth + 1)), false), nullptr);
}

TEST(SceneDepthTest, AHundredThousandLevelChainIsAnErrorOnEveryEngineEntryPoint)
{
    const json &chain = VeryDeepChain();
    EXPECT_THROW((void)GameObject::Deserialize(chain), std::invalid_argument);

    json scene = {{"name", "Deep"}, {"rootGameObjects", json::array()}};
    scene["rootGameObjects"].push_back(chain);
    EXPECT_EQ(Scene::FromJSON(scene, true), nullptr);
    EXPECT_EQ(Scene::FromJSON(scene, false), nullptr);

    const json prefab = {{"name", "Deep"}, {"rootObject", chain}};
    const auto parsed = Prefab::Deserialize(prefab);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), PrefabParseError::InvalidRootObject);
    EXPECT_EQ(PrefabManager::InstantiatePrefab(prefab), nullptr);
    EXPECT_EQ(PrefabManager::InstantiatePrefab(chain), nullptr);
}

TEST(SceneDepthTest, AFailedDeepLoadBuildsNothingAndLeavesNoPartialObjectsBehind)
{
    // Scene::FromJSON throws away the half-built tree: the scene the caller holds is the one it had
    auto scene = Scene::FromJSON(json::parse(TestJson::SceneText(2)), true);
    ASSERT_NE(scene, nullptr);
    const std::size_t roots = scene->GetRootGameObjectCount();
    EXPECT_EQ(Scene::FromJSON(json::parse(TestJson::SceneText(MaxHierarchyDepth + 1)), true), nullptr);
    EXPECT_EQ(scene->GetRootGameObjectCount(), roots);

    // and a resolver given to a refused Deserialize is the caller's, whose ResolveAll the throw skipped
    ReferenceResolver resolver;
    EXPECT_THROW((void)GameObject::Deserialize(json::parse(TestJson::ChainText(MaxHierarchyDepth + 1)), &resolver),
                 std::invalid_argument);
}

TEST(SceneDepthTest, ReparentingPastTheCapIsRefusedAndChangesNothing)
{
    GameObject::Ptr deepest;
    const GameObject::Ptr root = BuiltChain(MaxHierarchyDepth, &deepest);
    ASSERT_EQ(ChainLevels(root), MaxHierarchyDepth);

    // a leaf under the deepest object would be level cap + 1
    const GameObject::Ptr leaf = GameObject::Create("leaf");
    EXPECT_FALSE(deepest->CanAddChild(leaf));
    deepest->AddChild(leaf, false);
    EXPECT_EQ(leaf->GetParent(), nullptr);
    leaf->SetParent(deepest, false);
    EXPECT_EQ(leaf->GetParent(), nullptr);
    EXPECT_EQ(deepest->GetChildCount(), 0u);

    // a subtree two levels high does not fit under the parent of the deepest object, a leaf does
    const GameObject::Ptr above = deepest->GetParent();
    const GameObject::Ptr pair = GameObject::Create("pair");
    pair->AddChild(GameObject::Create("pairChild"), false);
    EXPECT_FALSE(above->CanAddChild(pair));
    above->AddChild(pair, false);
    EXPECT_EQ(pair->GetParent(), nullptr);
    EXPECT_TRUE(above->CanAddChild(leaf));

    // moving an object that has a parent already: it stays where it was when the move is refused
    const GameObject::Ptr other = GameObject::Create("other");
    const GameObject::Ptr moved = GameObject::Create("moved");
    moved->AddChild(GameObject::Create("movedChild"), false);
    other->AddChild(moved, false);
    moved->SetParent(above, false);
    EXPECT_EQ(moved->GetParent(), other);

    // one level shallower, the same kind of move is fine
    above->AddChild(leaf, false);
    EXPECT_EQ(leaf->GetParent(), above);
    EXPECT_EQ(ChainLevels(root), MaxHierarchyDepth) << "the chain is as it was";
}

TEST(SceneDepthTest, ASerializedDeepestTreeStaysUnderTheTextCap)
{
    // every GameObject level costs two JSON levels, so the deepest hierarchy fits the text cap with room for data
    GameObject::Ptr deepest;
    const GameObject::Ptr root = BuiltChain(MaxHierarchyDepth, &deepest);
    const auto scene = Scene::Create("Deepest");
    scene->AddRootGameObject(root);
    const std::string text = scene->Serialize().dump();
    EXPECT_FALSE(JsonLimits::SceneJsonTooDeep(text));
    EXPECT_LE(JsonLimits::NestingDepth(json::parse(text)), MaxSceneJsonDepth - 60);
}
