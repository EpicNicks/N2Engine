#include "engine/io/Resources.hpp"

#include <algorithm>
#include <fstream>

#include <renderer/common/IShader.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/audio/AudioClip.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::IO
{
    bool Resources::ResourcePathIsValid() const
    {
        return std::filesystem::exists(_resourcePath) && std::filesystem::is_directory(_resourcePath);
    }

    void Resources::SetResourcePath(const std::filesystem::path &path)
    {
        _resourcePath = path;
    }

    std::filesystem::path Resources::ResolvePath(const std::filesystem::path &relativePath) const
    {
        if (relativePath.is_absolute())
        {
            return relativePath;
        }

        if (ResourcePathIsValid())
        {
            return _resourcePath / relativePath;
        }

        return relativePath;
    }

    std::optional<ResourcePath> Resources::FindProjectPath(const std::filesystem::path &path) const
    {
        const ResourceLoader &loader = ResourceLoader::Instance();

        const std::string spelled = path.string();
        if (spelled.starts_with("res://") || spelled.starts_with("user://"))
        {
            if (ResourcePath resourcePath(spelled); loader.Exists(resourcePath))
            {
                return resourcePath;
            }
            return std::nullopt;
        }

        if (loader.GetAssetsRoot().empty())
        {
            return std::nullopt; // ResourceLoader isn't initialized, so it tracks no files
        }

        std::error_code pathError;
        std::error_code rootError;
        const std::filesystem::path physicalPath = std::filesystem::absolute(ResolvePath(path), pathError);
        const std::filesystem::path assetsRoot = std::filesystem::absolute(loader.GetAssetsRoot(), rootError);
        if (pathError || rootError)
        {
            return std::nullopt;
        }

        // Cheap lexical test first, so lookups of files elsewhere don't touch the filesystem. (A file
        // spelled with a different 8.3 short/long form than the root isn't recognised.)
        const std::filesystem::path fromRoot =
            physicalPath.lexically_normal().lexically_relative(assetsRoot.lexically_normal());
        if (fromRoot.empty() || *fromRoot.begin() == "..")
        {
            return std::nullopt;
        }

        // The key itself comes from MakeResourcePath (canonical paths), the same as the scan's keys
        try
        {
            if (ResourcePath resourcePath = loader.MakeResourcePath(physicalPath); loader.Exists(resourcePath))
            {
                return resourcePath;
            }
        }
        catch (const std::filesystem::filesystem_error &)
        {
            // Not a path ResourceLoader can express, so not one of its assets
        }
        return std::nullopt;
    }

    Renderer::Common::IShader* Resources::LoadShader(const std::string &vertexShaderPath,
                                                     const std::string &fragmentShaderPath) const
    {
        auto vertexShaderPathLocal = ResolvePath(vertexShaderPath);
        auto fragmentShaderPathLocal = ResolvePath(fragmentShaderPath);

        if (!std::filesystem::exists(vertexShaderPathLocal) ||
            !std::filesystem::exists(fragmentShaderPathLocal))
        {
            return nullptr;
        }

        std::ifstream vertexFile{vertexShaderPathLocal};
        std::ifstream fragmentFile{fragmentShaderPathLocal};

        if (!vertexFile.is_open() || !fragmentFile.is_open())
        {
            return nullptr;
        }

        std::string vertexSource{
            std::istreambuf_iterator<char>(vertexFile),
            std::istreambuf_iterator<char>()
        };
        std::string fragmentSource{
            std::istreambuf_iterator<char>(fragmentFile),
            std::istreambuf_iterator<char>()
        };

        return Application::GetInstance()
               .GetWindow()
               .GetRenderer()
               ->CreateShaderProgram(vertexSource.c_str(), fragmentSource.c_str());
    }

    void Resources::UnregisterAsset(const Math::UUID &uuid)
    {
        if (auto it = _assetsByUUID.find(uuid); it != _assetsByUUID.end())
        {
            // Find and remove from path map
            for (auto pathIt = _assetsByPath.begin(); pathIt != _assetsByPath.end(); ++pathIt)
            {
                if (pathIt->second == it->second)
                {
                    _assetsByPath.erase(pathIt);
                    break;
                }
            }
            _assetsByUUID.erase(it);
        }
    }

    void Resources::Clear()
    {
        _assetsByUUID.clear();
        _assetsByPath.clear();
    }

    void Resources::RemoveUnused()
    {
        // Remove assets from UUID map that have no other references
        for (auto it = _assetsByUUID.begin(); it != _assetsByUUID.end();)
        {
            // References held here: the UUID map plus one per path the asset is registered under
            const auto sameAsset = [&it](const auto &entry) { return entry.second == it->second; };
            const auto pathRefs = std::ranges::count_if(_assetsByPath, sameAsset);
            if (it->second.use_count() <= 1 + pathRefs)
            {
                std::erase_if(_assetsByPath, sameAsset);
                it = _assetsByUUID.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
