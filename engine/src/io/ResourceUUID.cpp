// ResourceUUID.cpp
#include "engine/io/ResourceUUID.hpp"
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

    const Math::UUID& ResourceUUID::GetProjectNamespace()
    {
        return s_projectNamespace;
    }
}