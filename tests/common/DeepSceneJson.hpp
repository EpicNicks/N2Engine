#pragma once

// Deep scene, prefab and GameObject JSON as text, for the tests of the nesting caps (engine/serialization/JsonLimits.hpp).
// Every text is built with loops, never with a recursive helper, so building one can't overflow the stack itself.

#include <cstddef>
#include <cstdio>
#include <string>

namespace N2Engine::TestJson
{
    /// A GameObject chain of `levels` objects, the root being level 1, each the only child of the one above. Every
    /// object has a distinct UUID and is named "n<level>"; the deepest is named "leaf" and has no children.
    inline std::string ChainText(const std::size_t levels)
    {
        std::string text;
        text.reserve(levels * 90 + 16);
        for (std::size_t level = 1; level <= levels; ++level)
        {
            char head[128];
            std::snprintf(head, sizeof(head),
                          "{\"uuid\":\"00000000-0000-0000-0000-%012zx\",\"name\":\"%s\",\"children\":[",
                          level, level == levels ? "leaf" : "n");
            text += head;
        }
        for (std::size_t level = 0; level < levels; ++level)
        {
            text += "]}";
        }
        return text;
    }

    /// A scene whose one root starts a chain of `levels` GameObjects (JSON nesting: 2 * levels + 2)
    inline std::string SceneText(const std::size_t levels, const std::string &name = "Deep")
    {
        return "{\"name\":\"" + name + "\",\"rootGameObjects\":[" + ChainText(levels) + "]}";
    }

    /// A prefab whose root object starts a chain of `levels` GameObjects (JSON nesting: 2 * levels + 1)
    inline std::string PrefabText(const std::size_t levels)
    {
        return "{\"name\":\"DeepPrefab\",\"rootObject\":" + ChainText(levels) + "}";
    }

    /// `depth` nested arrays: [[[...]]]
    inline std::string NestedArrays(const std::size_t depth)
    {
        return std::string(depth, '[') + std::string(depth, ']');
    }
}
