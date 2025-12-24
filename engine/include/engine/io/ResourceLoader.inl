#pragma once

#include "engine/io/ResourceLoader.hpp"
#include "ResourceUUID.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::IO
{
    template <typename T>
    std::shared_ptr<T> ResourceLoader::Load(const ResourcePath& resourcePath)
    {
        static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

        if (auto cached = GetCached<T>(resourcePath))
        {
            return cached;
        }

        auto metaIt = _metadata.find(resourcePath);
        if (metaIt == _metadata.end())
        {
            Logger::Error(std::format("Resource not found: {}", resourcePath.ToString()));
            return nullptr;
        }

        const AssetMetadata& meta = metaIt->second;
        std::filesystem::path sourcePath = Resolve(resourcePath);

        if (!std::filesystem::exists(sourcePath))
        {
            Logger::Error(std::format("Source file missing: {}", sourcePath.string()));
            return nullptr;
        }

        std::string ext = sourcePath.extension().string();
        std::ranges::transform(ext, ext.begin(), ::tolower);

        auto loaderIt = _loaders.find(ext);
        if (loaderIt == _loaders.end())
        {
            Logger::Error(std::format("No loader for extension: {}", ext));
            return nullptr;
        }

        auto asset = loaderIt->second(sourcePath);
        if (!asset)
        {
            return nullptr;
        }

        asset->SetUUID(meta.uuid);
        asset->SetResourcePath(resourcePath);

        _cache[resourcePath] = asset;
        _cacheByUUID[meta.uuid] = asset;

        return std::dynamic_pointer_cast<T>(asset);
    }

    template <typename T>
    std::shared_ptr<T> ResourceLoader::LoadByUUID(const Math::UUID& uuid)
    {
        if (auto it = _cacheByUUID.find(uuid); it != _cacheByUUID.end())
        {
            return std::dynamic_pointer_cast<T>(it->second);
        }

        auto pathIt = _uuidToPath.find(uuid);
        if (pathIt == _uuidToPath.end())
        {
            return nullptr;
        }

        return Load<T>(pathIt->second);
    }

    template <typename T>
    std::shared_ptr<T> ResourceLoader::GetCached(const ResourcePath& resourcePath) const
    {
        auto it = _cache.find(resourcePath);
        if (it != _cache.end())
        {
            return std::dynamic_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    template <typename T>
    void ResourceLoader::RegisterSimpleLoader(const std::string& extension)
    {
        static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

        _loaders[extension] = [](const std::filesystem::path& path) -> std::shared_ptr<Base::Asset>
        {
            auto asset = std::make_shared<T>();
            if (asset->Load(path))
            {
                return asset;
            }
            return nullptr;
        };
    }

    template <typename T>
    void ResourceLoader::RegisterAsset(std::shared_ptr<T> asset, const ResourcePath& path)
    {
        static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

        _cache[path] = asset;
        _cacheByUUID[asset->GetUUID()] = asset;
    }
}