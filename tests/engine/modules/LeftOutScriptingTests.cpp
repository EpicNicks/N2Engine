// A build without the scripting module (N2ENGINE_USE_SCRIPTING=OFF; docs/project-modules.html). A scene saved with scripts still
// loads: each LuaComponent keeps its script reference and its data, saves them again as they were read, does nothing, and one
// warning says so. The engine's own macro N2ENGINE_SCRIPTING_ENABLED is defined when the module is in the build, so a full build
// compiles none of these tests (the module's own tests run instead).

#include <gtest/gtest.h>

#ifndef N2ENGINE_SCRIPTING_ENABLED

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/replay/GoldenReplay.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

using namespace N2Engine;

namespace
{
    const char *const kScene = R"({"name":"WithScripts","rootGameObjects":[{"uuid":"00000000-0000-0000-0000-000000000001","name":"holder",
        "children":[],"components":[{"type":"LuaComponent","data":{"isActive":true,
        "scriptUUID":"11111111-2222-3333-4444-555555555555","scriptData":{"speed":7.5,"label":"hello","nested":{"a":[1,2,3]}}}}]}]})";

    /// Counts the warnings logged while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        [[nodiscard]] int Count(const std::string &text) const
        {
            return static_cast<int>(std::ranges::count_if(
                warnings, [&](const std::string &w) { return w.find(text) != std::string::npos; }));
        }

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };
}

TEST(LeftOutScriptingTest, LuaComponentStaysRegistered)
{
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("LuaComponent"));
}

TEST(LeftOutScriptingTest, ASceneWithScriptsLoadsKeepsTheDataAndSavesItAgain)
{
    const auto scene = Scene::FromJSON(nlohmann::json::parse(kScene), true);
    ASSERT_NE(scene, nullptr);
    const GameObject::Ptr holder = scene->FindGameObject("holder");
    ASSERT_NE(holder, nullptr);
    auto *component = holder->GetComponent<Scripting::LuaComponent>();
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->GetScriptData().at("speed"), 7.5);
    EXPECT_EQ(component->GetScriptData().at("label"), "hello");
    EXPECT_EQ(component->GetScriptData().at("nested").at("a").size(), 3u);
    EXPECT_FALSE(component->HasMissingScript());
    EXPECT_FALSE(component->IsFaulted());

    // Nothing runs, and nothing fails when the engine calls its lifecycle
    component->OnAttach();
    component->OnUpdate();
    component->OnFixedUpdate();
    component->OnDestroy();

    const nlohmann::json saved = scene->Serialize();
    const nlohmann::json &saved0 = saved.at("rootGameObjects").at(0).at("components").at(0).at("data");
    EXPECT_EQ(saved0.at("scriptUUID"), "11111111-2222-3333-4444-555555555555");
    EXPECT_EQ(saved0.at("scriptData").at("speed"), 7.5);

    const auto reloaded = Scene::FromJSON(saved, true);
    ASSERT_NE(reloaded, nullptr);
    const auto *again = reloaded->FindGameObject("holder")->GetComponent<Scripting::LuaComponent>();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->GetScriptData(), component->GetScriptData());
}

TEST(LeftOutScriptingTest, LoadingScriptsWarnsAtMostOnce)
{
    WarningCapture capture;
    for (int i = 0; i < 3; ++i)
    {
        const auto scene = Scene::FromJSON(nlohmann::json::parse(kScene), true);
        ASSERT_NE(scene, nullptr);
    }
    EXPECT_LE(capture.Count("N2ENGINE_USE_SCRIPTING"), 1) << "once per process, not once per component";
}

TEST(LeftOutScriptingTest, TheEditorFieldsKeepTheScriptAndItsData)
{
    const auto scene = Scene::FromJSON(nlohmann::json::parse(kScene), true);
    ASSERT_NE(scene, nullptr);
    auto *component = scene->FindGameObject("holder")->GetComponent<Scripting::LuaComponent>();
    ASSERT_NE(component, nullptr);
    const std::vector<FieldInfo> fields = component->DescribeFields();
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0].name, "scriptUUID");
    component->SetEditorFields(nlohmann::json::parse(R"({"scriptData":{"speed":9}})"), nullptr);
    EXPECT_EQ(component->GetScriptData().at("speed"), 9);
    EXPECT_EQ(component->GetScriptData().at("label"), "hello") << "the other keys are kept";
}

TEST(LeftOutScriptingTest, TheReplayHasOnlyTheTransformsScene)
{
    const std::vector<std::string> names = Replay::SceneNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "transforms");
}

#endif
