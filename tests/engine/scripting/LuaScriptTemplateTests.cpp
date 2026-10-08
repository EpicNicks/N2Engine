#include <gtest/gtest.h>

#include <string>

#include <engine/scripting/LuaScriptTemplate.hpp>

using namespace N2Engine;

TEST(LuaScriptTemplateTest, ClassNamesAreLuaNames)
{
    EXPECT_EQ(Scripting::LuaScriptClassName("player.lua"), "Player");
    EXPECT_EQ(Scripting::LuaScriptClassName("my enemy"), "My_enemy");
    EXPECT_EQ(Scripting::LuaScriptClassName("2d_player.lua"), "_2d_player") << "a Lua name can't start with a digit";
    EXPECT_EQ(Scripting::LuaScriptClassName(""), "Script");
    EXPECT_EQ(Scripting::LuaScriptClassName(".lua"), "Script");
}

TEST(LuaScriptTemplateTest, TheTemplateIsNamedAfterTheScript)
{
    const std::string text = Scripting::MakeLuaScriptTemplate("Example.lua");
    EXPECT_NE(text.find("local Example = {}"), std::string::npos) << text;
    EXPECT_NE(text.find("Example.SerializableFields"), std::string::npos) << text;
    EXPECT_NE(text.find("return Example"), std::string::npos) << text;
    EXPECT_EQ(text.find("{C}"), std::string::npos) << "every placeholder is filled";
}
