#include "engine/sceneManagement/SceneFile.hpp"

#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#include "engine/Logger.hpp"
#include "engine/io/Resources.hpp"
#include "engine/serialization/JsonLimits.hpp"

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
        // Read whole, so the nesting is checked before anything is parsed (a scene file is untrusted data: a client
        // can write one, and a tree 100 000 deep would overflow the stack of the walks over it)
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (JsonLimits::SceneJsonTooDeep(text))
        {
            Logger::Error(std::format("Scene file {}: {}", IO::PathToUtf8(path), JsonLimits::SceneJsonTooDeepMessage()));
            return false;
        }
        nlohmann::json data = nlohmann::json::parse(text, nullptr, false);
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
