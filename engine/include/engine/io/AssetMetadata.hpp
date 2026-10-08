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
        /// The source file's modification time (Unix milliseconds) and size as the last scan saw them. Kept in memory
        /// (and in .n2/asset-state.json, see ResourceLoader), not in the .meta: they change with every edit of the
        /// file, and a .meta is committed to version control
        std::uint64_t lastModified = 0;
        std::size_t fileSize = 0;
        /// Import settings (and the sub-asset index): what a .meta keeps besides the UUID and the type
        nlohmann::json customData;
        /// Set by FromFile when the .meta still had lastModified and fileSize (written before they moved to the
        /// state file): the scan rewrites it without them, once
        bool hadStateFields = false;

        static AssetMetadata FromFile(const std::filesystem::path& metaPath);
        bool SaveToFile(const std::filesystem::path& metaPath) const;
    };
}