// ResourceUUID.cpp
#include "engine/io/ResourceUUID.hpp"

#include <cctype>
#include <format>
#include <string>
#include <system_error>

#include "engine/Logger.hpp"

namespace N2Engine::IO
{
    Math::UUID ResourceUUID::s_projectNamespace = Math::UUID::ZERO;
    bool ResourceUUID::s_initialized = false;

    std::string ResourceUUID::NormalizeProjectDir(const std::filesystem::path& projectDir)
    {
        std::error_code error;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(projectDir, error);
        if (error)
        {
            resolved = std::filesystem::absolute(projectDir, error).lexically_normal();
        }

        std::string text = resolved.generic_string();
        // "C:/game/" and "C:/game" are one folder; a root ("/", "C:/") keeps its slash
        const bool hasDrive = text.size() >= 2 && text[1] == ':' &&
                              std::isalpha(static_cast<unsigned char>(text[0]));
        const size_t rootLength = hasDrive ? 3 : 1;
        while (text.size() > rootLength && text.back() == '/')
        {
            text.pop_back();
        }
        // Windows paths are case-insensitive, and CMake writes the drive letter upper-case; the process's
        // current directory (which a relative path resolves against) may have it lower-case
        if (hasDrive)
        {
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        }
        return text;
    }

    Math::UUID ResourceUUID::NamespaceForProjectDir(const std::filesystem::path& projectDir)
    {
        return Math::UUID::GenerateNameBased(Math::UUID::ZERO, NormalizeProjectDir(projectDir));
    }

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