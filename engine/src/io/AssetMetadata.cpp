#include "engine/io/AssetMetadata.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ProjectFile.hpp"
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
        // A .meta written before the state file holds the file's real size and time; one written since holds constant
        // zeros (see SaveToFile). Only the real ones are old: the scan rewrites such a file with zeros, once.
        if (j.contains("lastModified") || j.contains("fileSize"))
        {
            meta.hadStateFields = j.value("lastModified", std::uint64_t{0}) != 0 || j.value("fileSize", std::size_t{0}) != 0;
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
        // Constant, never the file's real size and time (those are in .n2/asset-state.json, so an edit of the asset
        // leaves the committed .meta alone). The keys stay because an engine that predates the state file reads them
        // as required and would take their absence for a corrupt .meta and regenerate it, losing customData.
        j["lastModified"] = 0;
        j["fileSize"] = 0;

        if (!customData.empty())
        {
            j["customData"] = customData;
        }

        // Through a temporary file, so a crash or a full disk never leaves half a .meta (import settings are in it)
        if (auto written = WriteTextFileAtomically(metaPath, j.dump(2)); !written)
        {
            Logger::Error("Failed to save metadata " + metaPath.string() + ": " + written.error());
            return false;
        }
        return true;
    }
}