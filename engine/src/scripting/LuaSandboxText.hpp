#pragma once

// What the sandbox builder (LuaSandbox.cpp) and the declaration evaluator share for turning text that came from a
// script into text that is safe to show. Private to engine/src/scripting.

#include <cstddef>
#include <string>
#include <string_view>

struct lua_State;

namespace N2Engine::Scripting::Detail
{
    // Longest error text kept
    inline constexpr std::size_t MaxErrorBytes = 200;

    // What a log line or an error entry may show of text that came from a script: control characters replaced by
    // '?', cut at MaxErrorBytes on a character boundary
    std::string CleanScriptText(std::string_view text);

    // The error object on top of the stack as text, read without converting it (a string as it is, anything else
    // by its type)
    std::string ErrorObjectText(lua_State *L);
}
