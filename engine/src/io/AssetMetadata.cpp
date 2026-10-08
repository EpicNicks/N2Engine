#include "engine/io/AssetMetadata.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/MathSerialization.hpp"
#include <fstream>

namespace N2Engine::IO
{
    AssetMetadata AssetMetadata::FromFile(const std::filesystem::path& metaPath)
    {
        std::ifstream file(metaPath);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open metadata file: " + metaPath.string());
        }
        
        nlohmann::json j;
        file >> j;
        
        AssetMetadata meta;
        meta.uuid = j["uuid"].get<Math::UUID>();
        meta.resourcePath = j["resourcePath"].get<ResourcePath>();
        meta.resourceType = j["resourceType"];
        // Written by older versions only: the scan takes over from the state file, and rewrites the .meta without them
        if (j.contains("lastModified") || j.contains("fileSize"))
        {
            meta.hadStateFields = true;
            meta.lastModified = j.value("lastModified", std::uint64_t{0});
            meta.fileSize = j.value("fileSize", std::size_t{0});
        }
        
        if (j.contains("customData"))
        {
            meta.customData = j["customData"];
        }
        
        return meta;
    }
    
    bool AssetMetadata::SaveToFile(const std::filesystem::path& metaPath) const
    {
        std::filesystem::create_directories(metaPath.parent_path());
        
        nlohmann::json j;
        j["uuid"] = uuid.ToString();
        j["resourcePath"] = resourcePath;
        j["resourceType"] = resourceType;

        if (!customData.empty())
        {
            j["customData"] = customData;
        }
        
        std::ofstream file(metaPath);
        if (!file.is_open())
        {
            Logger::Error("Failed to save metadata: " + metaPath.string());
            return false;
        }
        
        file << j.dump(2);
        return true;
    }
}