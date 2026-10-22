#include "n2server/SceneLoading.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/serialization/JsonLimits.hpp"

#include "editor-server/EditorServer.hpp"

namespace N2Engine::Server
{
    namespace
    {
        std::expected<std::string, std::string> ReadTextFile(const std::filesystem::path &file)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return std::unexpected("the file can't be read");
            }
            std::ostringstream text;
            text << stream.rdbuf();
            if (stream.bad())
            {
                return std::unexpected("the file can't be read");
            }
            return std::move(text).str();
        }
    }

    std::expected<LoadedScene, std::string> LoadSceneForRun(const std::filesystem::path &projectDir,
                                                            const std::string &scenePath)
    {
        const auto resolved = Editor::EditorServer::ResolveScenePath(projectDir / "assets", scenePath);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }
        const std::string resourcePath = resolved->resourcePath.ToString();
        std::error_code error;
        if (!std::filesystem::is_regular_file(resolved->file, error))
        {
            return std::unexpected("scene file not found: " + resourcePath);
        }
        const auto text = ReadTextFile(resolved->file);
        if (!text)
        {
            return std::unexpected("can't read " + resourcePath + ": " + text.error());
        }
        if (JsonLimits::SceneJsonTooDeep(*text))
        {
            return std::unexpected(resourcePath + ": " + JsonLimits::SceneJsonTooDeepMessage());
        }
        const nlohmann::json sceneJson = nlohmann::json::parse(*text, nullptr, false);
        if (sceneJson.is_discarded())
        {
            return std::unexpected("not valid JSON: " + resourcePath);
        }
        const std::uint64_t contentHash = Network::HashSceneJson(sceneJson);
        std::unique_ptr<Scene> scene = Scene::FromJSON(sceneJson, true);
        if (scene == nullptr)
        {
            return std::unexpected("not a valid scene (see the log): " + resourcePath);
        }

        // The file's asset UUID, the same every run: a scene's UUID is its file's
        auto &loader = IO::ResourceLoader::Instance();
        if (!loader.Exists(resolved->resourcePath))
        {
            (void)loader.Reload(resolved->resourcePath);
        }
        scene->SetUUID(IO::ResourceUUID::FromPath(resolved->resourcePath));
        scene->SetResourcePath(resolved->resourcePath);

        Scene *const loaded = scene.get();
        SceneManager::AddScene(std::move(scene), true);
        SceneManager::ProcessAnyPendingSceneChange();
        if (SceneManager::GetCurScene() != loaded)
        {
            return std::unexpected("the scene couldn't be loaded: " + resourcePath);
        }
        return LoadedScene{resourcePath, contentHash};
    }
}
