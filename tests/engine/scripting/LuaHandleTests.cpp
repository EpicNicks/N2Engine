#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
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
                                 "handle_self_component", "handle_parent", "handle_detached"})
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
