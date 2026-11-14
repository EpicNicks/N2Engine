#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <math/UUID.hpp>

#include "engine/common/Color.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/rendering/Sprite2D.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

#include "../rendering/TextureTestSupport.hpp"

// Sprite2D:SetOutlineTextureOf and Sprite2D:GetTextureSize from Lua (Texture::MakeOutline, docs/sprite2d.html#texture-outline),
// against an image file in a temporary project

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

class LuaSpriteOutlineTest : public ::testing::Test
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
        Rendering::Texture::ClearOutlineCache();
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_lua_outline_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "sprites");
        // 2 x 1, opaque (a 24-bit BMP has no alpha)
        const auto bmp = TextureTestSupport::MakeBmp(2, 1, {{255, 0, 0}, {0, 0, 255}});
        std::ofstream(s_root / "assets" / "sprites" / "ship.bmp", std::ios::binary)
            .write(reinterpret_cast<const char *>(bmp.data()), static_cast<std::streamsize>(bmp.size()));

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "LuaSpriteOutlineTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);

        Run(R"(
            lua_outline_go = GameObject.Create("OutlinedShip")
            lua_outline_body = lua_outline_go:AddComponent("Sprite2D")
            lua_outline_ring = lua_outline_go:AddComponent("Sprite2D")
        )");
    }

    void TearDown() override
    {
        Run("lua_outline_go = nil; lua_outline_body = nil; lua_outline_ring = nil");
        Lua().collect_garbage();
        IO::ResourceLoader::Instance().ClearCache();
        Rendering::Texture::ClearOutlineCache();
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

    /// The error message `code` raises, or "" (and a failure) when it raises none
    static std::string ErrorOf(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "no error from: " << code;
            return "";
        }
        const sol::error err = result;
        return err.what();
    }

    static Rendering::Sprite2D *Sprite(const char *global)
    {
        const sol::object sprite = Lua()[global];
        return sprite.as<ComponentRef<Rendering::Sprite2D>>().Pin().get();
    }

    static std::shared_ptr<Rendering::Texture> Ship()
    {
        return IO::ResourceLoader::Instance().GetCached<Rendering::Texture>(IO::ResourcePath("res://sprites/ship.bmp"));
    }
};

TEST_F(LuaSpriteOutlineTest, FromAPathTheTextureBecomesTheCachedOutline)
{
    Run(R"(
        assert(lua_outline_ring:GetTextureSize() == nil, "no texture yet")
        lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 1, Color.Red)
        local size = lua_outline_ring:GetTextureSize()
        assert(size.x == 4 and size.y == 3, "2 x 1 padded by 1 on every side")
        assert(lua_outline_ring:HasTexture())
    )");

    Rendering::Sprite2D *ring = Sprite("lua_outline_ring");
    ASSERT_NE(ring, nullptr);
    const auto ship = Ship();
    ASSERT_NE(ship, nullptr) << "loaded through Resources";
    EXPECT_EQ(ring->GetTexture(), Rendering::Texture::MakeOutline(ship, 1, Common::Color::Red))
        << "the outline Texture::MakeOutline caches for the asset";
    const auto &pixels = ring->GetTexture()->GetPixels();
    // Row 1, x 1: the first texel of the source, in the outline colour
    const std::size_t i = (1 * 4 + 1) * 4;
    EXPECT_EQ(pixels[i], 255);
    EXPECT_EQ(pixels[i + 1], 0);
    EXPECT_EQ(pixels[i + 3], 255);
    EXPECT_EQ(pixels[3], 0) << "the corner is transparent";

    const auto first = ring->GetTexture();
    Run(R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 1, Color.Red))");
    EXPECT_EQ(ring->GetTexture(), first) << "asking again is a cache hit";
}

TEST_F(LuaSpriteOutlineTest, FromAnotherSpriteItTakesThatSpritesTexture)
{
    Run(R"(
        lua_outline_body:SetTexture("res://sprites/ship.bmp")
        lua_outline_ring:SetOutlineTextureOf(lua_outline_body, 2, Color.Green)
        lua_outline_ring:SetSortingOrder(lua_outline_body:GetSortingOrder() - 1)
        local size = lua_outline_ring:GetTextureSize()
        assert(size.x == 6 and size.y == 5)
        local body = lua_outline_body:GetTextureSize()
        assert(body.x == 2 and body.y == 1, "the source is unchanged")
    )");
    Rendering::Sprite2D *body = Sprite("lua_outline_body");
    Rendering::Sprite2D *ring = Sprite("lua_outline_ring");
    ASSERT_NE(body, nullptr);
    ASSERT_NE(ring, nullptr);
    EXPECT_EQ(ring->GetTexture(), Rendering::Texture::MakeOutline(body->GetTexture(), 2, Common::Color::Green));
}

TEST_F(LuaSpriteOutlineTest, ABadSourceOrWidthIsAnErrorAndKeepsTheTexture)
{
    Run(R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 1, Color.Red))");
    Rendering::Sprite2D *ring = Sprite("lua_outline_ring");
    ASSERT_NE(ring, nullptr);
    const auto before = ring->GetTexture();
    ASSERT_NE(before, nullptr);

    for (const char *code : {
             R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", -1, Color.Red))",
             R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 1.5, Color.Red))",
             R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 257, Color.Red))",
             R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 0/0, Color.Red))",
             R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", math.huge, Color.Red))",
             R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/NoSuchShip.png", 1, Color.Red))",
             R"(lua_outline_ring:SetOutlineTextureOf(lua_outline_body, 1, Color.Red))", // a sprite with no texture
             R"(lua_outline_ring:SetOutlineTextureOf(42, 1, Color.Red))",
         })
    {
        const std::string message = ErrorOf(code);
        EXPECT_NE(message.find("Sprite2D:SetOutlineTextureOf"), std::string::npos) << code << "\n" << message;
        EXPECT_EQ(ring->GetTexture(), before) << code;
    }

    Run(R"(lua_outline_ring:SetOutlineTextureOf("res://sprites/ship.bmp", 256, Color.Red))");
    EXPECT_EQ(ring->GetTexture()->GetWidth(), 2u + 2u * 256u) << "the widest allowed";
}
