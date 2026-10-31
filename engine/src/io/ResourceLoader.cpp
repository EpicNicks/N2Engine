#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/Logger.hpp"
#include "engine/audio/AudioClip.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/prefabs/PrefabFile.hpp"
#include "engine/sceneManagement/SceneFile.hpp"
#include "engine/text/Font.hpp"
#include <assetimport/ModelImporters.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace N2Engine::IO
{
    namespace
    {
        /// A file's modification time in whole Unix milliseconds, converted exactly (clock_cast), so the same file time
        /// always gives the same number. The old conversion went through both clocks' now(), which differ by a
        /// moment on each call, so a file could look modified on one scan and not the next. Milliseconds, not seconds
        /// (as it was before the state file): two saves of a file a second apart, with the same size, are two changes.
        uint64_t UnixMilliseconds(const std::filesystem::file_time_type fileTime)
        {
#ifdef N2ENGINE_PLATFORM_WEB
            // libc++ (Emscripten) has no clock_cast; its file clock counts from the Unix epoch, so the same duration
            // since the epoch is the system clock's time
            const std::chrono::sys_time<std::filesystem::file_time_type::duration> systemTime(fileTime.time_since_epoch());
#else
            const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
#endif
            const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(systemTime).time_since_epoch().count();
            return milliseconds > 0 ? static_cast<uint64_t>(milliseconds) : 0u;
        }

        /// A file's extension, lower-cased, as the loader lookup spells it
        std::string LowerExtension(const std::filesystem::path &path)
        {
            std::string ext = PathToUtf8(path.extension());
            std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        /// splitmix64's finaliser: spreads a value over all 64 bits
        uint64_t Mix(uint64_t value)
        {
            value += 0x9E3779B97F4A7C15ull;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
            return value ^ (value >> 31);
        }

        /// The .meta resourceType for a file, from its extension, case-insensitively (as the loader lookup
        /// is): Music.WAV is an AudioClip like music.wav
        std::string ResourceTypeFor(const std::filesystem::path &sourcePath)
        {
            std::string ext = PathToUtf8(sourcePath.extension());
            std::ranges::transform(ext, ext.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".lua")
                return std::string(LuaScript::ResourceTypeName);
            if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
                return std::string(Audio::AudioClip::ResourceTypeName);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
                return std::string(Rendering::Texture::ResourceTypeName);
            if (ext == ".ttf" || ext == ".otf")
                return std::string(Text::Font::ResourceTypeName);
            if (ext == ".mat")
                return std::string(Rendering::Material::ResourceTypeName);
            if (AssetImport::FindModelImporter(ext) != nullptr) // .gltf, .glb, and .fbx, .obj with ufbx built
                return std::string(Rendering::Model::ResourceTypeName);
            if (ext == SceneFile::Extension)
                return std::string(SceneFile::ResourceTypeName);
            if (ext == PrefabFile::Extension)
                return std::string(PrefabFile::ResourceTypeName);
            return "Unknown";
        }
    }

    void ResourceLoader::Initialize(const std::filesystem::path& projectRoot, const std::filesystem::path& userDataRoot)
    {
        // Font.cpp's, Texture.cpp's, Material.cpp's, Model.cpp's, SceneFile.cpp's and PrefabFile.cpp's own registration only runs if
        // something calls it (or, for the static registrars, the linker keeps those files, which a program that
        // names none of the types wouldn't); registering here (idempotent) makes .ttf/.otf, the image extensions,
        // .mat, the model extensions (.gltf/.glb, and .fbx/.obj with ufbx built), .scene and .prefab scan and load anyway. Done before the roots change, so a first registration
        // doesn't rescan anything.
        Text::Font::RegisterLoader();
        Rendering::Texture::RegisterLoader();
        Rendering::Material::RegisterLoader();
        Rendering::Model::RegisterLoader();
        SceneFile::RegisterLoader();
        PrefabFile::RegisterLoader();

        _projectRoot = projectRoot;
        _assetsRoot = projectRoot / "assets";
        _metadataRoot = projectRoot / ".import";
        _userDataRoot = userDataRoot.empty() ? ProjectFile::UserDataBase() : userDataRoot;

        if (!_readOnly)
        {
            std::filesystem::create_directories(_metadataRoot);
        }
        std::filesystem::create_directories(_userDataRoot);

        Logger::Info(std::format("ResourceLoader initialized: {}", PathToUtf8(projectRoot)));

        // Everything here is keyed by res:// paths, which don't include the root: entries from a
        // previous root would otherwise pass for this one's files
        _metadata.clear();
        _uuidToPath.clear();
        _subAssets.clear();
        _subAssetParentsTried.clear();
        ClearCache();
        _assetState.clear();
        _assetStateDirty = false;
        _assetStateWarned = false;
        _fingerprint = {};
        _hasFingerprint = false;
        LoadAssetState();

        (void)RescanAssets();

        Logger::Info(std::format("Found {} assets", _metadata.size()));
    }

    ResourceLoader::RescanResult ResourceLoader::RescanAssets()
    {
        RescanResult result;
        if (!std::filesystem::exists(_assetsRoot))
        {
            Logger::Warn("Assets directory not found");
            _fingerprint = {};
            _hasFingerprint = true;
            return result;
        }

        // Taken before the scan reads anything: a file that changes while it runs differs from this, so the next
        // AssetsChangedOnDisk says so
        _fingerprint = ComputeFingerprint();
        _hasFingerprint = true;

        // Who held each UUID before this scan: when two files turn out to hold one (a file copied with its .meta), the
        // one that had it keeps it
        const std::unordered_map<Math::UUID, ResourcePath, UUIDHash> heldBefore = _uuidToPath;
        _mintedThisScan.clear();

        std::unordered_set<ResourcePath, ResourcePath::Hash> seen;
        try
        {
            ScanDirectory(_assetsRoot, result, seen);
        }
        catch (...)
        {
            // A scan cut short saw only some of the files, and took none of them for gone. Nothing now matches the
            // fingerprint, so the next AssetsChangedOnDisk says yes and the watcher scans again.
            _fingerprint = DirectoryFingerprint{~std::uint64_t{0}, ~std::uint64_t{0}};
            _mintedThisScan.clear();
            throw;
        }

        // Files indexed before and gone now: forget them, so they stop being listed and loaded by path or UUID. Only
        // project files (res://) are scanned, so only they can be found missing. Their .meta files stay: a file put
        // back (a branch switch, an undo) gets its import settings back.
        for (auto it = _metadata.begin(); it != _metadata.end();)
        {
            const ResourcePath &path = it->first;
            if (path.GetType() != PathType::Resource || seen.contains(path))
            {
                ++it;
                continue;
            }
            result.removed.push_back(path);
            const Math::UUID uuid = it->second.uuid;
            if (const auto byUUID = _uuidToPath.find(uuid); byUUID != _uuidToPath.end() && byUUID->second == path)
            {
                _uuidToPath.erase(byUUID);
            }
            // Whoever holds the asset keeps it; a later file at this path loads afresh
            if (const auto cached = _cache.find(path); cached != _cache.end())
            {
                if (const auto cachedByUUID = _cacheByUUID.find(uuid);
                    cachedByUUID != _cacheByUUID.end() && cachedByUUID->second == cached->second)
                {
                    _cacheByUUID.erase(cachedByUUID);
                }
                _cache.erase(cached);
            }
            _subAssetParentsTried.erase(path);
            if (_assetState.erase(path) > 0)
            {
                _assetStateDirty = true;
            }
            it = _metadata.erase(it);
        }
        // Only files that are still there can clash: a file moved outside the editor together with its .meta is gone
        // from its old path by now, so it keeps its UUID
        ResolveDuplicateUuids(heldBefore);
        _mintedThisScan.clear();

        // State of files that are gone (deleted before this session, or never indexed) is no use
        for (auto it = _assetState.begin(); it != _assetState.end();)
        {
            if (seen.contains(it->first))
            {
                ++it;
                continue;
            }
            it = _assetState.erase(it);
            _assetStateDirty = true;
        }
        SaveAssetState();

        // Sub-assets of files deleted since: unknown again, so lookups of them give null quietly rather than an
        // error about a missing source file
        std::erase_if(_subAssets, [this](const auto &item)
        {
            std::error_code existsError;
            const std::filesystem::path sourcePath = Resolve(item.second.parent);
            return sourcePath.empty() || !std::filesystem::exists(sourcePath, existsError);
        });

        std::sort(result.added.begin(), result.added.end());
        std::sort(result.removed.begin(), result.removed.end());
        std::sort(result.modified.begin(), result.modified.end());
        return result;
    }

    void ResourceLoader::ScanDirectory(const std::filesystem::path &directory, RescanResult &result,
                                       std::unordered_set<ResourcePath, ResourcePath::Hash> &seen)
    {
        // Error codes, not exceptions, and unreadable folders skipped (as ComputeFingerprint does). A walk that fails
        // part way throws below instead of returning: the files it didn't reach would otherwise look deleted.
        std::error_code walkError;
        std::filesystem::recursive_directory_iterator walk(
            directory, std::filesystem::directory_options::skip_permission_denied, walkError);
        const std::filesystem::recursive_directory_iterator walkEnd;
        for (; !walkError && walk != walkEnd; walk.increment(walkError))
        {
            const std::filesystem::directory_entry &entry = *walk;
            std::error_code entryError;
            if (!entry.is_regular_file(entryError) || entryError)
                continue;

            if (entry.path().extension() == ".meta")
                continue;

            // One file whose name can't be converted (or whose metadata fails) is skipped, never the rest of the scan
            try
            {
                std::string ext = PathToUtf8(entry.path().extension());
                std::ranges::transform(ext, ext.begin(), [](const unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });

                if (_loaders.find(ext) == _loaders.end())
                    continue;

                // Before the metadata: a file whose .meta can't be written still exists, so it isn't "removed"
                seen.insert(MakeResourcePath(entry.path()));

                AssetMetadata meta = CreateOrUpdateMetadata(entry.path());
                if (const auto previous = _metadata.find(meta.resourcePath); previous == _metadata.end())
                {
                    result.added.push_back(meta.resourcePath);
                }
                else if (previous->second.lastModified != meta.lastModified ||
                         previous->second.fileSize != meta.fileSize)
                {
                    result.modified.push_back(meta.resourcePath);
                }
                _metadata[meta.resourcePath] = meta;
                _uuidToPath[meta.uuid] = meta.resourcePath;
                // A model's sub-asset index, as its last full load wrote it (the scan never parses a model)
                ReadSubAssetIndex(meta);
            }
            catch (const std::exception &e)
            {
                // PathToUtf8 never throws, unlike string() for a name the code page can't spell
                Logger::Warn(std::format("Skipping asset {}: {}", PathToUtf8(entry.path()), e.what()));
            }
        }
        if (walkError)
        {
            throw std::filesystem::filesystem_error("can't scan the assets folder", directory, walkError);
        }
    }

    AssetMetadata ResourceLoader::CreateOrUpdateMetadata(const std::filesystem::path &sourcePath)
    {
        const uint64_t timestamp = UnixMilliseconds(std::filesystem::last_write_time(sourcePath));
        auto fileSize = std::filesystem::file_size(sourcePath);

        ResourcePath resourcePath = MakeResourcePath(sourcePath);
        std::filesystem::path metaPath = GetMetadataPath(sourcePath);

        AssetMetadata meta;

        bool haveMeta = false;
        bool metaFileFound = false;
        if (std::filesystem::exists(metaPath))
        {
            metaFileFound = true;
            try
            {
                meta = AssetMetadata::FromFile(metaPath);
                haveMeta = true;
            }
            catch (const std::exception &e)
            {
                // A corrupt .meta used to throw out of Initialize; regenerate it instead. The new UUID is minted from
                // the path: the same as before for a file that never moved, a new one for a file that did.
                Logger::Warn(std::format("Corrupt metadata {} ({}); regenerating: references to the UUID it held before "
                                         "will not resolve if the file was moved",
                                         PathToUtf8(metaPath), e.what()));
            }
        }

        // What the file was when last seen: from the state file (or this session's scans). A .meta still holding the
        // old lastModified and fileSize (seconds, not milliseconds) isn't used for it, only rewritten without them.
        std::optional<AssetState> lastSeen;
        if (const auto known = _assetState.find(resourcePath); known != _assetState.end())
        {
            lastSeen = known->second;
        }

        if (haveMeta)
        {
            // The .meta is written when something it keeps changed, never for the file's size or time
            bool saveMeta = meta.hadStateFields;
            meta.hadStateFields = false;

            // The .meta's UUID is the asset's identity, kept as it is: a moved file's differs from the hash of its path
            // (ResourceUUID::FromPath only mints a UUID). An all-zero one was written by an engine whose ResourceUUID
            // was not initialised (every asset got the same): it is minted again.
            if (meta.uuid == Math::UUID::ZERO)
            {
                if (const Math::UUID minted = MintUuid(resourcePath); minted != Math::UUID::ZERO)
                {
                    Logger::Warn(std::format("{}: its .meta held no UUID (all zeros); gave it {}",
                                             resourcePath.ToString(), minted.ToString()));
                    meta.uuid = minted;
                    _mintedThisScan.insert(resourcePath);
                    saveMeta = true;
                }
            }

            // The file decides where it is: a .meta moved or copied here with its file (outside the editor, by git or
            // by hand) still names the old path. Its sub-asset UUIDs were hashed from that path, so it becomes the
            // seed, unless the .meta already has one.
            if (meta.resourcePath != resourcePath)
            {
                Logger::Info(std::format("{}: its .meta named {}; the file's own path is used",
                                         resourcePath.ToString(), meta.resourcePath.ToString()));
                if (meta.uuidSeed.empty() && meta.resourcePath.GetType() == PathType::Resource &&
                    meta.resourcePath.IsValid())
                {
                    meta.uuidSeed = meta.resourcePath.ToString();
                }
                meta.resourcePath = resourcePath;
                if (ResourcePath(meta.uuidSeed) == resourcePath)
                {
                    meta.uuidSeed.clear(); // moved back where its sub-asset UUIDs came from
                }
                saveMeta = true;
            }

            // A .meta written before its extension was known (or matched case-insensitively, as Music.WAV
            // now is) said "Unknown"; give it its type now
            if (meta.resourceType == "Unknown")
            {
                if (std::string type = ResourceTypeFor(sourcePath); type != "Unknown")
                {
                    meta.resourceType = std::move(type);
                    saveMeta = true;
                }
            }

            if (lastSeen && (lastSeen->lastModified != timestamp || lastSeen->fileSize != fileSize))
            {
                Logger::Info(std::format("Asset modified: {}", resourcePath.ToString()));
            }
            meta.lastModified = timestamp;
            meta.fileSize = fileSize;
            if (saveMeta)
            {
                PersistMeta(meta, metaPath);
            }
        }
        else
        {
            // Minted from the path the file has now, then stored: the same UUID every earlier engine gave it, unless
            // an asset moved away from this path still holds that one (then "path#2", ...)
            meta.uuid = MintUuid(resourcePath);
            _mintedThisScan.insert(resourcePath);
            meta.resourcePath = resourcePath;
            meta.lastModified = timestamp;
            meta.fileSize = fileSize;

            meta.resourceType = ResourceTypeFor(sourcePath);
            PersistMeta(meta, metaPath);
            if (lastSeen && !metaFileFound && !_readOnly)
            {
                // Seen by an earlier scan (.n2/asset-state.json), so it had a .meta: deleted, or never committed (a
                // read-only loader writes none, so it can't tell)
                Logger::Warn(std::format("{} lost its .meta; it gets the UUID of its path again, and references to a "
                                         "UUID it held before a move will not resolve",
                                         resourcePath.ToString()));
            }
            else
            {
                Logger::Info(std::format("New asset: {}", resourcePath.ToString()));
            }
        }

        if (!lastSeen || lastSeen->lastModified != timestamp || lastSeen->fileSize != fileSize)
        {
            _assetState[resourcePath] = AssetState{timestamp, static_cast<std::size_t>(fileSize)};
            _assetStateDirty = true;
        }

        return meta;
    }

    std::filesystem::path ResourceLoader::GetMetadataPath(const std::filesystem::path &sourcePath) const
    {
        auto relative = std::filesystem::relative(sourcePath, _assetsRoot);
        // Appended as a path, not a narrow string: a file name the code page can't spell keeps its .meta beside it
        std::filesystem::path metaName = relative.filename();
        metaName += ".meta";
        return _metadataRoot / relative.parent_path() / metaName;
    }

    namespace
    {
        // Joins a relative resource path onto its root and rejects anything that normalizes to
        // outside it (e.g. res://../../secrets), which used to resolve to wherever ".." led
        std::filesystem::path ResolveUnder(const std::filesystem::path &root, const std::string &relative)
        {
            const std::filesystem::path normalizedRoot = root.lexically_normal();
            const std::filesystem::path resolved = (root / PathFromUtf8(relative)).lexically_normal();
            const std::filesystem::path fromRoot = resolved.lexically_relative(normalizedRoot);
            if (fromRoot.empty() || *fromRoot.begin() == "..")
            {
                Logger::Warn(std::format("Resource path escapes its root and was rejected: {}", relative));
                return {};
            }
            return resolved;
        }
    }

    std::filesystem::path ResourceLoader::Resolve(const ResourcePath &resourcePath) const
    {
        switch (resourcePath.GetType())
        {
        case PathType::Resource:
            return ResolveUnder(_assetsRoot, resourcePath.GetPath());
        case PathType::User:
            return ResolveUnder(_userDataRoot, resourcePath.GetPath());
        case PathType::Absolute:
            return std::filesystem::path(resourcePath.GetPath());
        case PathType::Invalid:
            return {};
        }
        return {};
    }

    ResourcePath ResourceLoader::MakeResourcePath(const std::filesystem::path &physicalPath) const
    {
        auto relative = std::filesystem::relative(physicalPath, _assetsRoot);
        // UTF-8, as every res:// path is (the .meta keys, the editor's wire paths)
        if (!PathToUtf8(relative).starts_with(".."))
        {
            return ResourcePath(PathType::Resource, PathToUtf8(relative));
        }

        relative = std::filesystem::relative(physicalPath, _userDataRoot);
        if (!PathToUtf8(relative).starts_with(".."))
        {
            return ResourcePath(PathType::User, PathToUtf8(relative));
        }

        return ResourcePath(PathType::Absolute, PathToUtf8(physicalPath));
    }

    const AssetMetadata* ResourceLoader::GetMetadata(const ResourcePath &resourcePath) const
    {
        auto it = _metadata.find(resourcePath);
        return it != _metadata.end() ? &it->second : nullptr;
    }

    const AssetMetadata* ResourceLoader::GetMetadata(const Math::UUID &uuid) const
    {
        auto pathIt = _uuidToPath.find(uuid);
        if (pathIt != _uuidToPath.end())
        {
            return GetMetadata(pathIt->second);
        }
        return nullptr;
    }

    Math::UUID ResourceLoader::GetUUID(const ResourcePath &resourcePath) const
    {
        auto meta = GetMetadata(resourcePath);
        return meta ? meta->uuid : Math::UUID::ZERO;
    }

    Math::UUID ResourceLoader::GetOrMintUUID(const ResourcePath &resourcePath) const
    {
        if (const AssetMetadata *meta = GetMetadata(resourcePath))
        {
            return meta->uuid;
        }
        return MintUuid(resourcePath);
    }

    bool ResourceLoader::Exists(const ResourcePath &resourcePath) const
    {
        return _metadata.find(resourcePath) != _metadata.end();
    }

    bool ResourceLoader::HasSourceChanged(const ResourcePath &resourcePath) const
    {
        auto meta = GetMetadata(resourcePath);
        if (!meta)
            return false;

        auto sourcePath = Resolve(resourcePath);
        if (!std::filesystem::exists(sourcePath))
            return false;

        const uint64_t timestamp = UnixMilliseconds(std::filesystem::last_write_time(sourcePath));

        return timestamp != meta->lastModified;
    }

    bool ResourceLoader::Reload(const ResourcePath &resourcePath)
    {
        _cache.erase(resourcePath);

        auto meta = GetMetadata(resourcePath);
        if (meta)
        {
            _cacheByUUID.erase(meta->uuid);
        }

        auto sourcePath = Resolve(resourcePath);
        if (sourcePath.empty() || !std::filesystem::exists(sourcePath))
        {
            return false;
        }

        // Keep the refreshed metadata: discarding it left HasSourceChanged comparing against the old
        // timestamp, so it reported the asset as changed forever
        IndexOne(CreateOrUpdateMetadata(sourcePath));

        Logger::Info(std::format("Reloaded: {}", resourcePath.ToString()));
        return true;
    }

    void ResourceLoader::ClearCache()
    {
        _cache.clear();
        _cacheByUUID.clear();
    }

    void ResourceLoader::RemoveUnused()
    {
        for (auto it = _cache.begin(); it != _cache.end();)
        {
            // Both caches hold a reference, so "unused" means no references beyond those two
            // (this used to require use_count() <= 1, which never happened)
            const auto meta = GetMetadata(it->first);
            const bool inUUIDCache = meta && _cacheByUUID.contains(meta->uuid) && _cacheByUUID.at(meta->uuid) == it->second;
            // A model whose meshes or materials are in use stays: evicting it would make the next load of one of its
            // sub-assets import the file again, giving second objects (and GPU copies) with the same UUIDs
            if (it->second.use_count() <= (inUUIDCache ? 2 : 1) && !it->second->AreSubAssetsInUse())
            {
                auto meta = GetMetadata(it->first);
                if (meta)
                {
                    _cacheByUUID.erase(meta->uuid);
                }
                it = _cache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::vector<AssetMetadata> ResourceLoader::GetAllAssets() const
    {
        std::vector<AssetMetadata> result;
        result.reserve(_metadata.size());

        for (const auto &[path, meta] : _metadata)
        {
            result.push_back(meta);
        }

        return result;
    }

    std::vector<AssetMetadata> ResourceLoader::GetAssetsByType(const std::string &type) const
    {
        std::vector<AssetMetadata> result;

        for (const auto &[path, meta] : _metadata)
        {
            if (meta.resourceType == type)
            {
                result.push_back(meta);
            }
        }

        return result;
    }

    void ResourceLoader::RegisterLoader(const std::string &extension, LoaderFunc loader)
    {
        const bool newExtension = !_loaders.contains(extension);
        _loaders[extension] = std::move(loader);

        // The scan skips extensions without a loader, so files of one registered after Initialize would
        // never get metadata (and would fall back to random UUIDs); index them now
        if (newExtension && !_assetsRoot.empty())
        {
            RescanAssets();
        }
    }

    // ===== Sub-assets =====

    void ResourceLoader::RegisterSubAssetParentType(const std::string &resourceType)
    {
        _subAssetParentTypes.insert(resourceType);
    }

    const ResourceLoader::SubAssetLocation *ResourceLoader::FindSubAssetLocation(const Math::UUID &uuid) const
    {
        const auto it = _subAssets.find(uuid);
        return it != _subAssets.end() ? &it->second : nullptr;
    }

    bool ResourceLoader::SetSubAssetIndex(const ResourcePath &parent, const std::vector<SubAssetIndexEntry> &entries)
    {
        const auto metaIt = _metadata.find(parent);
        if (metaIt == _metadata.end())
        {
            return false;
        }

        std::erase_if(_subAssets, [&parent](const auto &item) { return item.second.parent == parent; });
        nlohmann::json index = nlohmann::json::object();
        for (const SubAssetIndexEntry &entry : entries)
        {
            index[entry.key] = {{"type", entry.type}, {"uuid", entry.uuid.ToString()}};
            _subAssets[entry.uuid] = SubAssetLocation{parent, entry.key};
        }

        AssetMetadata &meta = metaIt->second;
        const std::filesystem::path sourcePath = Resolve(parent);
        const std::filesystem::path metaPath = parent.GetType() == PathType::Resource && !sourcePath.empty()
                                                   ? GetMetadataPath(sourcePath)
                                                   : std::filesystem::path{};
        // customData as the file has it now: it may have been edited (import settings) since the scan read it
        std::error_code existsError;
        if (!metaPath.empty() && std::filesystem::exists(metaPath, existsError))
        {
            try
            {
                meta.customData = AssetMetadata::FromFile(metaPath).customData;
            }
            catch (const std::exception &)
            {
                // A corrupt .meta: keep the scan's copy, which the save below rewrites it with
            }
        }
        if (meta.customData.is_null())
        {
            meta.customData = nlohmann::json::object();
        }
        if (!meta.customData.is_object())
        {
            Logger::Warn(std::format("{}: the .meta customData isn't an object, so its sub-asset index isn't saved",
                                     parent.ToString()));
            return true;
        }
        // The source file the index describes; a scan that finds the file changed treats the index as stale
        const nlohmann::json source = {{"fileSize", meta.fileSize}, {"lastModified", meta.lastModified}};
        const auto existing = meta.customData.find("subAssets");
        const auto existingSource = meta.customData.find("subAssetsSource");
        if (existing != meta.customData.end() && *existing == index && existingSource != meta.customData.end() &&
            *existingSource == source)
        {
            return true; // unchanged: the .meta isn't rewritten
        }
        meta.customData["subAssets"] = std::move(index);
        meta.customData["subAssetsSource"] = source;

        if (!metaPath.empty())
        {
            bool saved = false;
            try
            {
                saved = PersistMeta(meta, metaPath);
            }
            catch (const std::exception &e)
            {
                Logger::Warn(std::format("{}: can't save the sub-asset index: {}", parent.ToString(), e.what()));
            }
            if (!saved)
            {
                Logger::Warn(std::format("{}: the sub-asset index wasn't saved to {}; it is rebuilt the next time the "
                                         "model loads", parent.ToString(), PathToUtf8(metaPath)));
            }
        }
        return true;
    }

    void ResourceLoader::RegisterSubAssets(const ResourcePath &parent, const Base::Asset &asset)
    {
        std::vector<Base::SubAssetRef> subAssets = asset.GetSubAssets();
        if (subAssets.empty() && !_subAssetParentTypes.contains(asset.GetResourceType()))
        {
            return; // not a file with sub-assets
        }
        std::vector<SubAssetIndexEntry> entries;
        entries.reserve(subAssets.size());
        // Hashed from the .meta's seed: the path the file had before it was first moved, so its parts keep their UUIDs
        const auto metaIt = _metadata.find(parent);
        const ResourcePath seed = metaIt != _metadata.end() ? SubAssetSeed(metaIt->second) : parent;
        for (Base::SubAssetRef &sub : subAssets)
        {
            if (!sub.asset)
            {
                continue;
            }
            const Math::UUID uuid = ResourceUUID::FromSubAsset(seed, sub.key);
            sub.asset->SetUUID(uuid);
            sub.asset->SetResourcePath(parent);
            sub.asset->MarkAsSubAsset(sub.key);
            entries.push_back(SubAssetIndexEntry{sub.key, sub.asset->GetResourceType(), uuid});
        }
        SetSubAssetIndex(parent, entries);
    }

    bool ResourceLoader::HasCurrentSubAssetIndex(const AssetMetadata &meta)
    {
        if (!meta.customData.is_object())
        {
            return false;
        }
        const auto index = meta.customData.find("subAssets");
        const auto source = meta.customData.find("subAssetsSource");
        if (index == meta.customData.end() || !index->is_object() || source == meta.customData.end() ||
            !source->is_object())
        {
            return false;
        }
        const auto size = source->find("fileSize");
        const auto time = source->find("lastModified");
        return size != source->end() && time != source->end() && size->is_number_unsigned() &&
               time->is_number_unsigned() && size->get<std::uint64_t>() == meta.fileSize &&
               time->get<std::uint64_t>() == meta.lastModified;
    }

    void ResourceLoader::ReadSubAssetIndex(const AssetMetadata &meta)
    {
        if (!meta.customData.is_object() || !meta.customData.contains("subAssets"))
        {
            return; // never loaded: nothing indexed
        }
        if (!HasCurrentSubAssetIndex(meta))
        {
            // The file changed since its index was written (or the index has no record of the file): forget the
            // old entries and the old model, so the next lookup loads the new file and indexes it again
            const ResourcePath &parent = meta.resourcePath;
            std::erase_if(_subAssets, [&parent](const auto &item) { return item.second.parent == parent; });
            if (_cache.erase(parent) > 0)
            {
                _cacheByUUID.erase(meta.uuid);
            }
            _subAssetParentsTried.erase(parent);
            return;
        }
        for (const auto &item : meta.customData.at("subAssets").items())
        {
            // Re-derived from the seed, not trusted from the file (a hand-edited index could name any UUID)
            _subAssets[ResourceUUID::FromSubAsset(SubAssetSeed(meta), item.key())] =
                SubAssetLocation{meta.resourcePath, item.key()};
        }
    }

    std::shared_ptr<Base::Asset> ResourceLoader::LoadSubAsset(const Math::UUID &uuid)
    {
        const SubAssetLocation *found = FindSubAssetLocation(uuid);
        if (!found)
        {
            if (!LoadUnindexedSubAssetParents())
            {
                return nullptr;
            }
            found = FindSubAssetLocation(uuid);
            if (!found)
            {
                return nullptr;
            }
        }
        // A copy: loading the parent rewrites the index (and so this map)
        const SubAssetLocation location = *found;
        const std::shared_ptr<Base::Asset> parent = Load<Base::Asset>(location.parent);
        if (!parent)
        {
            return nullptr;
        }
        return parent->FindSubAsset(location.key);
    }

    bool ResourceLoader::LoadUnindexedSubAssetParents()
    {
        std::vector<ResourcePath> candidates;
        for (const auto &[path, meta] : _metadata)
        {
            if (!_subAssetParentTypes.contains(meta.resourceType) || _cache.contains(path) ||
                _subAssetParentsTried.contains(path))
            {
                continue;
            }
            std::error_code existsError;
            const std::filesystem::path sourcePath = Resolve(path);
            if (!HasCurrentSubAssetIndex(meta) && !sourcePath.empty() &&
                std::filesystem::exists(sourcePath, existsError))
            {
                if (!_loaders.contains(LowerExtension(sourcePath)))
                {
                    // A model whose format this build can't read (an .fbx or .obj .meta from a build with ufbx, in one
                    // without it): skipped, said once, instead of attempting a load that can only fail
                    _subAssetParentsTried.insert(path);
                    Logger::Warn(std::format("Skipping model '{}': this build has no importer for its format "
                                             "(FBX and OBJ need N2ENGINE_MODEL_UFBX)", path.ToString()));
                    continue;
                }
                candidates.push_back(path);
            }
        }
        bool loadedAny = false;
        for (const ResourcePath &path : candidates)
        {
            _subAssetParentsTried.insert(path);
            loadedAny = Load<Base::Asset>(path) != nullptr || loadedAny;
        }
        return loadedAny;
    }

    // ===== Single-file refresh, change detection, import settings =====

    ResourceLoader::RefreshResult ResourceLoader::RefreshAsset(const ResourcePath &resourcePath)
    {
        if (resourcePath.GetType() != PathType::Resource || _assetsRoot.empty())
        {
            return RefreshResult::Missing;
        }
        const std::filesystem::path sourcePath = Resolve(resourcePath);
        std::error_code error;
        if (sourcePath.empty() || !std::filesystem::is_regular_file(sourcePath, error) ||
            sourcePath.extension() == ".meta" || !_loaders.contains(LowerExtension(sourcePath)))
        {
            return RefreshResult::Missing;
        }

        const auto previous = _metadata.find(resourcePath);
        const bool wasIndexed = previous != _metadata.end();
        const uint64_t previousModified = wasIndexed ? previous->second.lastModified : 0;
        const std::size_t previousSize = wasIndexed ? previous->second.fileSize : 0;

        const AssetMetadata fresh = CreateOrUpdateMetadata(sourcePath);
        IndexOne(fresh);
        // As the scan does: a model changed since its index was written loses the index's entries
        ReadSubAssetIndex(_metadata.at(fresh.resourcePath));
        SaveAssetState();

        if (!wasIndexed)
        {
            return RefreshResult::Added;
        }
        return previousModified != fresh.lastModified || previousSize != fresh.fileSize ? RefreshResult::Modified
                                                                                        : RefreshResult::Unchanged;
    }

    std::filesystem::path ResourceLoader::GetAssetStatePath() const
    {
        return _projectRoot / ".n2" / "asset-state.json";
    }

    ResourceLoader::DirectoryFingerprint ResourceLoader::ComputeFingerprint() const
    {
        DirectoryFingerprint fingerprint;
        std::error_code error;
        if (_assetsRoot.empty() || !std::filesystem::exists(_assetsRoot, error))
        {
            return fingerprint;
        }

        // One walk of the directory listing: a directory_entry holds the size and time the listing gave (on Windows the
        // same find-data call), so no file is opened or even stat'ed one by one
        std::filesystem::recursive_directory_iterator iterator(
            _assetsRoot, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; !error && iterator != end; iterator.increment(error))
        {
            const std::filesystem::directory_entry &entry = *iterator;
            std::error_code entryError;
            if (!entry.is_regular_file(entryError) || entryError)
            {
                continue;
            }
            const std::filesystem::path &path = entry.path();
            if (path.extension() == ".meta" || !_loaders.contains(LowerExtension(path)))
            {
                continue;
            }
            const std::uintmax_t size = entry.file_size(entryError);
            if (entryError)
            {
                continue;
            }
            const auto time = entry.last_write_time(entryError);
            if (entryError)
            {
                continue;
            }
            uint64_t hash = std::hash<std::string>{}(PathToUtf8(path));
            hash = Mix(hash ^ Mix(static_cast<uint64_t>(size)));
            hash = Mix(hash ^ Mix(UnixMilliseconds(time)));
            fingerprint.sum += hash;
            ++fingerprint.count;
        }
        return fingerprint;
    }

    bool ResourceLoader::AssetsChangedOnDisk() const
    {
        if (!_hasFingerprint)
        {
            return false;
        }
        return ComputeFingerprint() != _fingerprint;
    }

    void ResourceLoader::LoadAssetState()
    {
        std::ifstream file(GetAssetStatePath(), std::ios::binary);
        if (!file)
        {
            return; // none yet: every file is new to it
        }
        const nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
        if (!state.is_object() || !state.contains("assets") || !state.at("assets").is_object())
        {
            Logger::Warn(std::format("{} can't be read; it is rebuilt", PathToUtf8(GetAssetStatePath())));
            return;
        }
        for (const auto &item : state.at("assets").items())
        {
            const ResourcePath path(item.key());
            const nlohmann::json &entry = item.value();
            if (path.GetType() != PathType::Resource || !entry.is_object() || !entry.contains("lastModified") ||
                !entry.contains("fileSize") || !entry.at("lastModified").is_number_unsigned() ||
                !entry.at("fileSize").is_number_unsigned())
            {
                continue;
            }
            _assetState[path] = AssetState{entry.at("lastModified").get<std::uint64_t>(),
                                           entry.at("fileSize").get<std::size_t>()};
        }
    }

    bool ResourceLoader::PersistMeta(const AssetMetadata &meta, const std::filesystem::path &path) const
    {
        return _readOnly || meta.SaveToFile(path);
    }

    void ResourceLoader::SaveAssetState()
    {
        if (!_assetStateDirty || _projectRoot.empty() || _readOnly)
        {
            return;
        }
        nlohmann::json assets = nlohmann::json::object();
        for (const auto &[path, state] : _assetState)
        {
            assets[path.ToString()] = {{"lastModified", state.lastModified}, {"fileSize", state.fileSize}};
        }
        const nlohmann::json document = {{"formatVersion", 1}, {"assets", std::move(assets)}};

        std::error_code error;
        std::filesystem::create_directories(GetAssetStatePath().parent_path(), error);
        std::expected<void, std::string> written = std::unexpected(error ? error.message() : std::string{});
        if (!error)
        {
            written = WriteTextFileAtomically(GetAssetStatePath(), document.dump(2) + "\n");
        }
        if (written)
        {
            _assetStateDirty = false;
        }
        else if (!_assetStateWarned)
        {
            // Once: a read-only project folder would otherwise say so on every scan. Nothing depends on the file
            // (it only says what was last seen), so the scan goes on.
            _assetStateWarned = true;
            Logger::Warn(std::format("Can't save {}: {}", PathToUtf8(GetAssetStatePath()), written.error()));
        }
    }

    std::expected<void, std::string> ResourceLoader::SetImportSettings(const ResourcePath &resourcePath,
                                                                       const nlohmann::json &customData)
    {
        if (resourcePath.GetType() != PathType::Resource)
        {
            return std::unexpected("import settings belong to project (res://) files");
        }
        const auto found = _metadata.find(resourcePath);
        if (found == _metadata.end())
        {
            return std::unexpected("not an asset: " + resourcePath.ToString());
        }
        if (!customData.is_object() && !customData.is_null())
        {
            return std::unexpected("import settings must be a JSON object");
        }

        AssetMetadata &meta = found->second;
        nlohmann::json settings = customData.is_object() ? customData : nlohmann::json::object();
        // The sub-asset index is the loader's own, not an import setting
        for (const char *key : {"subAssets", "subAssetsSource"})
        {
            settings.erase(key);
            if (meta.customData.is_object() && meta.customData.contains(key))
            {
                settings[key] = meta.customData.at(key);
            }
        }

        const std::filesystem::path sourcePath = Resolve(resourcePath);
        if (sourcePath.empty())
        {
            return std::unexpected("can't resolve " + resourcePath.ToString());
        }
        if (_readOnly)
        {
            return std::unexpected("the project is open read-only: " + resourcePath.ToString() + " was not changed");
        }
        AssetMetadata updated = meta;
        updated.customData = settings;
        try
        {
            if (!updated.SaveToFile(GetMetadataPath(sourcePath)))
            {
                return std::unexpected("can't save the .meta of " + resourcePath.ToString());
            }
        }
        catch (const std::exception &e)
        {
            return std::unexpected(std::format("can't save the .meta of {}: {}", resourcePath.ToString(), e.what()));
        }
        meta.customData = std::move(settings);
        return {};
    }

    std::vector<ResourceLoader::SubAssetIndexEntry> ResourceLoader::GetSubAssets(const ResourcePath &parent) const
    {
        std::vector<SubAssetIndexEntry> entries;
        const auto found = _metadata.find(parent);
        if (found == _metadata.end() || !HasCurrentSubAssetIndex(found->second))
        {
            return entries;
        }
        for (const auto &item : found->second.customData.at("subAssets").items())
        {
            std::string type = item.value().is_object() && item.value().contains("type") &&
                                       item.value().at("type").is_string()
                                   ? item.value().at("type").get<std::string>()
                                   : std::string{};
            // Re-derived from the seed, as the scan does
            entries.push_back(SubAssetIndexEntry{item.key(), std::move(type),
                                                 ResourceUUID::FromSubAsset(SubAssetSeed(found->second), item.key())});
        }
        return entries;
    }

    // ===== Stable UUIDs: minting, clashes and moves =====

    namespace
    {
        /// ASCII letters folded, for an order of paths that is the same on every file system
        std::string FoldAscii(std::string text)
        {
            std::ranges::transform(text, text.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        /// A file's bytes, or nothing when it can't be read
        std::optional<std::string> ReadWholeFile(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        /// Renames `from` to `to`, two spellings of one entry on a case-insensitive file system, through a temporary
        /// name beside it (a direct rename may be refused as a rename onto itself). False when either step failed.
        bool RenameCaseOnly(const std::filesystem::path &from, const std::filesystem::path &to)
        {
            std::filesystem::path temporary = from;
            temporary += ".n2move-tmp";
            std::error_code error;
            std::filesystem::rename(from, temporary, error);
            if (error)
            {
                return false;
            }
            std::filesystem::rename(temporary, to, error);
            if (error)
            {
                std::error_code ignored;
                std::filesystem::rename(temporary, from, ignored);
                return false;
            }
            return true;
        }
    }

    std::filesystem::path ResourceLoader::MetaPathOf(const ResourcePath &resourcePath) const
    {
        std::filesystem::path path = _metadataRoot / PathFromUtf8(resourcePath.GetPath());
        path += ".meta";
        return path;
    }

    ResourcePath ResourceLoader::SubAssetSeed(const AssetMetadata &meta)
    {
        if (!meta.uuidSeed.empty())
        {
            if (ResourcePath seed(meta.uuidSeed); seed.GetType() == PathType::Resource && seed.IsValid())
            {
                return seed;
            }
        }
        return meta.resourcePath;
    }

    bool ResourceLoader::UuidHeldByOther(const Math::UUID &uuid, const ResourcePath &self) const
    {
        const auto holder = _uuidToPath.find(uuid);
        if (holder == _uuidToPath.end() || holder->second == self)
        {
            return false;
        }
        const auto meta = _metadata.find(holder->second);
        return meta != _metadata.end() && meta->second.uuid == uuid;
    }

    Math::UUID ResourceLoader::MintUuid(const ResourcePath &path) const
    {
        for (unsigned variant = 1; variant <= 1000; ++variant)
        {
            const Math::UUID uuid = ResourceUUID::FromPath(path, variant);
            if (uuid == Math::UUID::ZERO || !UuidHeldByOther(uuid, path))
            {
                return uuid; // ZERO: ResourceUUID isn't initialised, and every asset gets ZERO as before
            }
        }
        return Math::UUID::Random(); // a thousand moved assets came from this one path
    }

    void ResourceLoader::IndexOne(const AssetMetadata &meta)
    {
        const ResourcePath path = meta.resourcePath;
        if (const auto previous = _metadata.find(path); previous != _metadata.end() && previous->second.uuid != meta.uuid)
        {
            if (const auto byUUID = _uuidToPath.find(previous->second.uuid);
                byUUID != _uuidToPath.end() && byUUID->second == path)
            {
                _uuidToPath.erase(byUUID);
            }
        }
        _metadata[path] = meta;
        if (meta.uuid != Math::UUID::ZERO && UuidHeldByOther(meta.uuid, path))
        {
            // The file indexed before keeps the UUID: this one is the copy
            const ResourcePath keeper = _uuidToPath.at(meta.uuid);
            RegenerateUuid(path, keeper);
            return;
        }
        _uuidToPath[meta.uuid] = path;
    }

    void ResourceLoader::RegenerateUuid(const ResourcePath &path, const ResourcePath &keeper)
    {
        const auto found = _metadata.find(path);
        if (found == _metadata.end())
        {
            return;
        }
        AssetMetadata &meta = found->second;
        const Math::UUID previous = meta.uuid;
        meta.uuid = MintUuid(path);
        // Its parts are its own now too: hashed from its own path
        meta.uuidSeed.clear();

        // Whatever was cached or indexed of this file under the keeper's UUIDs goes
        if (const auto cached = _cache.find(path); cached != _cache.end())
        {
            if (const auto byUUID = _cacheByUUID.find(previous);
                byUUID != _cacheByUUID.end() && byUUID->second == cached->second)
            {
                _cacheByUUID.erase(byUUID);
            }
            _cache.erase(cached);
        }
        std::erase_if(_subAssets, [&path](const auto &item) { return item.second.parent == path; });
        _subAssetParentsTried.erase(path);
        _uuidToPath[meta.uuid] = path;

        if (const std::filesystem::path sourcePath = Resolve(path);
            path.GetType() == PathType::Resource && !sourcePath.empty())
        {
            try
            {
                PersistMeta(meta, GetMetadataPath(sourcePath));
            }
            catch (const std::exception &e)
            {
                Logger::Warn(std::format("Can't save the .meta of {}: {}", path.ToString(), e.what()));
            }
        }
        ReadSubAssetIndex(meta);

        if (_mintedThisScan.contains(path))
        {
            // A new file at a path a moved asset came from: that asset still holds the path's UUID
            Logger::Info(std::format("{}: {} holds the UUID of this path, so it gets {}", path.ToString(),
                                     keeper.ToString(), meta.uuid.ToString()));
        }
        else
        {
            Logger::Warn(std::format("{} had the same UUID as {} (a file copied with its .meta?): it gets a new one, {}; "
                                     "references to {} go to {}",
                                     path.ToString(), keeper.ToString(), meta.uuid.ToString(), previous.ToString(),
                                     keeper.ToString()));
        }
    }

    void ResourceLoader::ResolveDuplicateUuids(const std::unordered_map<Math::UUID, ResourcePath, UUIDHash> &heldBefore)
    {
        // Rebuilt from the index: the scan's map kept whichever file it saw last
        _uuidToPath.clear();
        std::unordered_map<Math::UUID, std::vector<ResourcePath>, UUIDHash> holders;
        for (const auto &[path, meta] : _metadata)
        {
            _uuidToPath[meta.uuid] = path;
            if (meta.uuid != Math::UUID::ZERO)
            {
                holders[meta.uuid].push_back(path);
            }
        }

        for (auto &[uuid, paths] : holders)
        {
            if (paths.size() < 2)
            {
                continue;
            }
            // Never the directory order, which differs between file systems: two machines must agree
            std::ranges::sort(paths, [](const ResourcePath &a, const ResourcePath &b)
            {
                const std::string foldedA = FoldAscii(a.ToString());
                const std::string foldedB = FoldAscii(b.ToString());
                return foldedA != foldedB ? foldedA < foldedB : a < b;
            });
            ResourcePath keeper = paths.front();
            if (const auto before = heldBefore.find(uuid);
                before != heldBefore.end() && std::ranges::find(paths, before->second) != paths.end())
            {
                keeper = before->second;
            }
            else if (const auto stored = std::ranges::find_if(paths, [this](const ResourcePath &path)
                                                              { return !_mintedThisScan.contains(path); });
                     stored != paths.end())
            {
                keeper = *stored;
            }
            _uuidToPath[uuid] = keeper;
            for (const ResourcePath &path : paths)
            {
                if (path != keeper)
                {
                    RegenerateUuid(path, keeper);
                }
            }
            // The copies' indexes, read by the scan, named the keeper's parts: read the keeper's again
            ReadSubAssetIndex(_metadata.at(keeper));
        }
    }

    std::vector<ResourcePath> ResourceLoader::AssetsUnder(const ResourcePath &prefix) const
    {
        std::vector<ResourcePath> found;
        if (prefix.GetType() != PathType::Resource || !prefix.IsValid())
        {
            return found;
        }
        const std::string folder = prefix.GetPath() + "/";
        for (const auto &[path, meta] : _metadata)
        {
            if (path.GetType() == PathType::Resource && (path == prefix || path.GetPath().starts_with(folder)))
            {
                found.push_back(path);
            }
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    std::expected<std::vector<ResourceLoader::MovedAsset>, std::string> ResourceLoader::MovePath(
        const ResourcePath &from, const ResourcePath &to,
        const std::function<std::expected<void, std::string>()> &renameOnDisk)
    {
        if (_readOnly)
        {
            return std::unexpected("the project is open read-only: nothing was moved");
        }
        if (from.GetType() != PathType::Resource || to.GetType() != PathType::Resource || !from.IsValid() ||
            !to.IsValid() || _assetsRoot.empty())
        {
            return std::unexpected("only project (res://) files and folders can be moved");
        }
        if (from == to)
        {
            return std::unexpected(from.ToString() + " is already there");
        }

        const std::size_t fromLength = from.GetPath().size();
        const auto rebase = [&](const ResourcePath &path)
        {
            return path == from ? to : ResourcePath(PathType::Resource, to.GetPath() + path.GetPath().substr(fromLength));
        };

        struct Step
        {
            ResourcePath oldPath;
            ResourcePath newPath;
            AssetMetadata moved;
            std::filesystem::path oldMeta;
            std::filesystem::path newMeta;
            std::optional<std::string> oldText;  ///< the old .meta as it was
            std::optional<std::string> replaced; ///< a stale .meta that was at the destination
            bool sameFile = false;               ///< a case-only rename: the two .meta paths are one file
        };
        std::vector<Step> steps;
        for (const ResourcePath &path : AssetsUnder(from))
        {
            Step step;
            step.oldPath = path;
            step.newPath = rebase(path);
            if (_metadata.contains(step.newPath))
            {
                return std::unexpected(step.newPath.ToString() + " is already an asset");
            }
            step.moved = _metadata.at(path);
            step.moved.resourcePath = step.newPath;
            step.oldMeta = MetaPathOf(path);
            step.newMeta = MetaPathOf(step.newPath);
            steps.push_back(std::move(step));
        }

        // 1. The new .meta files, written first: a crash after this leaves the file where it was with its .meta
        const auto undoMetas = [&steps](const std::size_t count)
        {
            for (std::size_t i = count; i-- > 0;)
            {
                const Step &step = steps[i];
                std::error_code ignored;
                if (step.sameFile)
                {
                    if (step.oldText)
                    {
                        (void)WriteTextFileAtomically(step.oldMeta, *step.oldText);
                    }
                }
                else if (step.replaced)
                {
                    (void)WriteTextFileAtomically(step.newMeta, *step.replaced);
                }
                else
                {
                    std::filesystem::remove(step.newMeta, ignored);
                }
            }
        };
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            Step &step = steps[i];
            std::error_code error;
            if (std::filesystem::exists(step.oldMeta, error))
            {
                step.oldText = ReadWholeFile(step.oldMeta);
                try
                {
                    // The import settings as the file has them (edited by hand since the scan, perhaps)
                    step.moved.customData = AssetMetadata::FromFile(step.oldMeta).customData;
                }
                catch (const std::exception &)
                {
                    // A corrupt .meta: the index's copy is written instead
                }
            }
            if (std::filesystem::exists(step.newMeta, error))
            {
                std::error_code sameError;
                step.sameFile = std::filesystem::equivalent(step.oldMeta, step.newMeta, sameError) && !sameError;
                if (!step.sameFile)
                {
                    step.replaced = ReadWholeFile(step.newMeta);
                }
            }
            // The parts of a file with sub-assets keep the UUIDs hashed from the path it had when they were made
            const bool hasParts = _subAssetParentTypes.contains(step.moved.resourceType) ||
                                  (step.moved.customData.is_object() && step.moved.customData.contains("subAssets"));
            if (hasParts && step.moved.uuidSeed.empty())
            {
                step.moved.uuidSeed = step.oldPath.ToString();
            }
            if (!step.moved.uuidSeed.empty() && ResourcePath(step.moved.uuidSeed) == step.newPath)
            {
                step.moved.uuidSeed.clear(); // back where its seed came from
            }
            bool saved = false;
            std::string reason;
            try
            {
                saved = step.moved.SaveToFile(step.newMeta);
            }
            catch (const std::exception &e)
            {
                reason = e.what();
            }
            if (!saved)
            {
                undoMetas(i);
                return std::unexpected(std::format("can't write the .meta of {}{}", step.newPath.ToString(),
                                                   reason.empty() ? std::string{} : ": " + reason));
            }
        }

        // 2. The file or folder itself
        std::expected<void, std::string> renamed;
        try
        {
            if (renameOnDisk)
            {
                renamed = renameOnDisk();
            }
        }
        catch (const std::exception &e)
        {
            renamed = std::unexpected(std::string(e.what()));
        }
        if (!renamed)
        {
            undoMetas(steps.size());
            return std::unexpected(renamed.error());
        }

        // 3. The index, the caches, the parts and the state, re-keyed; the UUIDs are the same
        std::unordered_map<ResourcePath, ResourcePath, ResourcePath::Hash> newPathOf;
        std::vector<MovedAsset> moved;
        moved.reserve(steps.size());
        for (Step &step : steps)
        {
            newPathOf[step.oldPath] = step.newPath;
            _metadata.erase(step.oldPath);
            _uuidToPath[step.moved.uuid] = step.newPath;
            if (const auto state = _assetState.find(step.oldPath); state != _assetState.end())
            {
                const AssetState kept = state->second;
                _assetState.erase(state);
                _assetState[step.newPath] = kept;
                _assetStateDirty = true;
            }
            if (const auto cached = _cache.find(step.oldPath); cached != _cache.end())
            {
                std::shared_ptr<Base::Asset> asset = cached->second;
                _cache.erase(cached);
                asset->SetResourcePath(step.newPath);
                for (const Base::SubAssetRef &sub : asset->GetSubAssets())
                {
                    if (sub.asset)
                    {
                        sub.asset->SetResourcePath(step.newPath);
                    }
                }
                _cache[step.newPath] = std::move(asset);
            }
            if (_subAssetParentsTried.erase(step.oldPath) > 0)
            {
                _subAssetParentsTried.insert(step.newPath);
            }
            moved.push_back(MovedAsset{step.oldPath, step.newPath, step.moved.uuid, step.moved.resourceType});
            _metadata[step.newPath] = step.moved;
        }
        for (auto &[uuid, location] : _subAssets)
        {
            if (const auto renamedParent = newPathOf.find(location.parent); renamedParent != newPathOf.end())
            {
                location.parent = renamedParent->second;
            }
        }

        // 4. The old .meta files go, so a new file at an old path starts afresh
        for (const Step &step : steps)
        {
            if (step.sameFile)
            {
                // One file under two spellings (a case-only rename): its name takes the new spelling, so a
                // case-sensitive checkout finds it beside its file
                if (step.oldMeta.filename() != step.newMeta.filename() && !RenameCaseOnly(step.oldMeta, step.newMeta))
                {
                    Logger::Warn(std::format("Can't rename the .meta of {} to the new spelling", step.newPath.ToString()));
                }
                continue;
            }
            std::error_code ignored;
            std::filesystem::remove(step.oldMeta, ignored);
        }

        // A folder's .import folder: renamed itself for a case-only rename; otherwise the .meta files left in it (of
        // files the scan does not index, such as a model format this build can't read) follow their files, and the
        // folders left empty go
        const std::filesystem::path oldMetaFolder = _metadataRoot / PathFromUtf8(from.GetPath());
        const std::filesystem::path newMetaFolder = _metadataRoot / PathFromUtf8(to.GetPath());
        std::error_code folderError;
        if (std::filesystem::is_directory(oldMetaFolder, folderError))
        {
            std::error_code sameError;
            if (std::filesystem::exists(newMetaFolder, folderError) &&
                std::filesystem::equivalent(oldMetaFolder, newMetaFolder, sameError) && !sameError)
            {
                if (oldMetaFolder.filename() != newMetaFolder.filename() && !RenameCaseOnly(oldMetaFolder, newMetaFolder))
                {
                    Logger::Warn(std::format("Can't rename the .import folder of {} to the new spelling", to.ToString()));
                }
            }
            else
            {
                const std::filesystem::path newSourceFolder = Resolve(to);
                std::vector<std::filesystem::path> folders{oldMetaFolder};
                std::vector<std::pair<std::filesystem::path, std::filesystem::path>> follow;
                std::error_code walkError;
                for (std::filesystem::recursive_directory_iterator walk(oldMetaFolder, walkError), end;
                     !walkError && walk != end; walk.increment(walkError))
                {
                    std::error_code entryError;
                    if (walk->is_directory(entryError))
                    {
                        folders.push_back(walk->path());
                        continue;
                    }
                    if (!walk->is_regular_file(entryError) || walk->path().extension() != ".meta")
                    {
                        continue;
                    }
                    const std::filesystem::path relative = walk->path().lexically_relative(oldMetaFolder);
                    std::filesystem::path source = newSourceFolder / relative;
                    source.replace_extension(); // drops ".meta"
                    std::error_code existsError;
                    if (!newSourceFolder.empty() && std::filesystem::exists(source, existsError) &&
                        !std::filesystem::exists(newMetaFolder / relative, existsError))
                    {
                        follow.emplace_back(walk->path(), newMetaFolder / relative);
                    }
                }
                for (const auto &[oldFile, newFile] : follow)
                {
                    std::error_code moveError;
                    std::filesystem::create_directories(newFile.parent_path(), moveError);
                    std::filesystem::rename(oldFile, newFile, moveError);
                }
                // Deepest first; a folder that still holds something stays
                std::ranges::sort(folders, [](const std::filesystem::path &a, const std::filesystem::path &b)
                {
                    return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
                });
                for (const std::filesystem::path &folder : folders)
                {
                    std::error_code ignored;
                    if (std::filesystem::is_empty(folder, ignored) && !ignored)
                    {
                        std::filesystem::remove(folder, ignored);
                    }
                }
            }
        }

        SaveAssetState();
        Logger::Info(std::format("Moved {} to {} ({} asset{})", from.ToString(), to.ToString(), moved.size(),
                                 moved.size() == 1 ? "" : "s"));
        return moved;
    }
}
