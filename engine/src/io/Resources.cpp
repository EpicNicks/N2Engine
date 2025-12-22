#include "engine/io/Resources.hpp"

#include <fstream>

#include <renderer/common/IShader.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/audio/AudioClip.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"

namespace N2Engine
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

    Renderer::Common::IShader *Resources::LoadShader(
        const std::string &vertexShaderPath,
        const std::string &fragmentShaderPath)
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
        auto it = _assetsByUUID.find(uuid);
        if (it != _assetsByUUID.end())
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
            if (it->second.use_count() <= 1)
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
                it = _assetsByUUID.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}