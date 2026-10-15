// TEMPORARY editor stack probe (removed before the PR is final): LoadScene of a chain of N GameObjects in a child
// process of this executable, to see which depths survive.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>

#include "DeepSceneJson.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;

namespace
{
    void RunOp(const std::string &op, const std::size_t levels)
    {
        EditorServer server;
        BufferWriter w;
        w.WriteString(TestJson::SceneText(levels, "Probe"));
        (void)server.ExecuteCommand(static_cast<uint8_t>(CommandType::LoadScene), w.Release());
        if (op == "loadscene-twice")
        {
            BufferWriter again;
            again.WriteString(TestJson::SceneText(levels, "Probe"));
            (void)server.ExecuteCommand(static_cast<uint8_t>(CommandType::LoadScene), again.Release());
        }
    }

    class EditorProbeEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            if (const char *op = std::getenv("N2_PROBE_OP"))
            {
                RunOp(op, static_cast<std::size_t>(std::atoi(std::getenv("N2_PROBE_LEVELS"))));
                std::fflush(stdout);
                std::_Exit(0);
            }
            const std::string exe = ::testing::internal::GetArgvs().at(0);
            const std::vector<std::string> ops = {"loadscene", "loadscene-twice"};
            std::printf("PROBE exe=%s\n", exe.c_str());
            for (const std::string &op : ops)
            {
                std::string line = "PROBE " + op + ":";
                for (const int levels : {64, 128, 192, 256, 288, 320, 384, 448, 512, 640, 768})
                {
#ifdef _WIN32
                    _putenv_s("N2_PROBE_OP", op.c_str());
                    _putenv_s("N2_PROBE_LEVELS", std::to_string(levels).c_str());
                    const std::string command = "\"\"" + exe + "\" --gtest_filter=EditorSceneDepthProbe.Noop > NUL 2>&1\"";
#else
                    setenv("N2_PROBE_OP", op.c_str(), 1);
                    setenv("N2_PROBE_LEVELS", std::to_string(levels).c_str(), 1);
                    const std::string command = "\"" + exe + "\" --gtest_filter=EditorSceneDepthProbe.Noop > /dev/null 2>&1";
#endif
                    const int code = std::system(command.c_str());
                    line += " " + std::to_string(levels) + (code == 0 ? "=ok" : "=FAIL(" + std::to_string(code) + ")");
                }
                std::printf("%s\n", line.c_str());
                std::fflush(stdout);
            }
#ifdef _WIN32
            _putenv_s("N2_PROBE_OP", "");
#else
            unsetenv("N2_PROBE_OP");
#endif
        }
    };

    [[maybe_unused]] ::testing::Environment *const editorProbeEnvironment =
        ::testing::AddGlobalTestEnvironment(new EditorProbeEnvironment);
}

TEST(EditorSceneDepthProbe, Noop)
{
}
