// ResourceUUID.cpp
#include "engine/io/ResourceUUID.hpp"

#include <format>
#include <string>

#include "engine/Logger.hpp"

namespace N2Engine::IO
{
    Math::UUID ResourceUUID::s_projectNamespace = Math::UUID::ZERO;
    bool ResourceUUID::s_initialized = false;

    void ResourceUUID::Initialize(const Math::UUID& projectNamespace)
    {
        s_projectNamespace = projectNamespace;
        s_initialized = true;
        Logger::Info(std::format("ResourceUUID initialized with namespace: {}",
                                projectNamespace.ToString()));
    }

    Math::UUID ResourceUUID::FromPath(const ResourcePath& path)
    {
        if (!s_initialized)
        {
            Logger::Error("ResourceUUID not initialized! Call Initialize() first.");
            return Math::UUID::ZERO;
        }

        return Math::UUID::GenerateNameBased(s_projectNamespace, path.ToString());
    }

    Math::UUID ResourceUUID::FromSubAsset(const ResourcePath& parentPath, const std::string_view key)
    {
        if (!s_initialized)
        {
            Logger::Error("ResourceUUID not initialized! Call Initialize() first.");
            return Math::UUID::ZERO;
        }

        // Their own namespace, derived from the project's: no file name can produce a sub-asset's UUID
        const Math::UUID subAssetNamespace = Math::UUID::GenerateNameBased(s_projectNamespace, "n2engine:sub-asset");
        return Math::UUID::GenerateNameBased(subAssetNamespace, parentPath.ToString() + "#" + std::string(key));
    }

    const Math::UUID& ResourceUUID::GetProjectNamespace()
    {
        return s_projectNamespace;
    }
}