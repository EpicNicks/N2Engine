// build.targets in project.n2proj (build targets stage 2; engine/include/engine/io/BuildTargets.hpp)
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "engine/io/BuildTargets.hpp"
#include "engine/io/ProjectFile.hpp"

using namespace N2Engine::IO;
using nlohmann::json;

namespace
{
    json Full()
    {
        return json::parse(R"({
            "windows": {"enabled": true, "displayName": "Triangle-Like", "version": "1.2.3", "buildNumber": 12,
                        "renderer": "vulkan", "config": "release", "icon": "res://ui/Icon.png",
                        "modules": {"physics3d": "none", "physics2d": true}},
            "android": {"appId": "com.example.triangle_like", "orientation": "landscape", "renderer": "software",
                        "signing": {"profile": "release-key"}, "displayName": "Триугольник"},
            "web": {"enabled": false, "futureSetting": {"nested": [1, 2, 3]}}
        })");
    }

    std::string ErrorOf(const json &targets)
    {
        const auto checked = BuildTargets::Validate(targets);
        return checked ? std::string{} : checked.error();
    }
}

TEST(BuildTargetsTest, AcceptsEverySettingAndUnknownKeys)
{
    const auto checked = BuildTargets::Validate(Full());
    EXPECT_TRUE(checked) << checked.error();
    EXPECT_TRUE(BuildTargets::Validate(json::object()));
}

TEST(BuildTargetsTest, TargetNames)
{
    for (const char *good : {"windows", "web", "switch-2", "a", "abcdefghijklmnopqrstuvwxyz012345"})
    {
        EXPECT_TRUE(BuildTargets::IsTargetName(good)) << good;
    }
    for (const char *bad : {"", "Windows", "2web", "-web", "we b", "web_gl", "abcdefghijklmnopqrstuvwxyz0123456"})
    {
        EXPECT_FALSE(BuildTargets::IsTargetName(bad)) << bad;
        EXPECT_NE(ErrorOf(json{{bad, json::object()}}).find("is not a target name"), std::string::npos) << bad;
    }
}

TEST(BuildTargetsTest, EachSettingIsCheckedAndTheErrorNamesTheKey)
{
    const std::vector<std::pair<json, std::string>> cases = {
        {json{{"enabled", "yes"}}, "build.targets.t.enabled"},
        {json{{"displayName", ""}}, "build.targets.t.displayName"},
        {json{{"displayName", "tab\there"}}, "build.targets.t.displayName"},
        {json{{"displayName", std::string(65, 'x')}}, "build.targets.t.displayName"},
        {json{{"appId", "game"}}, "build.targets.t.appId"},
        {json{{"appId", "com.1example.game"}}, "build.targets.t.appId"},
        {json{{"appId", "com..game"}}, "build.targets.t.appId"},
        {json{{"appId", "com.example-game.x"}}, "build.targets.t.appId"},
        {json{{"version", "1.2"}}, "build.targets.t.version"},
        {json{{"version", "1.2.3.4"}}, "build.targets.t.version"},
        {json{{"version", "1.2.x"}}, "build.targets.t.version"},
        {json{{"version", "123456.0.0"}}, "build.targets.t.version"},
        {json{{"buildNumber", 0}}, "build.targets.t.buildNumber"},
        {json{{"buildNumber", 2100000001LL}}, "build.targets.t.buildNumber"},
        {json{{"buildNumber", 1.5}}, "build.targets.t.buildNumber"},
        {json{{"orientation", "sideways"}}, "build.targets.t.orientation"},
        {json{{"renderer", "metal"}}, "build.targets.t.renderer"},
        {json{{"config", "profile"}}, "build.targets.t.config"},
        {json{{"icon", "ui/Icon.png"}}, "build.targets.t.icon"},
        {json{{"icon", "res://../Icon.png"}}, "build.targets.t.icon"},
        {json{{"icon", "res://ui/Icon.jpg"}}, "build.targets.t.icon"},
        {json{{"icon", "res://ui\\Icon.png"}}, "build.targets.t.icon"},
        {json{{"icon", "res://.png"}}, "build.targets.t.icon"},
        {json{{"modules", {{"physics3d", "havok"}}}}, "build.targets.t.modules.physics3d"},
        {json{{"modules", {{"physics2d", "yes"}}}}, "build.targets.t.modules.physics2d"},
        {json{{"modules", {{"rendering", true}}}}, "unknown module"},
        {json{{"signing", "release-key"}}, "build.targets.t.signing"},
        {json{{"signing", {{"profile", "has space"}}}}, "build.targets.t.signing.profile"},
        {json{{"signing", {{"keystore", "C:/keys/release.jks"}}}}, "signing holds only a profile name"},
    };
    for (const auto &[target, expected] : cases)
    {
        const std::string error = ErrorOf(json{{"t", target}});
        EXPECT_NE(error.find(expected), std::string::npos) << target.dump() << " -> " << error;
    }
    EXPECT_NE(ErrorOf(json{{"t", "not an object"}}).find("must be an object"), std::string::npos);
    EXPECT_NE(ErrorOf(json::array()).find("keyed by target name"), std::string::npos);
    EXPECT_TRUE(BuildTargets::Validate(json{{"t", {{"icon", "res://ui/Icon.PNG"}}}}));
    EXPECT_TRUE(BuildTargets::Validate(json{{"t", {{"buildNumber", 2100000000LL}}}}));
}

TEST(BuildTargetsTest, NoSecretCanBeStoredAtAnyDepth)
{
    for (const json &target : {
             json{{"keystorePassword", "hunter2"}},
             json{{"signing", {{"profile", "release"}, {"storePass", "x"}}}},
             json{{"future", {{"deep", {{"API_KEY", "x"}}}}}},
             json{{"future", json::array({json{{"privateKey", "x"}}})}},
             json{{"uploadToken", "x"}},
             json{{"Client-Secret", "x"}},
             json{{"notes", "-----BEGIN PRIVATE KEY-----\nMIIE..."}},
             json{{"certs", json::array({"-----BEGIN CERTIFICATE-----"})}},
         })
    {
        const std::string error = ErrorOf(json{{"android", target}});
        EXPECT_NE(error.find("looks like a password, key or other secret"), std::string::npos) << target.dump() << " -> " << error;
        EXPECT_EQ(error.find("hunter2"), std::string::npos) << "the value is never echoed";
    }
}

TEST(BuildTargetsTest, LimitsAndQuotedNames)
{
    json many = json::object();
    for (int i = 0; i <= static_cast<int>(BuildTargets::MaxTargets); ++i)
    {
        many["t" + std::to_string(i)] = json::object();
    }
    EXPECT_NE(ErrorOf(many).find("at most 32 targets"), std::string::npos);
    EXPECT_NE(ErrorOf(json{{"t", {{"future", std::string(70 * 1024, 'x')}}}}).find("KiB"), std::string::npos);
    // A hostile name is quoted without control characters and cut short
    const std::string error = ErrorOf(json{{std::string("bad\nname") + std::string(100, 'x'), json::object()}});
    EXPECT_EQ(error.find('\n'), std::string::npos) << error;
    EXPECT_LT(error.size(), 300u);
}

TEST(BuildTargetsTest, MergePatch)
{
    const json current = Full();
    const auto changed = BuildTargets::ApplyPatch(current, json::parse(R"({"windows": {"version": "1.3.0", "icon": null},
                                                                           "web": null, "linux": {"enabled": true}})"));
    ASSERT_TRUE(changed) << changed.error();
    EXPECT_EQ(changed->at("windows").at("version"), "1.3.0");
    EXPECT_FALSE(changed->at("windows").contains("icon"));
    EXPECT_EQ(changed->at("windows").at("buildNumber"), 12) << "untouched keys stay";
    EXPECT_FALSE(changed->contains("web"));
    EXPECT_TRUE(changed->at("linux").at("enabled").get<bool>());

    const auto refused = BuildTargets::ApplyPatch(current, json{{"windows", {{"version", "one"}}}});
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("build.targets.windows.version"), std::string::npos);
    EXPECT_FALSE(BuildTargets::ApplyPatch(current, json::array()));
    EXPECT_FALSE(BuildTargets::ApplyPatch(current, json{{"android", {{"signing", {{"password", "x"}}}}}}));
}

TEST(BuildTargetsTest, StoredInTheProjectFilesBuildBlockBesideTheModuleList)
{
    ProjectFile project;
    project.name = "Game";
    project.projectId = N2Engine::Math::UUID::Random();
    project.engineVersion = "1.0.0";
    project.unknownKeys["build"] = {{"modules", {{"physics3d", "none"}}}};
    EXPECT_EQ(BuildTargets::Of(project), json::object());

    BuildTargets::Set(project, Full());
    EXPECT_EQ(BuildTargets::Of(project), Full());
    EXPECT_EQ(project.ToJson().at("build").at("modules").at("physics3d"), "none") << "build.modules is kept";

    // Through the file and back
    const auto parsed = ProjectFile::Parse(project.ToText());
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(BuildTargets::Of(*parsed), Full());

    // No targets left: the key goes, and build with it when nothing else is in it
    BuildTargets::Set(project, json::object());
    EXPECT_FALSE(project.ToJson().at("build").contains("targets"));
    project.unknownKeys["build"].erase("modules");
    BuildTargets::Set(project, json::object());
    EXPECT_FALSE(project.ToJson().contains("build"));
}
