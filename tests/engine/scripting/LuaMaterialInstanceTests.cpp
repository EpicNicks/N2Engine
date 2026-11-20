#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <math/UUID.hpp>

#include "../network/NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/network/n2net/HashTree.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

#include "../rendering/MeshTestSupport.hpp"

// Lua Material handles (user shaders, stage 5b; orchestrator/proposals/user-shaders.md): Material.Load (the shared asset),
// renderer:GetMaterialInstance(slot) (copy-on-write per renderer slot, runtime only), the typed setters and GetFloat, and the determinism
// rule of section 13 item 10: a Lua material's values are render state, never part of a rollback snapshot, replication or state hash, so a
// script that sets a parameter every frame leaves the state-hash tree unchanged.

using namespace N2Engine;
using namespace N2Engine::Scripting;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using N2Engine::N2Net::HashTree;
using MeshTestSupport::WarningCapture;
namespace fs = std::filesystem;

namespace
{
    const char *kShader = R"({ "n2shader": 1, "name": "LuaSolid", "params": [
  { "name": "u_Color", "type": "color", "default": [1, 0, 0, 1] },
  { "name": "u_Gain", "type": "float", "default": 1, "min": 0, "max": 10 },
  { "name": "u_Offset", "type": "vec3", "default": [0, 0, 0] },
  { "name": "u_Steps", "type": "int", "default": 2 },
  { "name": "u_Flag", "type": "bool", "default": false },
  { "name": "u_Tex", "type": "texture" } ] }
#stage fragment
void main() { n2_FragColor = u_Color * u_Gain; }
)";
}

class LuaMaterialInstanceTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite() { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }

    void SetUp() override
    {
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_lua_matinst_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "materials");
        fs::create_directories(s_root / "assets" / "shaders");
        std::ofstream(s_root / "assets" / "shaders" / "solid.n2shader") << kShader;
        std::ofstream(s_root / "assets" / "materials" / "a.mat") << R"({"shader": "res://shaders/solid.n2shader"})";
        fs::create_directories(s_root / "assets" / "statehash");
        std::ofstream(s_root / "assets" / "statehash" / "Counter.lua") << R"(
            local Counter = {}
            Counter.__index = Counter
            Counter.SerializableFields = { score = { type = "int", default = 1, replicated = true } }
            function Counter:OnUpdate() self.score = self.score + 1 end
            return Counter
        )";
        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "LuaMaterialInstanceTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        Run("go1 = nil; go2 = nil; mr1 = nil; mr2 = nil; inst = nil; inst2 = nil; quad = nil; shared = nil");
        Lua().collect_garbage();
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    static std::string RunExpectingError(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "expected an error from: " << code;
            return {};
        }
        const sol::error err = result;
        return err.what();
    }

    template <typename T>
    static T Eval(const std::string &expression)
    {
        const sol::protected_function_result result = Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
            return T{};
        }
        return result.get<T>();
    }

    /// Two objects with a MeshRenderer each, both with slot 1 = the .mat, in Lua globals mr1 and mr2
    static void TwoRenderers()
    {
        Run(R"(
            go1 = GameObject.Create("One")
            mr1 = go1:AddComponent("MeshRenderer")
            mr1:SetMesh("Quad")
            mr1:SetMaterial(1, "res://materials/a.mat")
            go2 = GameObject.Create("Two")
            mr2 = go2:AddComponent("MeshRenderer")
            mr2:SetMesh("Quad")
            mr2:SetMaterial(1, "res://materials/a.mat")
        )");
    }

    static Rendering::MeshRenderer *Renderer(const char *global)
    {
        const sol::object renderer = Lua()[global];
        return renderer.as<ComponentRef<Rendering::MeshRenderer>>().Pin().get();
    }
};

TEST_F(LuaMaterialInstanceTest, MaterialLoadIsTheSharedAssetAndAWriteThroughItShowsOnEveryUser)
{
    TwoRenderers();
    Run(R"(shared = Material.Load("res://materials/a.mat"))");
    EXPECT_EQ(Eval<std::string>("shared:GetPath()"), "res://materials/a.mat");
    EXPECT_DOUBLE_EQ(Eval<double>("shared:GetFloat(\"u_Gain\")"), 1.0) << "the shader's default";
    EXPECT_TRUE(Eval<bool>("shared:SetFloat(\"u_Gain\", 4)"));
    const auto asset = IO::Resources::Instance().Load<Rendering::Material>(fs::path("res://materials/a.mat"));
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->GetParam("u_Gain")->value[0], 4.0f);
    EXPECT_EQ(Renderer("mr1")->GetMaterial(0), asset) << "both users draw with the asset itself";
    EXPECT_EQ(Renderer("mr2")->GetMaterial(0), asset);
    RunExpectingError(R"(Material.Load("res://materials/nope.mat"))");
}

TEST_F(LuaMaterialInstanceTest, AnInstanceCopiesOnItsFirstWriteAndNoOtherUserChanges)
{
    TwoRenderers();
    const auto asset = IO::Resources::Instance().Load<Rendering::Material>(fs::path("res://materials/a.mat"));
    ASSERT_NE(asset, nullptr);
    Run(R"(inst = mr1:GetMaterialInstance(1))");
    EXPECT_EQ(Renderer("mr1")->GetMaterial(0), asset) << "a read-only handle makes no copy";
    EXPECT_DOUBLE_EQ(Eval<double>("inst:GetFloat(\"u_Gain\")"), 1.0);
    EXPECT_EQ(Renderer("mr1")->GetMaterial(0), asset) << "GetFloat does not copy either";

    EXPECT_TRUE(Eval<bool>("inst:SetFloat(\"u_Gain\", 5)"));
    const auto mine = Renderer("mr1")->GetMaterial(0);
    ASSERT_NE(mine, nullptr);
    EXPECT_NE(mine, asset) << "the first write cloned the material into this renderer's slot";
    EXPECT_EQ(mine->GetParam("u_Gain")->value[0], 5.0f);
    EXPECT_EQ(asset->GetParam("u_Gain")->value[0], 1.0f) << "the asset is untouched";
    EXPECT_EQ(Renderer("mr2")->GetMaterial(0), asset) << "the other renderer still uses the shared asset";
    EXPECT_DOUBLE_EQ(Eval<double>("inst:GetFloat(\"u_Gain\")"), 5.0);

    // Further writes go to the same copy
    EXPECT_TRUE(Eval<bool>("inst:SetFloat(\"u_Gain\", 6)"));
    EXPECT_EQ(Renderer("mr1")->GetMaterial(0), mine);
    EXPECT_EQ(mine->GetParam("u_Gain")->value[0], 6.0f);

    // Another instance of the other renderer is its own copy
    Run(R"(inst2 = mr2:GetMaterialInstance(1); inst2:SetFloat("u_Gain", 2))");
    EXPECT_NE(Renderer("mr2")->GetMaterial(0), Renderer("mr1")->GetMaterial(0));
    EXPECT_EQ(Renderer("mr1")->GetMaterial(0)->GetParam("u_Gain")->value[0], 6.0f);
    EXPECT_EQ(Renderer("mr2")->GetMaterial(0)->GetParam("u_Gain")->value[0], 2.0f);

    // Scene serialization still names the .mat: the copy keeps the asset's identity, and nothing of it is saved
    EXPECT_EQ(Eval<std::string>("mr1:GetMaterial(1)"), "res://materials/a.mat");
    EXPECT_EQ(Renderer("mr1")->GetMaterial(0)->GetUUID(), asset->GetUUID());
}

TEST_F(LuaMaterialInstanceTest, TheTypedSettersTakeTheDeclaredNamesAndTypes)
{
    TwoRenderers();
    Run(R"(inst = mr1:GetMaterialInstance(1))");
    WarningCapture capture;
    EXPECT_TRUE(Eval<bool>("inst:SetColor(\"u_Color\", Color(0, 1, 0, 1))"));
    EXPECT_TRUE(Eval<bool>("inst:SetVec3(\"u_Offset\", 0.1, 0.2, 0.3)"));
    EXPECT_TRUE(Eval<bool>("inst:SetInt(\"u_Steps\", 7)"));
    EXPECT_TRUE(Eval<bool>("inst:SetInt(\"u_Flag\", 1)"));
    EXPECT_TRUE(Eval<bool>("inst:SetVec4(\"u_Color\", 0, 0, 1, 1)")) << "a color takes a vec4 too";
    EXPECT_TRUE(capture.messages.empty());
    const auto material = Renderer("mr1")->GetMaterial(0);
    EXPECT_EQ(material->GetParam("u_Color")->value, (std::array<float, 4>{0, 0, 1, 1}));
    EXPECT_EQ(material->GetParam("u_Steps")->value[0], 7.0f);
    EXPECT_EQ(material->GetParam("u_Flag")->value[0], 1.0f);
    EXPECT_FLOAT_EQ(material->GetParam("u_Offset")->value[2], 0.3f);

    // A wrong name or setter: false, one warning; GetFloat of those: nil
    EXPECT_FALSE(Eval<bool>("inst:SetFloat(\"nope\", 1)"));
    EXPECT_FALSE(Eval<bool>("inst:SetVec2(\"u_Gain\", 1, 2)"));
    EXPECT_EQ(capture.messages.size(), 2u);
    EXPECT_TRUE(Eval<bool>("inst:GetFloat(\"nope\") == nil"));
    EXPECT_TRUE(Eval<bool>("inst:GetFloat(\"u_Tex\") == nil")) << "a texture parameter has no number";
    // The declared range clamps
    EXPECT_TRUE(Eval<bool>("inst:SetFloat(\"u_Gain\", 99)"));
    EXPECT_DOUBLE_EQ(Eval<double>("inst:GetFloat(\"u_Gain\")"), 10.0);
    // A texture that does not load raises an error and changes nothing
    EXPECT_NE(RunExpectingError(R"(inst:SetTexture("u_Tex", "res://textures/missing.png"))").find("can't load texture"), std::string::npos);
    EXPECT_TRUE(Eval<bool>("inst:SetTexture(\"u_Tex\", nil)")) << "nil clears";
}

TEST_F(LuaMaterialInstanceTest, AnEmptySlotHasNoInstanceAndTheShapesHaveOne)
{
    Run(R"(
        go1 = GameObject.Create("One")
        mr1 = go1:AddComponent("MeshRenderer")
        mr1:SetMesh("Quad")
    )");
    EXPECT_TRUE(Eval<bool>("mr1:GetMaterialInstance(1) == nil"));
    RunExpectingError("mr1:GetMaterialInstance(0)");

    Run(R"(
        go2 = GameObject.Create("Shape")
        quad = go2:AddComponent("QuadRenderer")
    )");
    EXPECT_TRUE(Eval<bool>("quad:GetMaterialInstance() == nil")) << "no material";
    Run(R"(quad:SetMaterial("res://materials/a.mat"); inst = quad:GetMaterialInstance())");
    EXPECT_TRUE(Eval<bool>("inst:SetFloat(\"u_Gain\", 3)"));
    const sol::object shape = Lua()["quad"];
    const auto drawn = shape.as<ComponentRef<QuadRenderer>>().Pin()->GetMaterial();
    ASSERT_NE(drawn, nullptr);
    EXPECT_EQ(drawn->GetParam("u_Gain")->value[0], 3.0f);
    EXPECT_NE(drawn, IO::Resources::Instance().Load<Rendering::Material>(fs::path("res://materials/a.mat")));
}

// ---- determinism: Lua material state is not simulation state -----------------------------------------------------------------

namespace
{
    struct HashRig
    {
        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        LayoutCache layouts;
        std::vector<LuaComponent *> scripts;
        std::vector<Rendering::MeshRenderer *> renderers;

        explicit HashRig(const std::uint32_t objects)
        {
            scene = Scene::Create("LuaMaterialHash");
            const auto asset = IO::Resources::Instance().Load<Rendering::Material>(fs::path("res://materials/a.mat"));
            for (std::uint32_t n = 1; n <= objects; ++n)
            {
                const GameObject::Ptr go = GameObject::Create("Object" + std::to_string(n));
                go->SetUUID(TestUuid(n));
                go->AddComponent<NetworkObject>();
                auto *renderer = go->AddComponent<Rendering::MeshRenderer>();
                renderer->SetMesh(Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Quad));
                renderer->SetMaterial(0, asset);
                renderers.push_back(renderer);
                auto *script = go->AddComponent<LuaComponent>();
                script->SetScript(IO::ResourcePath("res://statehash/Counter.lua"));
                scripts.push_back(script);
                scene->AddRootGameObject(go);
            }
            EXPECT_TRUE(world.BindScene(*scene).has_value());
        }

        HashTree Tree()
        {
            auto tree = BuildStateHashTree(world, layouts, 3);
            EXPECT_TRUE(tree.has_value());
            return tree.value_or(HashTree());
        }
    };
}

TEST_F(LuaMaterialInstanceTest, SettingAParameterEveryFrameLeavesTheStateHashTreeUnchanged)
{
    HashRig plain(2);
    HashRig written(2);
    ASSERT_EQ(plain.Tree().Root(), written.Tree().Root());

    // The written rig gets a Lua instance on object 1's renderer and sets a different value of every kind of parameter each frame, on top
    // of the same script updates the plain rig runs
    Lua()["lua_renderer"] = ComponentRef<Rendering::MeshRenderer>(*written.renderers[0]);
    Run("lua_inst = lua_renderer:GetMaterialInstance(1)");
    for (int frame = 1; frame <= 8; ++frame)
    {
        for (LuaComponent *script : plain.scripts)
        {
            script->OnUpdate();
        }
        for (LuaComponent *script : written.scripts)
        {
            script->OnUpdate();
        }
        Lua()["lua_frame"] = frame;
        Run(R"(
            lua_inst:SetFloat("u_Gain", lua_frame * 0.5)
            lua_inst:SetColor("u_Color", Color(lua_frame / 8, 0, 1, 1))
            lua_inst:SetVec3("u_Offset", lua_frame, 0, 0)
            lua_inst:SetInt("u_Steps", lua_frame)
            lua_inst:SetInt("u_Flag", lua_frame % 2)
        )");
        EXPECT_EQ(plain.Tree().Root(), written.Tree().Root()) << "frame " << frame << ": the material values are not hashed";
    }
    // The values did change (the test is not vacuous), on a copy only the written rig's renderer holds
    EXPECT_EQ(written.renderers[0]->GetMaterial(0)->GetParam("u_Steps")->value[0], 8.0f);
    EXPECT_NE(written.renderers[0]->GetMaterial(0), plain.renderers[0]->GetMaterial(0));
    Run("lua_inst = nil; lua_renderer = nil");
}
