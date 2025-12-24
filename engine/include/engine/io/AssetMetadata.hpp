#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <math/UUID.hpp>
#include "engine/io/ResourcePath.hpp"

namespace N2Engine::IO
{
    struct AssetMetadata
    {
        Math::UUID uuid;
        ResourcePath resourcePath;
        std::string resourceType;
        std::uint64_t lastModified = 0;
        std::size_t fileSize = 0;
        nlohmann::json customData;
        
        static AssetMetadata FromFile(const std::filesystem::path& metaPath);
        bool SaveToFile(const std::filesystem::path& metaPath) const;
    };
}