#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <optional>
#include <algorithm>

#include <math/UUID.hpp>

#include "engine/base/Asset.hpp"
#include "engine/common/UUIDHash.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"

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
    /// The one place to look assets up by UUID or path. Files tracked by ResourceLoader (the project's
    /// assets, with stable .meta UUIDs) are loaded and cached there, and every lookup here sees them;
    /// this registry itself only holds assets registered at runtime and files outside the project,
    /// whose UUIDs are random per run.
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
            // Project assets are cached in ResourceLoader under their .meta UUIDs
            return ResourceLoader::Instance().GetCachedByUUID<T>(uuid);
        }

        /// GetAsset, or else loads the project asset with this .meta UUID. How saved asset references
        /// are resolved.
        template <typename T>
        std::shared_ptr<T> LoadByUUID(const Math::UUID &uuid)
        {
            if (auto existing = GetAsset<T>(uuid))
            {
                return existing;
            }
            return ResourceLoader::Instance().LoadByUUID<T>(uuid);
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
            if (auto projectPath = FindProjectPath(path))
            {
                return ResourceLoader::Instance().GetCached<T>(*projectPath);
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

            // A project asset loads through ResourceLoader, so it gets its stable .meta UUID and a single
            // cache entry (loading it here too gave it a second copy under a random UUID)
            if (auto projectPath = FindProjectPath(path))
            {
                return ResourceLoader::Instance().Load<T>(*projectPath);
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

        // Register a custom loader function. It's registered with ResourceLoader too, so project files
        // with this extension are scanned (given .meta UUIDs) and load there.
        void RegisterLoader(const std::string &extension, LoaderFunc loader)
        {
            ResourceLoader::Instance().RegisterLoader(extension, loader);
            _loaders[extension] = std::move(loader);
        }

        // Helper template for simple loaders
        template <typename T>
        void RegisterSimpleLoader(const std::string &extension)
        {
            static_assert(std::is_base_of_v<Base::Asset, T>, "T must be an Asset type");

            RegisterLoader(extension, [](const std::filesystem::path &path) -> std::shared_ptr<Base::Asset>
            {
                auto asset = std::make_shared<T>();
                if (asset->Load(path))
                {
                    return asset;
                }
                return nullptr;
            });
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

        // These cover this registry's own assets; project assets are managed by ResourceLoader
        // (ClearCache/RemoveUnused there)
        void UnregisterAsset(const Math::UUID &uuid);
        void Clear();

        // === Cache Management ===

        void RemoveUnused();

    private:
        Resources() = default;

        [[nodiscard]] bool ResourcePathIsValid() const;
        /// The ResourceLoader path of a file it tracks (a res:// or user:// string, or a file path
        /// under its assets root), or nullopt for anything else
        [[nodiscard]] std::optional<ResourcePath> FindProjectPath(const std::filesystem::path &path) const;

        std::filesystem::path _resourcePath;
        std::unordered_map<Math::UUID, std::shared_ptr<Base::Asset>, UUIDHash> _assetsByUUID;
        std::unordered_map<std::string, std::shared_ptr<Base::Asset>> _assetsByPath;
        std::unordered_map<std::string, LoaderFunc> _loaders;
    };
}