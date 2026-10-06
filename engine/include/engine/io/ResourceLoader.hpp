#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <filesystem>
#include <vector>

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
        
        /// A loaded asset by its .meta UUID, without loading anything
        template <typename T = Base::Asset>
        std::shared_ptr<T> GetCachedByUUID(const Math::UUID& uuid) const;
        
        // === Metadata ===
        const AssetMetadata* GetMetadata(const ResourcePath& resourcePath) const;
        const AssetMetadata* GetMetadata(const Math::UUID& uuid) const;
        Math::UUID GetUUID(const IO::ResourcePath& resourcePath) const;
        
        // === Path Conversion ===
        std::filesystem::path Resolve(const ResourcePath& resourcePath) const;
        IO::ResourcePath MakeResourcePath(const std::filesystem::path& physicalPath) const;
        
        // === Registration ===
        /// A new extension registered after Initialize rescans the assets, so its files get metadata
        void RegisterLoader(const std::string& extension, LoaderFunc loader);
        
        template <typename T>
        void RegisterSimpleLoader(const std::string& extension);
        
        template <typename T>
        void RegisterAsset(std::shared_ptr<T> asset, const ResourcePath& path);
        
        // === Sub-assets (a model's meshes, materials and textures) ===

        /// One entry of a file's sub-asset index
        struct SubAssetIndexEntry
        {
            std::string key;  ///< "mesh/Body"
            std::string type; ///< its resource type, "Mesh"
            Math::UUID uuid;  ///< ResourceUUID::FromSubAsset(parent, key)
        };

        /// Where a sub-asset UUID points: the file holding it and its key there
        struct SubAssetLocation
        {
            ResourcePath parent;
            std::string key;
        };

        /**
         * Records a project file's sub-assets: maps each entry's UUID to (parent, key), so LoadByUUID can load the
         * parent and return the child, and writes the index into the parent's .meta as customData.subAssets
         * ({"mesh/Body": {"type": "Mesh", "uuid": "..."}, ...}) when it changed, keeping the rest of customData. The
         * entries replace any recorded for this parent before. Load calls it when a file with sub-assets first loads
         * (the scan only reads indexes back, it never parses a model). False, doing nothing, for a path that isn't a
         * scanned project file.
         */
        bool SetSubAssetIndex(const ResourcePath& parent, const std::vector<SubAssetIndexEntry>& entries);
        /// The parent and key a sub-asset UUID names, or nullptr for a UUID that isn't a known sub-asset
        [[nodiscard]] const SubAssetLocation* FindSubAssetLocation(const Math::UUID& uuid) const;
        /// A resource type whose files have sub-assets ("Model"). LoadByUUID of an unknown UUID loads every such file
        /// that has no sub-asset index yet (a project whose .import folder is new), once, then looks again.
        void RegisterSubAssetParentType(const std::string& resourceType);

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
        /// Gives a just-loaded asset's sub-assets their UUIDs and resource path, and records the index
        void RegisterSubAssets(const ResourcePath& parent, const Base::Asset& asset);
        /// Whether a .meta's sub-asset index was written for the file as it is now (its customData.subAssetsSource
        /// size and time match the metadata's)
        static bool HasCurrentSubAssetIndex(const AssetMetadata& meta);
        /// Reads a scanned file's .meta sub-asset index into _subAssets (UUIDs re-derived from the path and key); a
        /// stale index (the file changed) drops the old entries and the cached parent instead
        void ReadSubAssetIndex(const AssetMetadata& meta);
        /// The sub-asset a UUID names, loading its parent; nullptr if it isn't one
        std::shared_ptr<Base::Asset> LoadSubAsset(const Math::UUID& uuid);
        /// Loads every sub-asset parent file not yet indexed (and not tried before); whether any loaded
        bool LoadUnindexedSubAssetParents();

        std::filesystem::path _projectRoot;
        std::filesystem::path _assetsRoot;
        std::filesystem::path _metadataRoot;
        std::filesystem::path _userDataRoot;
        
        std::unordered_map<ResourcePath, AssetMetadata, ResourcePath::Hash> _metadata;
        std::unordered_map<Math::UUID, ResourcePath, UUIDHash> _uuidToPath;
        
        std::unordered_map<ResourcePath, std::shared_ptr<Base::Asset>, ResourcePath::Hash> _cache;
        std::unordered_map<Math::UUID, std::shared_ptr<Base::Asset>, UUIDHash> _cacheByUUID;
        
        std::unordered_map<std::string, LoaderFunc> _loaders;

        std::unordered_map<Math::UUID, SubAssetLocation, UUIDHash> _subAssets;
        std::unordered_set<std::string> _subAssetParentTypes;
        std::unordered_set<ResourcePath, ResourcePath::Hash> _subAssetParentsTried;
    };
}

// Template implementations
#include "engine/io/ResourceLoader.inl"