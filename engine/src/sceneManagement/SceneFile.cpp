#include "engine/sceneManagement/SceneFile.hpp"

#include <format>
#include <fstream>
#include <mutex>

#include "engine/Logger.hpp"
#include "engine/io/Resources.hpp"

namespace N2Engine
{
    bool SceneFile::Load(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            Logger::Error(std::format("Cannot read scene file {}", IO::PathToUtf8(path)));
            return false;
        }
        nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
        if (data.is_discarded() || !data.is_object())
        {
            Logger::Error(std::format("Scene file {} doesn't hold a JSON object", IO::PathToUtf8(path)));
            return false;
        }
        _data = std::move(data);
        return true;
    }

    std::string SceneFile::GetSceneName() const
    {
        const auto name = _data.find("name");
        return name != _data.end() && name->is_string() ? name->get<std::string>() : std::string{};
    }

    void SceneFile::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            IO::Resources::Instance().RegisterSimpleLoader<SceneFile>(std::string(Extension));
        });
    }
}
