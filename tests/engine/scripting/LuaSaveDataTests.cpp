#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// SaveData (user://saves) and Json from Lua
class LuaSaveDataTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;
    static inline fs::path s_user;

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
        s_root = fs::temp_directory_path() / "n2engine_lua_savedata_tests";
        s_user = s_root / "user";
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets");
        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_root, s_user);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    template <typename T>
    static T Eval(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << code;
            return T{};
        }
        return result.get<T>();
    }

    static std::string ErrorOf(const std::string &code)
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
};

TEST_F(LuaSaveDataTest, WriteReadExistsDelete)
{
    EXPECT_FALSE(Eval<bool>("return SaveData.Exists('slot1.json')"));
    EXPECT_TRUE(Eval<bool>("return SaveData.Read('slot1.json') == nil"));
    EXPECT_TRUE(Eval<bool>("return (SaveData.Write('slot1.json', 'hello'))"));
    EXPECT_TRUE(fs::is_regular_file(s_user / "saves" / "slot1.json")) << "under user://saves";
    EXPECT_FALSE(fs::exists(s_user / "saves" / "slot1.json.tmp")) << "the temporary file is renamed away";
    EXPECT_EQ(Eval<std::string>("return SaveData.Read('slot1.json')"), "hello");
    EXPECT_TRUE(Eval<bool>("return SaveData.Exists('slot1.json')"));
    // Overwrite replaces the whole file
    EXPECT_TRUE(Eval<bool>("return (SaveData.Write('slot1.json', 'hi'))"));
    EXPECT_EQ(Eval<std::string>("return SaveData.Read('slot1.json')"), "hi");
    EXPECT_TRUE(Eval<bool>("return SaveData.Delete('slot1.json')"));
    EXPECT_FALSE(Eval<bool>("return SaveData.Exists('slot1.json')"));
    EXPECT_FALSE(Eval<bool>("return SaveData.Delete('slot1.json')"));
}

TEST_F(LuaSaveDataTest, NamesThatAreNotPlainFileNamesAreErrors)
{
    for (const char *name : {"../escape.json", "a/b.json", ".hidden", "", "with space", "a\\\\b"})
    {
        const std::string error = ErrorOf(std::string("SaveData.Write('") + name + "', 'x')");
        EXPECT_NE(error.find("is not a save name"), std::string::npos) << name << ": " << error;
    }
    const std::string longName(SaveDataMaxNameLength + 1, 'a');
    EXPECT_NE(ErrorOf("SaveData.Read('" + longName + "')").find("is not a save name"), std::string::npos);
    EXPECT_FALSE(fs::exists(s_root / "escape.json"));
}

TEST_F(LuaSaveDataTest, OversizedTextIsRefused)
{
    Lua()["savedata_big"] = std::string(SaveDataMaxBytes + 1, 'x');
    EXPECT_FALSE(Eval<bool>("return (SaveData.Write('big.txt', savedata_big))"));
    EXPECT_NE(Eval<std::string>("local ok, message = SaveData.Write('big.txt', savedata_big) return message")
                  .find("limit"), std::string::npos);
    EXPECT_FALSE(Eval<bool>("return SaveData.Exists('big.txt')"));
    Lua()["savedata_big"] = sol::lua_nil;
}

TEST_F(LuaSaveDataTest, JsonRoundTrip)
{
    EXPECT_EQ(Eval<std::string>("return Json.Encode({ b = 2, a = { 1, 2, 3 }, c = 'x', d = true })"),
              R"({"a":[1,2,3],"b":2,"c":"x","d":true})");
    EXPECT_EQ(Eval<int>("local t = Json.Decode('{\"items\":[5,7],\"coins\":12}') return t.items[2] + t.coins"), 19);
    EXPECT_EQ(Eval<double>("return Json.Decode('1.5')"), 1.5);
    EXPECT_TRUE(Eval<bool>("return math.type(Json.Decode('3')) == 'integer'"));
    EXPECT_TRUE(Eval<bool>("local t = Json.Decode('{\"a\":null}') return t.a == nil"));
    EXPECT_NE(Eval<std::string>("return Json.Encode({ x = 1 }, true)").find('\n'), std::string::npos) << "pretty";
    // A whole save: encode, write, read, decode
    EXPECT_EQ(Eval<int>(R"(
        local save = { version = 1, unlocked = { 1, 3, 4 }, run = { seed = 'ABCDEFGHIJ', coins = 9 } }
        SaveData.Write('save.json', Json.Encode(save))
        local back = Json.Decode(SaveData.Read('save.json'))
        return back.run.coins + #back.unlocked
    )"), 12);
}

TEST_F(LuaSaveDataTest, BadJsonIsNilAndAMessage)
{
    EXPECT_TRUE(Eval<bool>("local v, message = Json.Decode('{oops') return v == nil and message ~= nil"));
    std::string deep;
    for (int i = 0; i <= JsonMaxDepth + 1; ++i)
    {
        deep += "[";
    }
    for (int i = 0; i <= JsonMaxDepth + 1; ++i)
    {
        deep += "]";
    }
    Lua()["savedata_deep"] = deep;
    EXPECT_NE(ErrorOf("Json.Decode(savedata_deep)").find("deeper"), std::string::npos);
    Lua()["savedata_deep"] = sol::lua_nil;
}
