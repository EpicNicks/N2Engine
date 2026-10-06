#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/ui/Image.hpp"

#include "../rendering/TextureTestSupport.hpp"

// Image:SetSprite / Image:GetSprite from Lua, against image files in a temporary project

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

class LuaImageSpriteTest : public ::testing::Test
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
                 (std::string("n2engine_lua_sprite_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "ui");
        const auto bmp = TextureTestSupport::MakeBmp(1, 2, {{255, 0, 0}, {0, 0, 255}});
        std::ofstream(s_root / "assets" / "ui" / "icon.bmp", std::ios::binary)
            .write(reinterpret_cast<const char *>(bmp.data()), static_cast<std::streamsize>(bmp.size()));

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "LuaImageSpriteTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);

        Run(R"(
            lua_sprite_element = UI.CreateElement("SpriteElement")
            lua_sprite_image = lua_sprite_element:AddComponent("Image")
        )");
    }

    void TearDown() override
    {
        Run("lua_sprite_element = nil; lua_sprite_image = nil");
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

    static UI::Image *Image()
    {
        const sol::object image = Lua()["lua_sprite_image"];
        return image.as<ComponentRef<UI::Image>>().Pin().get();
    }
};

TEST_F(LuaImageSpriteTest, SetsAndGetsTheSpriteByPath)
{
    EXPECT_TRUE(Eval<bool>("lua_sprite_image:GetSprite() == nil")) << "no sprite at first";

    Run(R"(lua_sprite_image:SetSprite("res://ui/icon.bmp"))");
    EXPECT_EQ(Eval<std::string>("lua_sprite_image:GetSprite()"), "res://ui/icon.bmp");

    UI::Image *image = Image();
    ASSERT_NE(image, nullptr);
    ASSERT_NE(image->GetSprite(), nullptr);
    EXPECT_EQ(image->GetSprite(),
              IO::ResourceLoader::Instance().GetCached<Rendering::Texture>(IO::ResourcePath("res://ui/icon.bmp")))
        << "loaded through Resources, so it is the project asset";
    EXPECT_EQ(image->GetSprite()->GetWidth(), 1u);
    EXPECT_EQ(image->GetSprite()->GetHeight(), 2u);

    Run("lua_sprite_image:SetSprite(nil)");
    EXPECT_TRUE(Eval<bool>("lua_sprite_image:GetSprite() == nil"));
    EXPECT_EQ(image->GetSprite(), nullptr);
}

TEST_F(LuaImageSpriteTest, AFileThatDoesntLoadIsAnErrorAndKeepsTheSprite)
{
    Run(R"(lua_sprite_image:SetSprite("res://ui/icon.bmp"))");

    const sol::protected_function_result missing =
        Lua().safe_script(R"(lua_sprite_image:SetSprite("res://ui/NoSuchImage.png"))", sol::script_pass_on_error);
    ASSERT_FALSE(missing.valid());
    const sol::error err = missing;
    EXPECT_NE(std::string(err.what()).find("Image:SetSprite"), std::string::npos) << err.what();
    EXPECT_EQ(Eval<std::string>("lua_sprite_image:GetSprite()"), "res://ui/icon.bmp") << "unchanged";
}

TEST_F(LuaImageSpriteTest, ASpriteMadeAtRuntimeHasAnEmptyPath)
{
    UI::Image *image = Image();
    ASSERT_NE(image, nullptr);
    image->SetSprite(Rendering::Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(Eval<std::string>("lua_sprite_image:GetSprite()"), "");
}
