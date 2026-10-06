#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

#include "../rendering/ModelTestSupport.hpp"

// Models from Lua: Model.Instantiate(path [, parent]), and a model's meshes and materials named by sub-asset paths
// ("res://models/robot.glb#mesh/Body") in MeshRenderer:SetMesh/SetMaterial, against a .glb in a temporary project

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

class LuaModelTest : public ::testing::Test
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
                 (std::string("n2engine_lua_model_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "models");
        const std::vector<std::uint8_t> glb = ModelTestSupport::Robot().ToGlb();
        std::ofstream(s_root / "assets" / "models" / "robot.glb", std::ios::binary)
            .write(reinterpret_cast<const char *>(glb.data()), static_cast<std::streamsize>(glb.size()));

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "LuaModelTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        Run("lua_model = nil; lua_parent = nil; lua_child = nil; lua_mesh_go = nil; lua_mesh = nil");
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
};

TEST_F(LuaModelTest, InstantiateReturnsTheRootOfANewHierarchy)
{
    Run(R"(lua_model = Model.Instantiate("res://models/robot.glb"))");
    EXPECT_TRUE(Eval<bool>("lua_model:IsValid()"));
    EXPECT_EQ(Eval<std::string>("lua_model:GetName()"), "robot");
    EXPECT_EQ(Eval<std::string>("lua_model:FindChild('Robot'):GetName()"), "Robot");
    EXPECT_EQ(Eval<std::string>("lua_model:FindChildRecursive('Arm'):GetName()"), "Arm");
    EXPECT_EQ(Eval<std::string>("lua_model:FindChild('Robot'):GetComponent('MeshRenderer'):GetMesh()"),
              "res://models/robot.glb#mesh/Body");
    EXPECT_EQ(Eval<std::string>("lua_model:FindChildRecursive('Arm'):GetComponent('MeshRenderer'):GetMaterial(1)"),
              "res://models/robot.glb#material/Green");
    EXPECT_TRUE(Eval<bool>("lua_model:GetParent() == nil")) << "no parent was given";

    // The script owns it until something else does: it survives a collection
    Lua().collect_garbage();
    EXPECT_TRUE(Eval<bool>("lua_model:IsValid()"));
}

TEST_F(LuaModelTest, InstantiateUnderAParentAddsItAsAChild)
{
    Run(R"(
        lua_parent = GameObject.Create("Holder")
        lua_child = Model.Instantiate("res://models/robot.glb", lua_parent)
    )");
    EXPECT_TRUE(Eval<bool>("lua_child:GetParent() == lua_parent"));
    EXPECT_EQ(Eval<std::string>("lua_parent:FindChild('robot'):GetName()"), "robot");
}

TEST_F(LuaModelTest, AModelThatDoesntLoadIsAnError)
{
    const std::string error = RunExpectingError(R"(Model.Instantiate("res://models/missing.glb"))");
    EXPECT_NE(error.find("can't load model 'res://models/missing.glb'"), std::string::npos) << error;
}

TEST_F(LuaModelTest, MeshRenderersTakeAModelsMeshesAndMaterialsBySubAssetPath)
{
    Run(R"(
        lua_mesh_go = GameObject.Create("LuaMesh")
        lua_mesh = lua_mesh_go:AddComponent("MeshRenderer")
        lua_mesh:SetMesh("res://models/robot.glb#mesh/Arm")
        lua_mesh:SetMaterial(1, "res://models/robot.glb#material/Red")
    )");
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMesh()"), "res://models/robot.glb#mesh/Arm");
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMaterial(1)"), "res://models/robot.glb#material/Red");
    EXPECT_EQ(Eval<int>("lua_mesh:GetMaterialCount()"), 1);

    const std::string error = RunExpectingError(R"(lua_mesh:SetMesh("res://models/robot.glb#mesh/Nope"))");
    EXPECT_NE(error.find("can't load mesh"), std::string::npos) << error;
    EXPECT_EQ(Eval<std::string>("lua_mesh:GetMesh()"), "res://models/robot.glb#mesh/Arm") << "kept";
    const std::string wrongKind = RunExpectingError(R"(lua_mesh:SetMesh("res://models/robot.glb#material/Red"))");
    EXPECT_NE(wrongKind.find("can't load mesh"), std::string::npos) << wrongKind;
}
