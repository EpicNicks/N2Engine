#include <gtest/gtest.h>

#include <chrono>
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
using namespace N2Engine::Scripting::Bindings;
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

TEST_F(LuaSaveDataTest, JsonKeepsTheNumberSubtype)
{
    // A whole float stays a float: it used to be written as an integer and come back as a Lua integer, whose
    // arithmetic wraps (issue #445)
    EXPECT_EQ(Eval<std::string>("return Json.Encode({ i = 3, f = 6 / 2 })"), R"({"f":3.0,"i":3})");
    EXPECT_EQ(Eval<std::string>("return math.type(Json.Decode(Json.Encode(1e15)))"), "float");
    EXPECT_TRUE(Eval<bool>(R"(
        local coins = Json.Decode(Json.Encode({ coins = 2^62 })).coins
        return math.type(coins) == 'float' and coins * 4 > 0
    )")) << "a large float currency value survives a save and its arithmetic does not wrap";

    // Doubles round-trip exactly
    EXPECT_TRUE(Eval<bool>(R"(
        for _, x in ipairs({ 0.1, 1 / 3, 123456789.123456789, 1.7976931348623157e308, 5e-324, -2.5e-12, 2^53 + 0.0 }) do
            local back = Json.Decode(Json.Encode(x))
            if back ~= x or math.type(back) ~= 'float' then return false end
        end
        return true
    )"));
    EXPECT_EQ(Eval<std::string>("return math.type(Json.Decode(Json.Encode(math.maxinteger)))"), "integer");
    EXPECT_TRUE(Eval<bool>("return Json.Decode(Json.Encode(math.maxinteger)) == math.maxinteger"));
    EXPECT_EQ(Eval<std::string>("return Json.Encode({ 0/0, 1/0 })"), "[null,null]") << "no JSON form";
}

TEST_F(LuaSaveDataTest, JsonDecodeCanMakeEveryNumberAFloat)
{
    EXPECT_EQ(Eval<std::string>("return math.type(Json.Decode('3', { numbers = 'float' }))"), "float");
    EXPECT_TRUE(Eval<bool>(R"(
        local t = Json.Decode('{"coins":9007199254740993,"list":[1,2]}', { numbers = 'float' })
        return math.type(t.coins) == 'float' and math.type(t.list[2]) == 'float' and t.list[2] == 2
    )"));
    EXPECT_EQ(Eval<std::string>("return math.type(Json.Decode('3', { numbers = 'auto' }))"), "integer");
    EXPECT_EQ(Eval<std::string>("return math.type(Json.Decode('3', {}))"), "integer");
    EXPECT_NE(ErrorOf("Json.Decode('3', { numbers = 'int' })").find("options.numbers"), std::string::npos);
}

TEST_F(LuaSaveDataTest, GetModifiedTimeIsTheLastWriteAsUnixTime)
{
    EXPECT_TRUE(Eval<bool>("return SaveData.GetModifiedTime('never-written.json') == nil"));
    EXPECT_TRUE(Eval<bool>("return (SaveData.Write('stamped.json', '{}'))"));
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const double written = Eval<double>("return SaveData.GetModifiedTime('stamped.json')");
    // File systems store times coarsely (FAT: 2 seconds); allow a few seconds either way
    EXPECT_NEAR(written, now, 5.0);
    EXPECT_NE(ErrorOf("SaveData.GetModifiedTime('../x')").find("not a save name"), std::string::npos);
    Eval<bool>("return SaveData.Delete('stamped.json')");
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
