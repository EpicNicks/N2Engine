#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include <editor-server/ExtensionHost.hpp>

using namespace N2Engine::Editor;
namespace fs = std::filesystem;

// editor-server/editor-api.lua (the LuaLS annotations of the Editor API, separate from engine-api.lua) is checked against
// what the host really registers: a state of an extension that declares every capability is made, the functions of its
// `Editor` table are listed (name, number of parameters, whether it takes `...`), and the annotation file must have a
// stub for each of them, no stub that is not registered, the same parameter list, and `---@param` lines that name the
// parameters of the stub in order. The hook names and the capability names of the file are checked against the host's
// own lists too. engine-api.lua has no such check; this one is cheap and keeps the editor file honest.
namespace
{
    std::string ReadWholeFile(const fs::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        std::string result = text.str();
        result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
        return result;
    }

    std::vector<std::string> SplitLines(const std::string &text)
    {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return lines;
    }

    bool StartsWith(const std::string &text, const std::string_view prefix)
    {
        return text.compare(0, prefix.size(), prefix) == 0;
    }

    std::string Trim(const std::string &text)
    {
        const std::size_t first = text.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            return {};
        }
        const std::size_t last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    // A function of the Editor table as the host's state holds it
    struct Registered
    {
        std::string name;
        int params = 0;
        bool vararg = false;
    };

    // A stub of the annotation file: `function Editor.Scene.GetEntity(id) end`, with the names of the `---@param` lines
    // of the comment block right above it
    struct Stub
    {
        std::string name;
        std::vector<std::string> params;
        std::vector<std::string> documented;
        bool vararg = false;
        int line = 0;
    };

    // The table on top of the stack, walked: every function in it (and in the tables in it) is recorded. The stack is
    // left as it was.
    void Walk(lua_State *L, const std::string &prefix, const int depth, std::vector<Registered> &out)
    {
        if (depth > 4 || lua_checkstack(L, 8) == 0)
        {
            ADD_FAILURE() << prefix << " is nested too deeply";
            return;
        }
        lua_pushnil(L);
        while (lua_next(L, -2) != 0)
        {
            // key at -2, value at -1
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                const std::string name = prefix + "." + std::string(lua_tostring(L, -2));
                if (lua_type(L, -1) == LUA_TFUNCTION)
                {
                    EXPECT_EQ(lua_iscfunction(L, -1), 0) << name << " is a C function: its parameters cannot be read";
                    lua_pushvalue(L, -1);
                    lua_Debug info;
                    lua_getinfo(L, ">u", &info);
                    Registered entry;
                    entry.name = name;
                    entry.params = info.nparams;
                    entry.vararg = info.isvararg != 0;
                    out.push_back(entry);
                }
                else if (lua_type(L, -1) == LUA_TTABLE)
                {
                    Walk(L, name, depth + 1, out);
                }
            }
            lua_pop(L, 1);
        }
    }

    // The functions of `Editor` in a state of an extension that declares every capability, sorted by name
    std::vector<Registered> RegisteredFunctions()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path root = fs::temp_directory_path() / ("n2-ext-api-" + std::to_string(stamp) + "-" +
                                                           std::to_string(counter++));
        fs::create_directories(root / "editor");
        {
            std::ofstream file(root / "editor" / "all.lua", std::ios::binary | std::ios::trunc);
            file << "return Editor.Extension {\n"
                    "  name = \"Every capability\",\n"
                    "  apiVersion = 1,\n"
                    "  capabilities = { \"scene.read\", \"scene.write\", \"assets.read\", \"assets.write\" },\n"
                    "}\n";
        }

        std::vector<Registered> found;
        {
            ExtensionHost host(root);
            host.Load();
            EXPECT_EQ(host.LoadedCount(), 1u) << "the file that declares every capability did not load";
            int states = 0;
            host.ForEachStateForTest([&](const std::string &, lua_State *state)
            {
                ++states;
                lua_getglobal(state, "Editor");
                if (lua_type(state, -1) == LUA_TTABLE)
                {
                    Walk(state, "Editor", 0, found);
                }
                else
                {
                    ADD_FAILURE() << "the state has no Editor table";
                }
                lua_settop(state, 0);
            });
            EXPECT_EQ(states, 1);
            (void)host.Shutdown(std::chrono::milliseconds(2000));
        }
        std::error_code error;
        fs::remove_all(root, error);

        std::sort(found.begin(), found.end(), [](const Registered &a, const Registered &b) { return a.name < b.name; });
        return found;
    }

    std::vector<std::string> SplitParams(const std::string &list)
    {
        std::vector<std::string> names;
        std::size_t start = 0;
        while (start <= list.size())
        {
            std::size_t end = list.find(',', start);
            if (end == std::string::npos)
            {
                end = list.size();
            }
            const std::string name = Trim(list.substr(start, end - start));
            if (!name.empty())
            {
                names.push_back(name);
            }
            start = end + 1;
        }
        return names;
    }

    std::vector<Stub> ParseStubs(const std::vector<std::string> &lines)
    {
        std::vector<Stub> stubs;
        std::vector<std::string> documented;
        int number = 0;
        for (const std::string &line : lines)
        {
            ++number;
            if (StartsWith(line, "---@param "))
            {
                std::string rest = line.substr(10);
                const std::size_t space = rest.find(' ');
                std::string name = space == std::string::npos ? rest : rest.substr(0, space);
                if (!name.empty() && name.back() == '?')
                {
                    name.pop_back();
                }
                documented.push_back(name);
                continue;
            }
            if (StartsWith(line, "---"))
            {
                continue; // another line of the comment block: the block goes on
            }
            if (StartsWith(line, "function Editor."))
            {
                const std::size_t open = line.find('(');
                const std::size_t close = line.find(')', open == std::string::npos ? 0 : open);
                if (open == std::string::npos || close == std::string::npos)
                {
                    ADD_FAILURE() << "editor-api.lua line " << number << " is not a stub: " << line;
                }
                else
                {
                    Stub stub;
                    stub.name = Trim(line.substr(9, open - 9));
                    stub.line = number;
                    for (const std::string &param : SplitParams(line.substr(open + 1, close - open - 1)))
                    {
                        if (param == "...")
                        {
                            stub.vararg = true;
                        }
                        stub.params.push_back(param);
                    }
                    stub.documented = documented;
                    stubs.push_back(std::move(stub));
                }
            }
            documented.clear(); // a stub or any line that is not a comment ends the block
        }
        return stubs;
    }

    // The members of the annotation file's aliases that hold a dot (---| "scene.read" ...): the capabilities. The members of
    // the other aliases (roles, parameter types) have none.
    std::set<std::string> CapabilityMembers(const std::vector<std::string> &lines)
    {
        std::set<std::string> names;
        constexpr std::string_view prefix = "---| \"";
        for (const std::string &line : lines)
        {
            if (!StartsWith(line, prefix))
            {
                continue;
            }
            const std::size_t close = line.find('"', prefix.size());
            if (close == std::string::npos)
            {
                continue;
            }
            const std::string name = line.substr(prefix.size(), close - prefix.size());
            if (name.find('.') != std::string::npos)
            {
                names.insert(name);
            }
        }
        return names;
    }

    // The names of the `---@field OnXxx` lines (the hooks table of the annotation file)
    std::set<std::string> HookFields(const std::vector<std::string> &lines)
    {
        std::set<std::string> names;
        constexpr std::string_view prefix = "---@field On";
        constexpr std::size_t nameStart = std::string_view("---@field ").size();
        for (const std::string &line : lines)
        {
            if (!StartsWith(line, prefix))
            {
                continue;
            }
            std::size_t end = nameStart;
            while (end < line.size() && line[end] != '?' && line[end] != ' ')
            {
                ++end;
            }
            names.insert(line.substr(nameStart, end - nameStart));
        }
        return names;
    }

    std::string Describe(const std::set<std::string> &names)
    {
        std::string text;
        for (const std::string &name : names)
        {
            text += (text.empty() ? "" : ", ") + name;
        }
        return text.empty() ? "(none)" : text;
    }
}

TEST(ExtensionApiAnnotationTest, EveryRegisteredFunctionHasAStubAndEveryStubIsRegistered)
{
    const std::vector<std::string> lines = SplitLines(ReadWholeFile(N2_EDITOR_API_LUA));
    ASSERT_GT(lines.size(), 10u) << "could not read " << N2_EDITOR_API_LUA;
    const std::vector<Stub> stubs = ParseStubs(lines);
    const std::vector<Registered> registered = RegisteredFunctions();
    ASSERT_FALSE(registered.empty());

    std::set<std::string> registeredNames;
    for (const Registered &entry : registered)
    {
        registeredNames.insert(entry.name);
    }
    std::set<std::string> stubNames;
    for (const Stub &stub : stubs)
    {
        EXPECT_TRUE(stubNames.insert(stub.name).second) << stub.name << " has two stubs in editor-api.lua";
    }

    std::set<std::string> missing;
    for (const std::string &name : registeredNames)
    {
        if (!stubNames.contains(name))
        {
            missing.insert(name);
        }
    }
    std::set<std::string> extra;
    for (const std::string &name : stubNames)
    {
        if (!registeredNames.contains(name))
        {
            extra.insert(name);
        }
    }
    EXPECT_TRUE(missing.empty()) << "registered by the host but not in editor-api.lua: " << Describe(missing);
    EXPECT_TRUE(extra.empty()) << "in editor-api.lua but not registered by the host: " << Describe(extra);

    // The same list of parameters, and `---@param` lines that name them in order
    for (const Stub &stub : stubs)
    {
        const auto found = std::find_if(registered.begin(), registered.end(),
                                        [&stub](const Registered &entry) { return entry.name == stub.name; });
        if (found == registered.end())
        {
            continue;
        }
        const int named = static_cast<int>(stub.params.size()) - (stub.vararg ? 1 : 0);
        EXPECT_EQ(named, found->params) << stub.name << " (editor-api.lua line " << stub.line
                                        << "): the stub names a different number of parameters than the host's function";
        EXPECT_EQ(stub.vararg, found->vararg) << stub.name << " (editor-api.lua line " << stub.line
                                              << "): the stub and the host's function disagree about `...`";
        EXPECT_EQ(stub.documented, stub.params) << stub.name << " (editor-api.lua line " << stub.line
                                                << "): the ---@param lines must name the stub's parameters, in order";
    }
}

TEST(ExtensionApiAnnotationTest, TheHookNamesAndTheCapabilityNamesAreTheHosts)
{
    const std::vector<std::string> lines = SplitLines(ReadWholeFile(N2_EDITOR_API_LUA));
    ASSERT_GT(lines.size(), 10u) << "could not read " << N2_EDITOR_API_LUA;

    std::set<std::string> hooks;
    for (const std::string_view name : ExtensionHost::HookNames)
    {
        hooks.insert(std::string(name));
    }
    EXPECT_EQ(HookFields(lines), hooks) << "the hooks of editor-api.lua: " << Describe(HookFields(lines))
                                        << "; the host's: " << Describe(hooks);

    std::set<std::string> capabilities;
    for (const std::string_view name : ExtensionHost::CapabilityNames)
    {
        capabilities.insert(std::string(name));
    }
    const std::set<std::string> annotated = CapabilityMembers(lines);
    EXPECT_EQ(annotated, capabilities) << "the capabilities of editor-api.lua: " << Describe(annotated)
                                       << "; the host's: " << Describe(capabilities);
}

TEST(ExtensionApiAnnotationTest, TheFileIsAPlainDefinitionFile)
{
    const std::string text = ReadWholeFile(N2_EDITOR_API_LUA);
    ASSERT_FALSE(text.empty()) << "could not read " << N2_EDITOR_API_LUA;
    EXPECT_TRUE(StartsWith(text, "---@meta\n")) << "a LuaLS definition file starts with ---@meta";
    EXPECT_EQ(text.find('\t'), std::string::npos) << "no tab characters";
    for (const char c : text)
    {
        ASSERT_TRUE(c == '\n' || (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F))
            << "editor-api.lua is plain ASCII";
    }
    // Nothing in it is code that runs: the only statements are the namespace tables and the empty stubs
    for (const std::string &line : SplitLines(text))
    {
        if (line.empty() || StartsWith(line, "--"))
        {
            continue;
        }
        const bool table = StartsWith(line, "Editor") && line.size() >= 5 && line.substr(line.size() - 5) == " = {}";
        const bool stub = StartsWith(line, "function Editor.") && line.size() >= 4 && line.substr(line.size() - 4) == " end";
        EXPECT_TRUE(table || stub) << "not a namespace table or an empty stub: " << line;
    }
}
