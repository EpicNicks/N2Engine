#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include <editor-server/Host.hpp>
#include <editor-server/HostOptions.hpp>
#include <engine/Application.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/scripting/LuaRuntime.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
namespace fs = std::filesystem;

// A project the engine refuses to start (network.preset deterministic with unmet requirements, which every project is
// until a backend declares CrossPlatform) ends RunHost with exit code 1 before it edits, plays or serves anything.
// In editor_project_tests because RunHost points the process-wide script policy at the project; this file restores it.
// A host that went past the refusal would start serving, so a regression makes this test hang rather than pass.
TEST(EditorRefusedProjectTest, RunHostExitsOneForARefusedDeterministicProjectBeforeEditingOrPlaying)
{
    const fs::path base = fs::temp_directory_path() / "n2-run-host-refused-test";
    std::error_code error;
    fs::remove_all(base, error);

    const fs::path projectDir = base / "Refused";
    auto created = IO::CreateProject(projectDir);
    ASSERT_TRUE(created) << created.error().message;
    IO::ProjectFile project = *created;
    project.settings["network"] = nlohmann::json{{"preset", "deterministic"}};
    const auto saved = project.Save(projectDir);
    ASSERT_TRUE(saved) << saved.error();

    const bool quitAllowed = Application::IsScriptQuitAllowed();
    const Scripting::LuaExecutionPolicy policy = Scripting::LuaRuntime::GetExecutionPolicy();

    // The host that edits
    HostOptions edit;
    edit.projectPath = projectDir.string();
    edit.port = 0;
    edit.renderer = HostRenderer::Software;
    EXPECT_EQ(RunHost(edit), 1);

    // The --play child: it never enters play mode (the startup scene exists, so only the refusal can stop it)
    HostOptions play = edit;
    play.playScene = std::string(IO::DefaultStartupScene);
    EXPECT_EQ(RunHost(play), 1);

    // Nothing was started
    EXPECT_FALSE(Application::GetInstance().GetWindow().IsValid());
    EXPECT_EQ(Application::GetInstance().Get3DPhysicsBackend(), nullptr);

    Application::SetScriptQuitAllowed(quitAllowed);
    Scripting::LuaRuntime::SetExecutionPolicy(policy);
    fs::remove_all(base, error);
}
