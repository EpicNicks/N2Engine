// TEMPORARY stack probe (removed before the PR is final): runs each operation over a chain of N GameObjects in a child
// process of this very executable and prints which depths survive, to choose the hierarchy cap from measurements.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "DeepSceneJson.hpp"

using namespace N2Engine;
using json = nlohmann::json;

namespace
{
    GameObject::Ptr Built(const std::size_t levels)
    {
        const GameObject::Ptr root = GameObject::Create("root");
        GameObject::Ptr current = root;
        for (std::size_t level = 1; level < levels; ++level)
        {
            const GameObject::Ptr child = GameObject::Create("n");
            current->AddChild(child, false);
            current = child;
        }
        return root;
    }

    void RunOp(const std::string &op, const std::size_t levels)
    {
        if (op == "deserialize")
        {
            const json data = json::parse(TestJson::ChainText(levels));
            const auto root = GameObject::Deserialize(data);
            (void)root;
        }
        else if (op == "destroy")
        {
            const auto root = Built(levels);
            (void)root;
        }
        else if (op == "serialize")
        {
            const auto root = Built(levels);
            const std::string text = root->Serialize().dump();
            (void)text;
        }
        else if (op == "copy")
        {
            const json data = json::parse(TestJson::ChainText(levels));
            const json copy = data;
            (void)copy;
        }
        else if (op == "scene")
        {
            const auto scene = Scene::FromJSON(json::parse(TestJson::SceneText(levels)), true);
            const std::string text = scene->Serialize().dump();
            (void)text;
        }
        else if (op == "instantiate")
        {
            const auto root = PrefabManager::InstantiatePrefab(json::parse(TestJson::PrefabText(levels)));
            (void)root;
        }
        else if (op == "walks")
        {
            const auto root = Built(levels);
            const auto all = root->GetChildrenRecursive();
            const auto found = root->FindChildRecursive("nothing");
            (void)all;
            (void)found;
        }
        else if (op == "manager")
        {
            json data = json::parse(TestJson::SceneText(levels, "ProbeScene"));
            SceneManager::AddScene(data);
            SceneManager::LoadScene("ProbeScene");
            SceneManager::ProcessAnyPendingSceneChange();
        }
    }

    class ProbeEnvironment final : public ::testing::Environment
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
            const std::vector<std::string> ops = {"deserialize", "destroy", "serialize", "copy",
                                                  "scene", "instantiate", "walks", "manager"};
            std::printf("PROBE exe=%s\n", exe.c_str());
            for (const std::string &op : ops)
            {
                std::string line = "PROBE " + op + ":";
                for (const int levels : {64, 128, 192, 256, 288, 320, 384, 448, 512, 640, 768})
                {
#ifdef _WIN32
                    _putenv_s("N2_PROBE_OP", op.c_str());
                    _putenv_s("N2_PROBE_LEVELS", std::to_string(levels).c_str());
                    const std::string command = "\"\"" + exe + "\" --gtest_filter=SceneDepthProbe.Noop > NUL 2>&1\"";
#else
                    setenv("N2_PROBE_OP", op.c_str(), 1);
                    setenv("N2_PROBE_LEVELS", std::to_string(levels).c_str(), 1);
                    const std::string command = "\"" + exe + "\" --gtest_filter=SceneDepthProbe.Noop > /dev/null 2>&1";
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

    [[maybe_unused]] ::testing::Environment *const probeEnvironment =
        ::testing::AddGlobalTestEnvironment(new ProbeEnvironment);
}

TEST(SceneDepthProbe, Noop)
{
}
