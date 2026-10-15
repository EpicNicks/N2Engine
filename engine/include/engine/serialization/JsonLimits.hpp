#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace N2Engine::JsonLimits
{
    /// The deepest GameObject hierarchy a scene or prefab may have, the root being level 1. GameObject::Deserialize
    /// refuses a deeper one, and a reparent that would make one is refused. A model's node tree can be as deep as the
    /// importers allow (kMaxNodeDepth, 256 levels) and is placed under a few parents of a scene, so the cap must stay
    /// at least a margin above that; it is also what keeps the recursive walks of a loaded tree (Serialize, SetScene,
    /// Find, prefab instantiate) inside a 1 MiB stack (MSVC Debug), which the tests prove.
    inline constexpr std::size_t MaxHierarchyDepth = 512;

    /// The deepest nesting scene, prefab and snapshot JSON text may have: every GameObject level is two JSON levels
    /// (the object and its "children" array), plus room for the nesting inside a component's data.
    inline constexpr std::size_t MaxSceneJsonDepth = 2 * MaxHierarchyDepth + 64;

    /// Whether the JSON text nests deeper than maxDepth, counting [ and { outside strings (a quote inside a string
    /// is escaped, so strings are skipped whole). A pre-scan, so nothing is built from text that is too deep:
    /// nlohmann's parser is iterative, but a tree 100 000 deep overflows the stack of every recursive walk that
    /// copies, saves, compares or dumps it.
    inline bool NestsDeeperThan(const std::string_view text, const std::size_t maxDepth)
    {
        std::size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        for (const char c : text)
        {
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '[' || c == '{')
            {
                if (++depth > maxDepth)
                    return true;
            }
            else if ((c == ']' || c == '}') && depth > 0)
                --depth;
        }
        return false;
    }

    /// Throws std::runtime_error when the JSON text nests deeper than maxDepth
    inline void CheckJsonDepth(const std::string_view text, const std::size_t maxDepth)
    {
        if (NestsDeeperThan(text, maxDepth))
            throw std::runtime_error("the JSON nests more than " + std::to_string(maxDepth) + " levels deep");
    }

    /// The text of the error for scene, prefab or snapshot JSON that nests deeper than MaxSceneJsonDepth
    inline std::string SceneJsonTooDeepMessage()
    {
        return "the scene JSON nests more than " + std::to_string(MaxSceneJsonDepth) + " levels deep";
    }

    /// Whether scene, prefab or snapshot JSON text is too deep to build (see MaxSceneJsonDepth). Run it before
    /// nlohmann::json::parse on every such text, whether it comes from a client or from disk.
    inline bool SceneJsonTooDeep(const std::string_view text)
    {
        return NestsDeeperThan(text, MaxSceneJsonDepth);
    }

    /// The nesting depth of a parsed value (a scalar is 0, [] is 1), walked iteratively, so it is safe for a tree of
    /// any depth. For data that arrives as a value rather than as text.
    inline std::size_t NestingDepth(const nlohmann::json &value)
    {
        std::size_t deepest = 0;
        std::vector<std::pair<const nlohmann::json *, std::size_t>> pending;
        pending.emplace_back(&value, 0);
        while (!pending.empty())
        {
            const auto [item, depth] = pending.back();
            pending.pop_back();
            if (!item->is_structured())
                continue;
            const std::size_t level = depth + 1;
            if (level > deepest)
                deepest = level;
            for (const nlohmann::json &child : *item)
                pending.emplace_back(&child, level);
        }
        return deepest;
    }

    /// Whether a parsed scene, prefab or snapshot value nests deeper than MaxSceneJsonDepth
    inline bool SceneValueTooDeep(const nlohmann::json &value)
    {
        return NestingDepth(value) > MaxSceneJsonDepth;
    }
}
