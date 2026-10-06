#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/QuadRenderer.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// MeshRenderer and the built-in shapes from Lua: AddComponent by name, meshes by built-in name, materials by .mat
// path (1-based slots), bounds; against .mat files in a temporary project

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

class LuaMeshRendererTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    void SetUp() override
    {
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_lua_mesh_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "materials");
        std::ofstream(s_root / "assets" / "materials" / "red.mat")
            << R"({"shading": "unlit", "baseColor": {"r": 1, "g": 0, "b": 0, "a": 1}})";
        std::ofstream(s_root / "assets" / "materials" / "glass.mat") << R"({"alphaMode": "blend"})";

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "LuaMeshRendererTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);

        Run(R"(
            lua_mesh_go = GameObject.Create("LuaMesh")
            lua_mesh = lua_mesh_go:AddComponent("MeshRenderer")
        )");
    }

    void TearDown() override
    {
        Run("lua_mesh_go = nil; lua_mesh = nil; lua_quad = nil");
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

    static Rendering::MeshRenderer *Renderer()
    {
        const sol::object renderer = Lua()["lua_mesh"];
        return renderer.as<ComponentRef<Rendering::MeshRenderer>>().Pin().get();
    }
};

TEST_F(LuaMeshRendererTest, SetsAndGetsABuiltinMeshByName)
{
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetMesh() == nil")) << "no mesh at first";
    EXPECT_EQ(Eval<int>("lua_mesh:GetMaterialCount()"), 0);
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetBounds() == nil"));

    Run(R"(lua_mesh:SetMesh("Cube"))");
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMesh()"), "Cube");
    EXPECT_EQ(Renderer()->GetMesh(), Rendering::Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube));
    EXPECT_EQ(Eval<int>("lua_mesh:GetMaterialCount()"), 1);
    EXPECT_FLOAT_EQ(Eval<float>("lua_mesh:GetBounds().min.x"), -0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_mesh:GetBounds().max.y"), 0.5f);

    Run(R"(lua_mesh:SetMesh("Quad"))");
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMesh()"), "Quad");
    EXPECT_FLOAT_EQ(Eval<float>("lua_mesh:GetBounds().max.z"), 0.0f);

    Run("lua_mesh:SetMesh(nil)");
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetMesh() == nil"));
}

TEST_F(LuaMeshRendererTest, AnUnknownMeshIsAnErrorAndKeepsTheCurrentOne)
{
    Run(R"(lua_mesh:SetMesh("Sphere"))");
    const std::string error = RunExpectingError(R"(lua_mesh:SetMesh("Teapot"))");
    EXPECT_NE(error.find("can't load mesh 'Teapot'"), std::string::npos) << error;
    EXPECT_NE(error.find("\"Cube\""), std::string::npos) << "lists the built-in names: " << error;
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMesh()"), "Sphere");
}

TEST_F(LuaMeshRendererTest, SetsAndGetsMaterialsByPathInOneBasedSlots)
{
    Run(R"(lua_mesh:SetMesh("Cube"))");
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetMaterial(1) == nil")) << "empty: drawn lit white";

    Run(R"(lua_mesh:SetMaterial(1, "res://materials/red.mat"))");
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMaterial(1)"), "res://materials/red.mat");
    const auto material = Renderer()->GetMaterial(0);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->GetShading(), Rendering::ShadingModel::Unlit);
    EXPECT_FLOAT_EQ(material->GetBaseColor().g, 0.0f);

    // A slot past the submeshes is kept
    Run(R"(lua_mesh:SetMaterial(3, "res://materials/glass.mat"))");
    EXPECT_EQ(Renderer()->GetMaterials().size(), 3u);
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMaterial(3)"), "res://materials/glass.mat");
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetMaterial(2) == nil"));

    Run("lua_mesh:SetMaterial(1, nil)");
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetMaterial(1) == nil"));
    EXPECT_EQ(Renderer()->GetMaterial(0), nullptr);
}

TEST_F(LuaMeshRendererTest, BadSlotsAndPathsAreErrors)
{
    Run(R"(lua_mesh:SetMesh("Cube"))");
    EXPECT_NE(RunExpectingError(R"(lua_mesh:SetMaterial(0, "res://materials/red.mat"))").find("count from 1"),
              std::string::npos);
    EXPECT_NE(RunExpectingError("lua_mesh:GetMaterial(0)").find("count from 1"), std::string::npos);
    const std::string missing = RunExpectingError(R"(lua_mesh:SetMaterial(1, "res://materials/nope.mat"))");
    EXPECT_NE(missing.find("can't load material"), std::string::npos) << missing;
    EXPECT_TRUE(Eval<bool>("lua_mesh:GetMaterial(1) == nil")) << "the slot is unchanged";
}

TEST_F(LuaMeshRendererTest, QuadRendererIsBoundAndTheShapesTakeMaterials)
{
    Run(R"(
        lua_quad = lua_mesh_go:AddComponent("QuadRenderer")
        lua_quad:SetColor(Color.Green)
        lua_quad:SetSize(Vector3(2, 3, 1))
        lua_quad:SetMaterial("res://materials/glass.mat")
    )");
    const sol::object quadObject = Lua()["lua_quad"];
    ASSERT_TRUE(quadObject.is<ComponentRef<Example::QuadRenderer>>()) << "QuadRenderer has its own Lua type";
    const auto quad = quadObject.as<ComponentRef<Example::QuadRenderer>>().Pin();
    EXPECT_FLOAT_EQ(quad->GetColor().g, 1.0f);
    EXPECT_FLOAT_EQ(quad->GetSize().y, 3.0f);
    ASSERT_NE(quad->GetMaterial(), nullptr);
    EXPECT_TRUE(quad->GetMaterial()->IsBlended());
    EXPECT_EQ(Eval<std::string>("lua_quad:GetMaterial()"), "res://materials/glass.mat");
    EXPECT_FLOAT_EQ(Eval<float>("lua_quad:GetSize().x"), 2.0f);

    Run("lua_quad:SetMaterial(nil)");
    EXPECT_TRUE(Eval<bool>("lua_quad:GetMaterial() == nil"));
    EXPECT_TRUE(Eval<bool>("lua_mesh_go:GetComponent('QuadRenderer') == lua_quad"));
}
