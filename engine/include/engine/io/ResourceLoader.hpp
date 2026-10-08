#pragma once

#include <cstdint>
#include <expected>
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

        /// What a rescan found changed since the scan before it, each list sorted
        struct RescanResult
        {
            /// Files with no metadata before (new, or of an extension registered since)
            std::vector<ResourcePath> added;
            /// Files that were indexed and are gone: their metadata and cache entries are dropped (their .meta
            /// files are kept, so import settings come back with the file)
            std::vector<ResourcePath> removed;
            /// Files whose size or modification time changed
            std::vector<ResourcePath> modified;

            [[nodiscard]] bool Empty() const { return added.empty() && removed.empty() && modified.empty(); }
        };

        /**
         * Makes projectRoot the project: res:// is <projectRoot>/assets, .meta files go in <projectRoot>/.import,
         * and user:// is userDataRoot (created if missing). An empty userDataRoot is the shared folder every project
         * used before projects had ids (ProjectFile::UserDataBase()); a project's own is ProjectFile::UserDataPath().
         * Forgets everything about the previous project, then scans the assets.
         */
        void Initialize(const std::filesystem::path& projectRoot, const std::filesystem::path& userDataRoot = {});
        /// Indexes new and changed files under the assets folder and forgets deleted ones; returns what changed
        RescanResult RescanAssets();

        /// What RefreshAsset found out about one file
        enum class RefreshResult
        {
            Missing,   ///< no such file, or not one the loader has a loader for (nothing changed)
            Added,     ///< no metadata before
            Modified,  ///< its size or modification time changed
            Unchanged
        };
        /**
         * RescanAssets for one project file (a res:// path): brings its metadata up to date, indexing it if it is
         * new. What a caller that has just written the file does, without scanning the whole folder. The cached copy
         * of a changed file is kept (Reload drops it). A path that isn't a res:// file with a loader, or a file that is gone
         * (a rescan forgets it), is Missing.
         */
        RefreshResult RefreshAsset(const ResourcePath &resourcePath);

        /**
         * Whether the files a scan looks at differ from the last scan's (a file added, removed, or with another size
         * or modification time), by walking the assets folder and reading only what the directory listing holds: no
         * file is opened, so it is cheap enough to ask every second. The editor's watcher asks, and calls
         * RescanAssets when it says yes. The comparison is a fingerprint taken before each scan reads anything, so a
         * file changed during a scan counts as changed afterwards. False before Initialize.
         */
        [[nodiscard]] bool AssetsChangedOnDisk() const;

        /// Where the last-seen size and modification time of every asset are kept (<project>/.n2/asset-state.json)
        [[nodiscard]] std::filesystem::path GetAssetStatePath() const;
        
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
        
        // === Import settings ===

        /**
         * Replaces an asset's import settings (the .meta's customData) with customData, which must be a JSON object
         * (null means empty), and saves the .meta. The index of a file's sub-assets (customData.subAssets and
         * subAssetsSource) belongs to the loader: it is kept as it is, and those two keys in customData are ignored.
         * The cached copy of the asset is not touched (it was made with the old settings); Reload it. An error
         * message when the path isn't an indexed project file, customData isn't an object, or the .meta can't be saved.
         */
        [[nodiscard]] std::expected<void, std::string> SetImportSettings(const ResourcePath &resourcePath,
                                                                         const nlohmann::json &customData);

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
        /// A file's recorded sub-assets (its .meta index, when it describes the file as it is now), sorted by key,
        /// with their UUIDs: empty for a file without sub-assets, one never loaded, or a path that isn't indexed
        [[nodiscard]] std::vector<SubAssetIndexEntry> GetSubAssets(const ResourcePath &parent) const;
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
        /// Where user:// paths resolve (see Initialize); empty before Initialize
        std::filesystem::path GetUserDataRoot() const { return _userDataRoot; }
        
    private:
        ResourceLoader() = default;
        
        /// Indexes every file with a loader under directory, recording in result what is new or changed, and in
        /// seen every such file found (even one whose metadata failed), so RescanAssets can tell what is gone
        void ScanDirectory(const std::filesystem::path& directory, RescanResult& result,
                           std::unordered_set<ResourcePath, ResourcePath::Hash>& seen);
        AssetMetadata CreateOrUpdateMetadata(const std::filesystem::path& sourcePath);
        std::filesystem::path GetMetadataPath(const std::filesystem::path& sourcePath) const;
        /// The files with a loader under the assets folder, as a fingerprint of (path, size, modification time): the
        /// sum of one hash per file, so the walk's order doesn't matter, and the file count
        struct DirectoryFingerprint
        {
            std::uint64_t sum = 0;
            std::uint64_t count = 0;
            bool operator==(const DirectoryFingerprint &) const = default;
        };
        [[nodiscard]] DirectoryFingerprint ComputeFingerprint() const;
        /// Reads .n2/asset-state.json into _assetState (a missing or unreadable file is no state)
        void LoadAssetState();
        /// Writes .n2/asset-state.json when _assetStateDirty
        void SaveAssetState();
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
        
        /// What each scanned file's size and modification time were when last seen, kept in .n2/asset-state.json (not in
        /// the .meta files, which would change with every edit of a file and are committed)
        struct AssetState
        {
            std::uint64_t lastModified = 0;
            std::size_t fileSize = 0;
        };
        std::unordered_map<ResourcePath, AssetState, ResourcePath::Hash> _assetState;
        bool _assetStateDirty = false;
        bool _assetStateWarned = false;
        DirectoryFingerprint _fingerprint;
        bool _hasFingerprint = false;

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