#pragma once

#include <string>
#include <string_view>

namespace N2Engine::Scripting
{
    /// The class name a new script gets from its file name: the part before the first '.', first letter upper-cased,
    /// every character but ASCII letters and digits turned into '_' ("player.lua" -> "Player", "my enemy" ->
    /// "My_enemy"), and '_' in front of a leading digit ("2d.lua" -> "_2d"); "Script" when that leaves nothing
    [[nodiscard]] std::string LuaScriptClassName(std::string_view scriptName);

    /**
     * A new behaviour script in the engine's script model (docs/scripting-lua.html): a class table returned from the
     * chunk, with SerializableFields and the OnAttach/OnUpdate/OnFixedUpdate/OnDestroy callbacks, named
     * LuaScriptClassName(scriptName). What the editor's CreateScript returns and what a new project's Example.lua
     * holds.
     */
    [[nodiscard]] std::string MakeLuaScriptTemplate(std::string_view scriptName);
}
