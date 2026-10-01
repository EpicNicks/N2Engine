#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include <math/Vector3.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// Scripts can keep GameObjects, components and collisions in globals or `self` for as long as they like.
// Once the object is gone, using what they kept must be a Lua error, not a use of freed memory.
class LuaHandleTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_handle_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // Keeps everything the engine hands it, like a script caching references would
        WriteAsset("handles/Collector.lua", R"(
            local Collector = {}
            Collector.__index = Collector

            function Collector:OnAttach()
                handle_self_go = self.gameObject
                handle_self_component = self.component
            end

            function Collector:OnCollisionEnter(collision)
                handle_collision = collision
            end

            function Collector:OnTriggerEnter(trigger)
                handle_trigger = trigger
            end

            return Collector
        )");

        // Reads its own object, position and a sibling component during teardown, like a script spawning
        // an effect where it died would
        WriteAsset("handles/TeardownReader.lua", R"(
            local Reader = {}
            Reader.__index = Reader

            local function Read(self)
                return {
                    name = self.gameObject:GetName(),
                    y = self.gameObject:GetPositionable():GetPosition().y,
                    radius = self.gameObject:GetComponent("SphereRenderer"):GetRadius(),
                }
            end

            function Reader:OnDisable()
                teardown_disable = Read(self)
            end

            function Reader:OnDestroy()
                teardown_destroy = Read(self)
                -- The scene being unloaded is still usable too (nil when the reader isn't torn down by a switch)
                if handle_scene ~= nil then
                    teardown_scene_name = handle_scene.sceneName
                end
                teardown_go = self.gameObject
                teardown_component = self.component
                teardown_renderer = self.gameObject:GetComponent("SphereRenderer")
            end

            return Reader
        )");

        // Collects garbage while it loads, i.e. while LuaComponent:SetScript is still running
        WriteAsset("handles/Collects.lua", R"(
            collectgarbage()
            collectgarbage()
            collects_ran = true
            local Collects = {}
            Collects.__index = Collects
            return Collects
        )");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    // Runs a chunk and fails the test with the Lua error message if it errors
    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    // Runs a chunk that should fail and returns its error message
    static std::string RunExpectingError(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "expected a Lua error from: " << code;
            return {};
        }
        const sol::error err = result;
        return err.what();
    }

    template <typename T>
    static T Eval(const std::string &expression)
    {
        const sol::protected_function_result result =
            Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
            return T{};
        }
        return result.get<T>();
    }

    static bool Contains(const std::string &text, const std::string &part)
    {
        return text.find(part) != std::string::npos;
    }

    Scene *_scene = nullptr;

    void SetUp() override
    {
        SceneManager::AddScene(Scene::Create("LuaHandle_" + std::string(
                                   ::testing::UnitTest::GetInstance()->current_test_info()->name())), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
    }

    void TearDown() override
    {
        for (const char *name : {"handle_go", "handle_other_go", "handle_rb", "handle_renderer", "handle_renderer_copy",
                                 "handle_pos", "handle_collision", "handle_trigger", "handle_self_go",
                                 "handle_self_component", "handle_parent", "handle_detached", "handle_plain",
                                 "handle_removed", "teardown_disable", "teardown_destroy", "teardown_go",
                                 "teardown_component", "teardown_renderer", "collected_component", "collects_ran",
                                 "handle_scene", "teardown_scene_name"})
        {
            Lua()[name] = sol::lua_nil;
        }
    }

    /// A root object of the test scene, owned only by the scene
    GameObjectRef Spawn(const std::string &name)
    {
        const auto go = GameObject::Create(name);
        _scene->AddRootGameObject(go);
        return GameObjectRef(go);
    }

    /// A scene object at y = 2 with a SphereRenderer (radius 1.5), then TeardownReader. The renderer comes
    /// first, so it's torn down before the script reads it.
    GameObject::Ptr SpawnTeardownReader(const std::string &name)
    {
        const auto go = GameObject::Create(name);
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(Math::Vector3(0.0f, 2.0f, 0.0f));
        go->AddComponent<Example::SphereRenderer>()->SetRadius(1.5f);
        auto *script = go->AddComponent<LuaComponent>();
        script->SetScript(IO::ResourcePath("res://handles/TeardownReader.lua"));
        EXPECT_FALSE(script->HasMissingScript());
        _scene->AddRootGameObject(go);
        return go;
    }

    /// What TeardownReader read in OnDisable or OnDestroy (teardown_disable / teardown_destroy)
    static void ExpectTeardownRead(const std::string &global, const std::string &name)
    {
        const sol::object read = Lua()[global];
        ASSERT_TRUE(read.is<sol::table>()) << global << " wasn't set: the script failed to read its handles";
        const sol::table table = read.as<sol::table>();
        EXPECT_EQ(table.get<std::string>("name"), name);
        EXPECT_FLOAT_EQ(table.get<float>("y"), 2.0f);
        EXPECT_FLOAT_EQ(table.get<float>("radius"), 1.5f);
    }
};

TEST_F(LuaHandleTest, LiveHandlesKeepWorking)
{
    Lua()["handle_go"] = Spawn("Alive");
    Run(R"(
        handle_renderer = handle_go:AddComponent("SphereRenderer")
        handle_renderer:SetRadius(1.5)
        handle_go:CreatePositionable()
        handle_go:GetPositionable():SetPosition(Vector3(1, 2, 3))
    )");

    EXPECT_TRUE(Eval<bool>("handle_go:IsValid() and handle_renderer:IsValid()"));
    EXPECT_FALSE(Eval<bool>("handle_renderer:IsDestroyed()"));
    EXPECT_FLOAT_EQ(Eval<float>("handle_renderer:GetRadius()"), 1.5f);
    EXPECT_EQ(Eval<std::string>("handle_renderer:GetGameObject():GetName()"), "Alive");
    EXPECT_FLOAT_EQ(Eval<float>("handle_go:GetPositionable():GetPosition().y"), 2.0f);
}

TEST_F(LuaHandleTest, CachedComponentErrorsAfterRemoveComponent)
{
    const auto go = GameObject::Create("Remover");
    Lua()["handle_go"] = GameObjectRef(go);
    Run(R"(handle_rb = handle_go:AddComponent("Rigidbody"))");
    ASSERT_TRUE(Eval<bool>("handle_rb:IsValid()"));

    ASSERT_TRUE(go->RemoveComponent<Physics::Rigidbody>());

    EXPECT_FALSE(Eval<bool>("handle_rb:IsValid()"));
    EXPECT_TRUE(Eval<bool>("handle_rb:IsDestroyed()"));
    // Used to call SetMass on the freed Rigidbody
    const std::string error = RunExpectingError("handle_rb:SetMass(2)");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed Rigidbody")) << error;

    // The object itself is unaffected
    EXPECT_TRUE(Eval<bool>("handle_go:GetComponent('Rigidbody') == nil"));
}

TEST_F(LuaHandleTest, CachedHandlesErrorAfterDestroy)
{
    Lua()["handle_go"] = Spawn("Doomed");
    Run(R"(
        handle_renderer = handle_go:AddComponent("SphereRenderer")
        handle_go:CreatePositionable()
        handle_pos = handle_go:GetPositionable()
        handle_go:Destroy()
    )");

    // Destroy takes effect at the end of the frame, as in C++
    EXPECT_TRUE(Eval<bool>("handle_go:IsValid()"));
    _scene->ProcessDestroyed();

    EXPECT_FALSE(Eval<bool>("handle_go:IsValid()"));
    EXPECT_FALSE(Eval<bool>("handle_renderer:IsValid()"));
    EXPECT_FALSE(Eval<bool>("handle_pos:IsValid()"));

    std::string error = RunExpectingError("handle_go:GetName()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed GameObject")) << error;
    error = RunExpectingError("handle_renderer:GetRadius()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed SphereRenderer")) << error;
    error = RunExpectingError("handle_pos:GetPosition()");
    EXPECT_TRUE(Contains(error, "destroyed GameObject")) << error;
    // IsActive too: it used to answer false for a destroyed object, now it's an error like any other use
    error = RunExpectingError("handle_go:IsActive()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed GameObject")) << error;
    error = RunExpectingError("handle_renderer:IsActive()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed SphereRenderer")) << error;

    // Destroying it again is harmless
    Run("handle_go:Destroy()");
}

TEST_F(LuaHandleTest, CachedHandlesErrorAfterSceneSwitch)
{
    Lua()["handle_go"] = Spawn("Switched");
    Run(R"(handle_renderer = handle_go:AddComponent("SphereRenderer"))");

    SceneManager::AddScene(Scene::Create("LuaHandle_SwitchTarget"), true);
    SceneManager::ProcessAnyPendingSceneChange(); // frees the old scene's objects and components
    _scene = nullptr;

    EXPECT_FALSE(Eval<bool>("handle_go:IsValid()"));
    EXPECT_FALSE(Eval<bool>("handle_renderer:IsValid()"));
    const std::string error = RunExpectingError("handle_renderer:SetRadius(2)");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed SphereRenderer")) << error;
}

TEST_F(LuaHandleTest, StoredCollisionOutlivesItsObjects)
{
    const auto receiver = GameObject::Create("Receiver");
    auto *collector = receiver->AddComponent<LuaComponent>();
    collector->SetScript(IO::ResourcePath("res://handles/Collector.lua"));
    ASSERT_FALSE(collector->HasMissingScript());
    _scene->AddRootGameObject(receiver);

    auto hit = GameObject::Create("Hit");
    auto *hitBody = hit->AddComponent<Physics::Rigidbody>();
    _scene->AddRootGameObject(hit);

    Physics::Collision collision;
    collision.gameObject = receiver.get();
    collision.otherGameObject = hit.get();
    collision.otherRigidbody = hitBody;
    collector->OnCollisionEnter(collision);

    Physics::Trigger trigger;
    trigger.gameObject = receiver.get();
    trigger.otherGameObject = hit.get();
    trigger.otherRigidbody = hitBody;
    collector->OnTriggerEnter(trigger);

    ASSERT_EQ(Eval<std::string>("handle_collision.otherGameObject:GetName()"), "Hit");
    EXPECT_TRUE(Eval<bool>("handle_collision.rigidbody == nil")) << "no Rigidbody is nil, as before";

    hit->Destroy();
    hit.reset();
    _scene->ProcessDestroyed(); // frees Hit and its Rigidbody

    // The script kept both past the callback; they used to hold raw pointers to the freed objects
    EXPECT_EQ(Eval<std::string>("handle_collision.gameObject:GetName()"), "Receiver");
    EXPECT_FALSE(Eval<bool>("handle_collision.otherGameObject:IsValid()"));
    EXPECT_FALSE(Eval<bool>("handle_collision.otherRigidbody:IsValid()"));
    EXPECT_FALSE(Eval<bool>("handle_trigger.otherGameObject:IsValid()"));

    std::string error = RunExpectingError("handle_collision.otherGameObject:GetName()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed GameObject")) << error;
    error = RunExpectingError("handle_trigger.otherRigidbody:AddForce(Vector3(0, 1, 0))");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed Rigidbody")) << error;
}

TEST_F(LuaHandleTest, CopiesOfSelfErrorAfterDestroy)
{
    const auto go = GameObject::Create("SelfCopier");
    auto *collector = go->AddComponent<LuaComponent>();
    collector->SetScript(IO::ResourcePath("res://handles/Collector.lua"));
    ASSERT_FALSE(collector->HasMissingScript());
    _scene->AddRootGameObject(go);
    _scene->ProcessAttachQueue(); // OnAttach copies self.gameObject and self.component into globals

    ASSERT_EQ(Eval<std::string>("handle_self_go:GetName()"), "SelfCopier");
    ASSERT_TRUE(Eval<bool>("handle_self_component:IsValid()"));

    go->Destroy();
    _scene->ProcessDestroyed();

    // `go` still keeps the object's memory alive here, but it's destroyed
    EXPECT_FALSE(Eval<bool>("handle_self_go:IsValid()"));
    const std::string error = RunExpectingError("handle_self_component:GetScriptPath()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed LuaComponent")) << error;
}

TEST_F(LuaHandleTest, HandlesToTheSameObjectAreEqual)
{
    Lua()["handle_go"] = Spawn("Same");
    Lua()["handle_other_go"] = Spawn("Different");
    Run(R"(
        handle_renderer = handle_go:AddComponent("SphereRenderer")
        handle_renderer_copy = handle_go:GetComponent("SphereRenderer")
    )");

    // Separate userdata for the same object
    EXPECT_TRUE(Eval<bool>("handle_go == SceneManager.GetCurrentScene():FindGameObject('Same')"));
    EXPECT_FALSE(Eval<bool>("handle_go == handle_other_go"));
    EXPECT_TRUE(Eval<bool>("handle_renderer == handle_renderer_copy"));
    EXPECT_TRUE(Eval<bool>("handle_renderer:GetGameObject() == handle_go"));
    EXPECT_FALSE(Eval<bool>("handle_renderer == handle_go")) << "different kinds of object";

    // Identity survives the object
    Run("handle_go:Destroy()");
    _scene->ProcessDestroyed();
    EXPECT_TRUE(Eval<bool>("handle_renderer == handle_renderer_copy"));
    EXPECT_FALSE(Eval<bool>("handle_go == handle_other_go"));
}

TEST_F(LuaHandleTest, DetachedChildStaysUsable)
{
    const auto parent = GameObject::Create("Parent");
    parent->AddChild(GameObject::Create("Child"));
    Lua()["handle_parent"] = GameObjectRef(parent);

    Run(R"(
        local child = handle_parent:FindChild("Child")
        handle_parent:RemoveChild(child)
        handle_detached = child
    )");

    // The parent was its only owner; the script's reference keeps it alive, as before
    EXPECT_TRUE(parent->GetChildren().empty());
    EXPECT_EQ(Eval<std::string>("handle_detached:GetName()"), "Child");
}

// Handles work throughout a teardown (OnDisable/OnDestroy), including to components torn down before the
// script's own, and fail once it's over. IsDestroyed() is already true during those callbacks, which used
// to make self.gameObject unusable in OnDestroy.
TEST_F(LuaHandleTest, HandlesWorkDuringTeardownOfADestroyedObject)
{
    const auto go = SpawnTeardownReader("Exploding");

    go->Destroy();
    _scene->ProcessDestroyed();

    ExpectTeardownRead("teardown_disable", "Exploding");
    ExpectTeardownRead("teardown_destroy", "Exploding");

    // `go` still keeps the object's memory alive, but its teardown is over
    EXPECT_FALSE(Eval<bool>("teardown_go:IsValid()"));
    EXPECT_FALSE(Eval<bool>("teardown_renderer:IsValid()"));
    EXPECT_FALSE(Eval<bool>("teardown_component:IsValid()"));
    const std::string error = RunExpectingError("teardown_go:GetName()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed GameObject")) << error;
}

TEST_F(LuaHandleTest, HandlesWorkDuringTeardownOnSceneSwitch)
{
    const auto go = SpawnTeardownReader("Unloaded");

    SceneManager::AddScene(Scene::Create("LuaHandle_TeardownSwitchTarget"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    _scene = nullptr;

    ExpectTeardownRead("teardown_disable", "Unloaded");
    ExpectTeardownRead("teardown_destroy", "Unloaded");

    EXPECT_FALSE(Eval<bool>("teardown_go:IsValid()"));
    EXPECT_FALSE(Eval<bool>("teardown_renderer:IsValid()"));
    const std::string error = RunExpectingError("teardown_renderer:GetRadius()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed SphereRenderer")) << error;
}

TEST_F(LuaHandleTest, HandlesWorkDuringTeardownOfARemovedComponent)
{
    const auto go = SpawnTeardownReader("Removing");

    ASSERT_TRUE(go->RemoveComponent<LuaComponent>());

    ExpectTeardownRead("teardown_disable", "Removing");
    ExpectTeardownRead("teardown_destroy", "Removing");

    // Only the removed component is gone
    EXPECT_FALSE(Eval<bool>("teardown_component:IsValid()"));
    EXPECT_TRUE(Eval<bool>("teardown_go:IsValid() and teardown_renderer:IsValid()"));
    const std::string error = RunExpectingError("teardown_component:GetScriptPath()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed LuaComponent")) << error;
}

TEST_F(LuaHandleTest, ConcreteAndPlainComponentHandlesAreEqual)
{
    const auto go = GameObject::Create("Plain");
    auto *renderer = go->AddComponent<Example::SphereRenderer>();
    Lua()["handle_renderer"] = ComponentRef<Example::SphereRenderer>(*renderer);
    // What ComponentToLua returns for a component type without its own Lua type
    Lua()["handle_plain"] = ComponentRef<Component>(*renderer);

    EXPECT_TRUE(Eval<bool>("handle_renderer == handle_plain"));
    EXPECT_TRUE(Eval<bool>("handle_plain == handle_renderer"));
    EXPECT_EQ(Eval<std::string>("handle_plain:GetGameObject():GetName()"), "Plain");
}

TEST_F(LuaHandleTest, RemoveRootGameObjectHandsOwnershipToTheHandle)
{
    Lua()["handle_go"] = Spawn("Root");

    Run("handle_removed = SceneManager.GetCurrentScene():RemoveRootGameObject(handle_go)");
    ASSERT_TRUE(Eval<bool>("handle_removed"));
    // The scene was its only owner
    EXPECT_EQ(Eval<std::string>("handle_go:GetName()"), "Root");

    // Back in the scene, which owns it again
    Run("SceneManager.GetCurrentScene():AddRootGameObject(handle_go)");
    const std::weak_ptr<GameObject> root = _scene->FindGameObject("Root");
    ASSERT_FALSE(root.expired());
    Run("handle_go = nil collectgarbage() collectgarbage()");
    EXPECT_FALSE(root.expired()) << "the scene owns it";

    // Not a root (any more): nothing to remove
    EXPECT_FALSE(Eval<bool>("SceneManager.GetCurrentScene():RemoveRootGameObject(GameObject.Create('Loose'))"));
}

TEST_F(LuaHandleTest, LuaOwnedObjectIsFreedWithItsLastHandle)
{
    Run("handle_go = GameObject.Create('Temporary')");
    const std::weak_ptr<GameObject> object = Lua()["handle_go"].get<GameObjectRef>().Lock();
    ASSERT_FALSE(object.expired());

    Run("handle_go = nil collectgarbage() collectgarbage()");

    EXPECT_TRUE(object.expired());
}

TEST_F(LuaHandleTest, ComponentCallKeepsItsGameObjectAlive)
{
    // Nothing references the new GameObject once AddComponent returns, so the GC inside the script's load
    // collects its handle. SetScript must keep the object (so the component) alive until it returns.
    Run(R"(
        collected_component = GameObject.Create("Collected"):AddComponent("LuaComponent")
        collected_component:SetScript("res://handles/Collects.lua")
    )");

    EXPECT_TRUE(Eval<bool>("collects_ran"));
    // Nothing owns the GameObject any more, so it's freed at the latest by the next collection
    Run("collectgarbage() collectgarbage()");
    EXPECT_FALSE(Eval<bool>("collected_component:IsValid()"));
}

TEST_F(LuaHandleTest, BadArgumentsAreCleanErrors)
{
    Lua()["handle_go"] = Spawn("Parent");

    std::string error = RunExpectingError("handle_go:AddChild(nil)");
    EXPECT_TRUE(Contains(error, "expected a GameObject, got nil")) << error;
    error = RunExpectingError("SceneManager.GetCurrentScene():AddRootGameObject(nil)");
    EXPECT_TRUE(Contains(error, "expected a GameObject, got nil")) << error;
    EXPECT_FALSE(Eval<bool>("SceneManager.GetCurrentScene():RemoveRootGameObject(nil)"));
    EXPECT_FALSE(Eval<bool>("SceneManager.GetCurrentScene():DestroyGameObject(nil)"));

    // `.` instead of `:`, and a handle of the wrong type, fail sol's argument checks
    RunExpectingError("handle_go.GetName()");
    RunExpectingError("handle_go:AddComponent('SphereRenderer').GetRadius(handle_go)");
}

TEST_F(LuaHandleTest, DestroyGameObjectIsFalseForAnAlreadyDestroyedObject)
{
    const auto go = GameObject::Create("Twice");
    _scene->AddRootGameObject(go);
    Lua()["handle_go"] = GameObjectRef(go);

    ASSERT_TRUE(Eval<bool>("SceneManager.GetCurrentScene():DestroyGameObject(handle_go)"));
    _scene->ProcessDestroyed(); // `go` keeps it alive, destroyed

    EXPECT_FALSE(Eval<bool>("SceneManager.GetCurrentScene():DestroyGameObject(handle_go)"));
}

TEST_F(LuaHandleTest, SceneHandleErrorsAfterSceneSwitch)
{
    Spawn("InOldScene");
    Run("handle_scene = SceneManager.GetCurrentScene()");
    ASSERT_TRUE(Eval<bool>("handle_scene:IsValid()"));
    EXPECT_EQ(Eval<std::string>("handle_scene:FindGameObject('InOldScene'):GetName()"), "InOldScene");
    EXPECT_TRUE(Eval<bool>("handle_scene == SceneManager.GetCurrentScene()"));

    SceneManager::AddScene(Scene::Create("LuaHandle_SceneSwitchTarget"), true);
    SceneManager::ProcessAnyPendingSceneChange(); // frees the old scene
    _scene = nullptr;

    EXPECT_FALSE(Eval<bool>("handle_scene:IsValid()"));
    // These used to call into the freed Scene
    std::string error = RunExpectingError("handle_scene:FindGameObject('InOldScene')");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed Scene")) << error;
    error = RunExpectingError("return handle_scene.sceneName");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed Scene")) << error;
    error = RunExpectingError("handle_scene:AddRootGameObject(GameObject.Create('Late'))");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed Scene")) << error;
    error = RunExpectingError("handle_scene:GetRootGameObjects()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed Scene")) << error;

    // The current scene's handle works, and is a different scene
    EXPECT_EQ(Eval<std::string>("SceneManager.GetCurrentScene().sceneName"), "LuaHandle_SceneSwitchTarget");
    EXPECT_FALSE(Eval<bool>("handle_scene == SceneManager.GetCurrentScene()"));
}

TEST_F(LuaHandleTest, SceneHandleWorksDuringItsUnload)
{
    SpawnTeardownReader("UnloadedWithScene");
    Run("handle_scene = SceneManager.GetCurrentScene()");

    SceneManager::AddScene(Scene::Create("LuaHandle_SceneUnloadTarget"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    _scene = nullptr;

    // OnDestroy ran during the switch, while the old scene still existed
    EXPECT_TRUE(Eval<bool>("teardown_scene_name == 'LuaHandle_SceneHandleWorksDuringItsUnload'"));
    EXPECT_FALSE(Eval<bool>("handle_scene:IsValid()"));
}

TEST_F(LuaHandleTest, SceneNameCanBeSetThroughTheHandle)
{
    Run("SceneManager.GetCurrentScene().sceneName = 'Renamed'");
    EXPECT_EQ(_scene->sceneName, "Renamed");
}

TEST_F(LuaHandleTest, RemoveChildInASceneTakesOwnershipOnlyWhenNoSceneRootOwnsIt)
{
    const auto parent = GameObject::Create("SceneParent");
    parent->AddChild(GameObject::Create("SceneChild"));
    _scene->AddRootGameObject(parent);
    Lua()["handle_parent"] = GameObjectRef(parent);

    Run(R"(
        handle_detached = handle_parent:FindChild("SceneChild")
        handle_parent:RemoveChild(handle_detached)
    )");
    const std::weak_ptr<GameObject> child = Lua()["handle_detached"].get<GameObjectRef>().Lock();
    ASSERT_FALSE(child.expired());

    // Checks the ownership rule under both RemoveChild semantics: one leaves the child in the scene but not a
    // root (the parent was its only owner, so the handle must take over), the other makes it a root of the
    // scene, as SetParent(nullptr) does (the scene owns it, so the handle must stay weak, or it would keep the
    // object alive past Destroy or a scene switch).
    const bool isSceneRoot =
        std::ranges::find(_scene->GetRootGameObjects(), child.lock()) != _scene->GetRootGameObjects().end();
    if (isSceneRoot)
    {
        // Only the scene's root list owns it, not the handle as well
        EXPECT_EQ(child.use_count(), 1);
        Run("handle_detached = nil collectgarbage() collectgarbage()");
        EXPECT_FALSE(child.expired()) << "the scene owns it";
    }
    else
    {
        Run("collectgarbage() collectgarbage()");
        EXPECT_FALSE(child.expired()) << "the handle owns it";
        Run("handle_detached = nil collectgarbage() collectgarbage()");
        EXPECT_TRUE(child.expired());
    }
}
