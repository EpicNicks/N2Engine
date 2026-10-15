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
    /// at least a margin above that. It is also what keeps the recursive walks of a loaded tree (Deserialize, Serialize,
    /// SetScene, Find), one small frame per GameObject level, inside a 1 MiB stack (MSVC Debug), which the tests prove.
    /// nlohmann's dump and copy recurse once per JSON level with far bigger frames, so scene JSON is dumped and copied
    /// with IterativeDump and IterativeCopy (below), whose stack use does not grow with the depth.
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

    /// value.dump(indent, indentChar, ensureAscii, errorHandler), byte for byte, without recursion. nlohmann's own
    /// dump calls itself once per nesting level (about 2 KB of stack each in an MSVC Debug build), so scene JSON at the
    /// hierarchy cap would overflow a 1 MiB stack; this one keeps the open arrays and objects on the heap. Scalars,
    /// empty containers and object keys are still written by nlohmann's serializer (none of them nests), so number
    /// formatting, string escaping and the error handler behave exactly as in dump. Use it for scene, prefab and
    /// snapshot JSON, and for anything else that can be as deep.
    inline std::string IterativeDump(const nlohmann::json &value, const int indent = -1, const char indentChar = ' ',
                                     const bool ensureAscii = false,
                                     const nlohmann::json::error_handler_t errorHandler =
                                         nlohmann::json::error_handler_t::strict)
    {
        std::string out;
        nlohmann::detail::serializer<nlohmann::json> leaves(nlohmann::detail::output_adapter<char, std::string>(out),
                                                            indentChar, errorHandler);
        const bool pretty = indent >= 0;
        const unsigned int step = pretty ? static_cast<unsigned int>(indent) : 0u;

        // An open array or object: the next element to write, and the indent of its closing bracket
        struct Open
        {
            const nlohmann::json *container;
            nlohmann::json::const_iterator next;
            unsigned int indent;
            bool first;
        };
        std::vector<Open> open;

        // Opens a non-empty container (the stack holds it until it closes); writes anything else whole
        const auto begin = [&](const nlohmann::json &item, const unsigned int currentIndent)
        {
            if (item.is_structured() && !item.empty())
            {
                out += item.is_object() ? '{' : '[';
                if (pretty)
                    out += '\n';
                open.push_back(Open{&item, item.cbegin(), currentIndent, true});
            }
            else
            {
                leaves.dump(item, pretty, ensureAscii, step, currentIndent);
            }
        };

        begin(value, 0);
        while (!open.empty())
        {
            Open &top = open.back();
            if (top.next == top.container->cend())
            {
                if (pretty)
                {
                    out += '\n';
                    out.append(top.indent, indentChar);
                }
                out += top.container->is_object() ? '}' : ']';
                open.pop_back();
                continue;
            }
            if (!top.first)
                out += pretty ? ",\n" : ",";
            top.first = false;
            const unsigned int childIndent = top.indent + step;
            if (pretty)
                out.append(childIndent, indentChar);
            if (top.container->is_object())
            {
                // The key as dump writes it: a JSON string, escaped by the same serializer
                leaves.dump(nlohmann::json(top.next.key()), false, ensureAscii, 0);
                out += pretty ? ": " : ":";
            }
            const nlohmann::json &child = *top.next;
            ++top.next;
            begin(child, childIndent); // may grow `open`, so `top` is not used after this
        }
        return out;
    }

    /// A deep copy of value (equal to it under ==), made without recursion: nlohmann's copy constructor calls itself
    /// once per nesting level, as its dump does. Use it wherever scene, prefab or snapshot JSON is copied.
    inline nlohmann::json IterativeCopy(const nlohmann::json &value)
    {
        // A scalar or an empty container copies without recursion; a non-empty container starts empty and is filled
        const auto shell = [](const nlohmann::json &item)
        {
            return item.is_structured() && !item.empty() ? nlohmann::json(item.type()) : nlohmann::json(item);
        };

        nlohmann::json copy = shell(value);
        // Pairs of (source, its copy) whose elements are still to be copied. A copy's address is stable once pushed:
        // object elements are map nodes, and an array's elements are all added before any of them is pushed.
        std::vector<std::pair<const nlohmann::json *, nlohmann::json *>> pending;
        if (value.is_structured() && !value.empty())
            pending.emplace_back(&value, &copy);
        while (!pending.empty())
        {
            const auto [source, target] = pending.back();
            pending.pop_back();
            if (source->is_object())
            {
                for (auto item = source->cbegin(); item != source->cend(); ++item)
                {
                    nlohmann::json &element = ((*target)[item.key()] = shell(item.value()));
                    if (item->is_structured() && !item->empty())
                        pending.emplace_back(&item.value(), &element);
                }
            }
            else
            {
                auto &elements = target->get_ref<nlohmann::json::array_t &>();
                elements.reserve(source->size());
                for (const nlohmann::json &item : *source)
                    elements.push_back(shell(item));
                for (std::size_t index = 0; index < elements.size(); ++index)
                {
                    const nlohmann::json &item = (*source)[index];
                    if (item.is_structured() && !item.empty())
                        pending.emplace_back(&item, &elements[index]);
                }
            }
        }
        return copy;
    }
}
