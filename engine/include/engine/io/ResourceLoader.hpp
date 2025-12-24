#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <filesystem>

#include <math/UUID.hpp>
#include "engine/base/Asset.hpp"
#include "engine/common/UUIDHash.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/AssetMetadata.hpp"

namespace N2Engine::IO
{
    class ResourceLoader
    {
    public:
        using LoaderFunc = std::function<std::shared_ptr<Base::Asset>(const std::filesystem::path&)>;
        
        static ResourceLoader& Instance()
        {
            static ResourceLoader instance;
            return instance;
        }
        
        // === Initialization ===
        void Initialize(const std::filesystem::path& projectRoot);
        void RescanAssets();
        
        // === Loading ===
        template <typename T = Base::Asset>
        std::shared_ptr<T> Load(const IO::ResourcePath& resourcePath);
        
        template <typename T = Base::Asset>
        std::shared_ptr<T> LoadByUUID(const Math::UUID& uuid);
        
        template <typename T = Base::Asset>
        std::shared_ptr<T> GetCached(const IO::ResourcePath& resourcePath) const;
        
        // === Metadata ===
        const AssetMetadata* GetMetadata(const ResourcePath& resourcePath) const;
        const AssetMetadata* GetMetadata(const Math::UUID& uuid) const;
        Math::UUID GetUUID(const IO::ResourcePath& resourcePath) const;
        
        // === Path Conversion ===
        std::filesystem::path Resolve(const ResourcePath& resourcePath) const;
        IO::ResourcePath MakeResourcePath(const std::filesystem::path& physicalPath) const;
        
        // === Registration ===
        void RegisterLoader(const std::string& extension, LoaderFunc loader);
        
        template <typename T>
        void RegisterSimpleLoader(const std::string& extension);
        
        template <typename T>
        void RegisterAsset(std::shared_ptr<T> asset, const ResourcePath& path);
        
        // === Cache Management ===
        void ClearCache();
        void RemoveUnused();
        
        // === Hot Reload ===
        bool HasSourceChanged(const ResourcePath& resourcePath) const;
        bool Reload(const ResourcePath& resourcePath);
        
        // === Query ===
        bool Exists(const ResourcePath& resourcePath) const;
        std::vector<AssetMetadata> GetAllAssets() const;
        std::vector<AssetMetadata> GetAssetsByType(const std::string& type) const;
        
        std::filesystem::path GetProjectRoot() const { return _projectRoot; }
        std::filesystem::path GetAssetsRoot() const { return _assetsRoot; }
        
    private:
        ResourceLoader() = default;
        
        void ScanDirectory(const std::filesystem::path& directory);
        AssetMetadata CreateOrUpdateMetadata(const std::filesystem::path& sourcePath);
        std::filesystem::path GetMetadataPath(const std::filesystem::path& sourcePath) const;
        std::filesystem::path GetUserDataPath() const;
        
        std::filesystem::path _projectRoot;
        std::filesystem::path _assetsRoot;
        std::filesystem::path _metadataRoot;
        std::filesystem::path _userDataRoot;
        
        std::unordered_map<ResourcePath, AssetMetadata, ResourcePath::Hash> _metadata;
        std::unordered_map<Math::UUID, ResourcePath, UUIDHash> _uuidToPath;
        
        std::unordered_map<ResourcePath, std::shared_ptr<Base::Asset>, ResourcePath::Hash> _cache;
        std::unordered_map<Math::UUID, std::shared_ptr<Base::Asset>, UUIDHash> _cacheByUUID;
        
        std::unordered_map<std::string, LoaderFunc> _loaders;
    };
}

// Template implementations
#include "engine/io/ResourceLoader.inl"