#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <functional>

#include <math/UUID.hpp>

#include "engine/base/Asset.hpp"
#include "engine/common/UUIDHash.hpp"

namespace Renderer::Common
{
    class IShader;
}

namespace N2Engine::Audio
{
    class AudioClip;
}

namespace N2Engine::IO
{
    class Resources
    {
    public:
        // Loader function type - must be declared before use
        using LoaderFunc = std::function<std::shared_ptr<Base::Asset>(const std::filesystem::path &)>;

        static Resources &Instance()
        {
            static Resources instance;
            return instance;
        }

        // === Path Management ===

        [[nodiscard]] std::filesystem::path GetResourcePath() const { return _resourcePath; }
        void SetResourcePath(const std::filesystem::path &path);
        [[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path &relativePath) const;

        // === Asset Lookup by UUID ===

        template <typename T>
        std::shared_ptr<T> GetAsset(const Math::UUID &uuid)
        {
            if (auto it = _assetsByUUID.find(uuid); it != _assetsByUUID.end())
            {
                return std::dynamic_pointer_cast<T>(it->second);
            }
            return nullptr;
        }

        // === Asset Lookup by Path ===

        template <typename T>
        std::shared_ptr<T> GetAsset(const std::filesystem::path &path)
        {
            auto resolved = ResolvePath(path);
            if (auto it = _assetsByPath.find(resolved.string()); it != _assetsByPath.end())
            {
                return std::dynamic_pointer_cast<T>(it->second);
            }
            return nullptr;
        }

        // === Generic Asset Loading ===

        template <typename T>
        std::shared_ptr<T> Load(const std::filesystem::path &path)
        {
            static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

            // Check cache first
            if (auto existing = GetAsset<T>(path))
            {
                return existing;
            }

            auto resolved = ResolvePath(path);

            if (!std::filesystem::exists(resolved))
            {
                return nullptr;
            }

            // Get extension
            std::string ext = resolved.extension().string();
            if (ext.empty())
            {
                return nullptr;
            }

            // Convert to lowercase for case-insensitive comparison
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            // Find and use loader
            auto loaderIt = _loaders.find(ext);
            if (loaderIt == _loaders.end())
            {
                return nullptr;
            }

            auto asset = loaderIt->second(resolved);
            if (!asset)
            {
                return nullptr;
            }

            // Register the loaded asset
            auto typedAsset = std::dynamic_pointer_cast<T>(asset);
            if (typedAsset)
            {
                RegisterAsset(typedAsset, path);
            }

            return typedAsset;
        }

        // === Loader Registration (usually called by asset types themselves) ===

        // Register a custom loader function
        void RegisterLoader(const std::string &extension, LoaderFunc loader)
        {
            _loaders[extension] = std::move(loader);
        }

        // Helper template for simple loaders
        template <typename T>
        void RegisterSimpleLoader(const std::string &extension)
        {
            static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

            _loaders[extension] = [](const std::filesystem::path &path) -> std::shared_ptr<Base::Asset>
            {
                auto asset = std::make_shared<T>();
                if (asset->Load(path))
                {
                    return asset;
                }
                return nullptr;
            };
        }

        // Helper for auto-registration with custom loader function
        struct LoaderRegistrar
        {
            LoaderRegistrar(std::initializer_list<std::string> extensions, LoaderFunc loader)
            {
                for (const auto &ext : extensions)
                {
                    Resources::Instance().RegisterLoader(ext, loader);
                }
            }
        };

        // === Shader Loading (special case - requires two files) ===

        Renderer::Common::IShader *LoadShader(
            const std::string &vertexShaderPath,
            const std::string &fragmentShaderPath
        ) const;

        // === Asset Registration ===

        template <typename T>
        void RegisterAsset(std::shared_ptr<T> asset, const std::filesystem::path &path = {})
        {
            static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

            _assetsByUUID[asset->GetUUID()] = asset;

            if (!path.empty())
            {
                auto resolved = ResolvePath(path);
                _assetsByPath[resolved.string()] = asset;
            }
        }

        void UnregisterAsset(const Math::UUID &uuid);
        void Clear();

        // === Cache Management ===

        void RemoveUnused();

    private:
        Resources() = default;

        [[nodiscard]] bool ResourcePathIsValid() const;

        std::filesystem::path _resourcePath;
        std::unordered_map<Math::UUID, std::shared_ptr<Base::Asset>, UUIDHash> _assetsByUUID;
        std::unordered_map<std::string, std::shared_ptr<Base::Asset>> _assetsByPath;
        std::unordered_map<std::string, LoaderFunc> _loaders;
    };
}