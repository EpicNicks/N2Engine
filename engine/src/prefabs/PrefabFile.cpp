#include "engine/prefabs/PrefabFile.hpp"

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
    std::expected<nlohmann::json, std::string> PrefabFile::Parse(const std::string_view text)
    {
        // Before anything is parsed: a tree 100 000 deep would overflow the stack of the walks over it
        if (JsonLimits::SceneJsonTooDeep(text))
        {
            return std::unexpected(JsonLimits::SceneJsonTooDeepMessage());
        }
        nlohmann::json data = nlohmann::json::parse(text, nullptr, false);
        if (data.is_discarded() || !data.is_object())
        {
            return std::unexpected(std::string("the prefab file doesn't hold a JSON object"));
        }
        const auto name = data.find("name");
        if (name == data.end() || !name->is_string())
        {
            return std::unexpected(std::string("the prefab file has no string \"name\""));
        }
        const auto root = data.find("rootObject");
        if (root == data.end() || !root->is_object())
        {
            return std::unexpected(std::string("the prefab file has no object \"rootObject\""));
        }
        return data;
    }

    bool PrefabFile::Load(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            Logger::Error(std::format("Cannot read prefab file {}", IO::PathToUtf8(path)));
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto parsed = Parse(text);
        if (!parsed)
        {
            Logger::Error(std::format("Prefab file {}: {}", IO::PathToUtf8(path), parsed.error()));
            return false;
        }
        _data = std::move(*parsed);
        return true;
    }

    std::string PrefabFile::GetPrefabName() const
    {
        const auto name = _data.find("name");
        return name != _data.end() && name->is_string() ? name->get<std::string>() : std::string{};
    }

    void PrefabFile::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            IO::Resources::Instance().RegisterSimpleLoader<PrefabFile>(std::string(Extension));
        });
    }
}
