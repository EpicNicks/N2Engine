#include <gtest/gtest.h>

#include <string>

#include "engine/input/InputTypes.hpp"
#include "engine/input/KeySource.hpp"
#include "engine/input/Keyboard.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;

namespace
{
    // Installs a KeySource for one test and puts the previous one back
    struct ScopedKeys
    {
        InjectedKeys keys;
        const KeySource *previous;
        ScopedKeys() : previous(KeySource::Set(&keys)) {}
        ~ScopedKeys() { KeySource::Set(previous); }
    };
}

TEST(KeyboardTest, DownAndUpLastOneFrame)
{
    ScopedKeys scoped;
    Keyboard keyboard(nullptr);

    keyboard.Update();
    EXPECT_FALSE(keyboard.GetKey(Key::Space));

    scoped.keys.SetKey(Key::Space, true);
    keyboard.Update();
    EXPECT_TRUE(keyboard.GetKey(Key::Space));
    EXPECT_TRUE(keyboard.GetKeyDown(Key::Space));
    EXPECT_FALSE(keyboard.GetKeyUp(Key::Space));

    keyboard.Update(); // still held
    EXPECT_TRUE(keyboard.GetKey(Key::Space));
    EXPECT_FALSE(keyboard.GetKeyDown(Key::Space));

    scoped.keys.SetKey(Key::Space, false);
    keyboard.Update();
    EXPECT_FALSE(keyboard.GetKey(Key::Space));
    EXPECT_TRUE(keyboard.GetKeyUp(Key::Space));

    keyboard.Update();
    EXPECT_FALSE(keyboard.GetKeyUp(Key::Space));
    EXPECT_FALSE(keyboard.GetKey(Key::Unknown)) << "Unknown is never pressed";
}

TEST(KeyboardTest, TypedTextIsThisFramesOnly)
{
    Keyboard keyboard(nullptr);
    keyboard.InjectText("he");
    keyboard.InjectText("y");
    EXPECT_EQ(keyboard.GetTypedText(), "") << "not before the next Update";
    keyboard.Update();
    EXPECT_EQ(keyboard.GetTypedText(), "hey");
    keyboard.Update();
    EXPECT_EQ(keyboard.GetTypedText(), "");
}

TEST(KeyboardTest, CodepointsBecomeUtf8)
{
    Keyboard keyboard(nullptr);
    keyboard.AppendCodepoint('a');
    keyboard.AppendCodepoint(0xE9);    // é
    keyboard.AppendCodepoint(0x3042);  // あ
    keyboard.AppendCodepoint(0x1F600); // 😀
    keyboard.AppendCodepoint(0xD800);  // a lone surrogate: dropped
    keyboard.AppendCodepoint(0x110000); // past Unicode: dropped
    keyboard.Update();
    EXPECT_EQ(keyboard.GetTypedText(), "a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80");
}

TEST(KeyboardTest, TypedTextIsCappedPerFrame)
{
    Keyboard keyboard(nullptr);
    keyboard.InjectText(std::string(Keyboard::MaxTypedBytes, 'x'));
    keyboard.InjectText("y"); // over the cap: dropped
    keyboard.Update();
    EXPECT_EQ(keyboard.GetTypedText().size(), Keyboard::MaxTypedBytes);
    EXPECT_EQ(keyboard.GetTypedText().find('y'), std::string::npos);
}

TEST(KeyboardTest, KeyNamesAreTheActionMapNames)
{
    EXPECT_EQ(Keyboard::KeyFromName("Space"), Key::Space);
    EXPECT_EQ(Keyboard::KeyFromName("LeftShift"), Key::LeftShift);
    EXPECT_EQ(Keyboard::KeyFromName("Key1"), Key::Key1);
    EXPECT_FALSE(Keyboard::KeyFromName("Spcae").has_value());
    EXPECT_FALSE(Keyboard::KeyFromName("").has_value());
}

TEST(KeyboardLuaTest, UnknownKeyNamesAreLuaErrors)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    auto &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result bad = lua.safe_script("return Input.GetKey('Spcae')", sol::script_pass_on_error);
    ASSERT_FALSE(bad.valid());
    const sol::error err = bad;
    EXPECT_NE(std::string(err.what()).find("is not a key name"), std::string::npos) << err.what();
    const sol::protected_function_result text = lua.safe_script("return type(Input.GetTypedText())", sol::script_pass_on_error);
    ASSERT_TRUE(text.valid());
    EXPECT_EQ(text.get<std::string>(), "string");
}
