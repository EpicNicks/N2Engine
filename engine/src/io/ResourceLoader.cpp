#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/sceneManagement/SceneFile.hpp"
#include "engine/text/Font.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <utility>
#include <vector>

namespace N2Engine::IO
{
    namespace
    {
        /// A file's modification time in whole Unix seconds, converted exactly (clock_cast), so the same file time
        /// always gives the same number. The old conversion went through both clocks' now(), which differ by a
        /// moment on each call, so a file could look modified on one scan and not the next.
        uint64_t UnixSeconds(const std::filesystem::file_time_type fileTime)
        {
            const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
            const auto seconds = std::chrono::floor<std::chrono::seconds>(systemTime).time_since_epoch().count();
            return seconds > 0 ? static_cast<uint64_t>(seconds) : 0u;
        }

        /// The .meta resourceType for a file, from its extension, case-insensitively (as the loader lookup
        /// is): Music.WAV is an AudioClip like music.wav
        std::string ResourceTypeFor(const std::filesystem::path &sourcePath)
        {
            std::string ext = PathToUtf8(sourcePath.extension());
            std::ranges::transform(ext, ext.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".lua")
                return "LuaScript";
            if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
                return "AudioClip";
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
                return "Texture";
            if (ext == ".ttf" || ext == ".otf")
                return "Font";
            if (ext == ".mat")
                return "Material";
            if (ext == ".gltf" || ext == ".glb")
                return "Model";
            if (ext == SceneFile::Extension)
                return std::string(SceneFile::ResourceTypeName);
            return "Unknown";
        }
    }

    void ResourceLoader::Initialize(const std::filesystem::path& projectRoot, const std::filesystem::path& userDataRoot)
    {
        // Font.cpp's, Texture.cpp's, Material.cpp's, Model.cpp's and SceneFile.cpp's own registration only runs if
        // something calls it (or, for the static registrars, the linker keeps those files, which a program that
        // names none of the types wouldn't); registering here (idempotent) makes .ttf/.otf, the image extensions,
        // .mat, .gltf/.glb and .scene scan and load anyway. Done before the roots change, so a first registration
        // doesn't rescan anything.
        Text::Font::RegisterLoader();
        Rendering::Texture::RegisterLoader();
        Rendering::Material::RegisterLoader();
        Rendering::Model::RegisterLoader();
        SceneFile::RegisterLoader();

        _projectRoot = projectRoot;
        _assetsRoot = projectRoot / "assets";
        _metadataRoot = projectRoot / ".import";
        _userDataRoot = userDataRoot.empty() ? ProjectFile::UserDataBase() : userDataRoot;

        std::filesystem::create_directories(_metadataRoot);
        std::filesystem::create_directories(_userDataRoot);

        Logger::Info(std::format("ResourceLoader initialized: {}", PathToUtf8(projectRoot)));

        // Everything here is keyed by res:// paths, which don't include the root: entries from a
        // previous root would otherwise pass for this one's files
        _metadata.clear();
        _uuidToPath.clear();
        _subAssets.clear();
        _subAssetParentsTried.clear();
        ClearCache();

        (void)RescanAssets();

        Logger::Info(std::format("Found {} assets", _metadata.size()));
    }

    ResourceLoader::RescanResult ResourceLoader::RescanAssets()
    {
        RescanResult result;
        if (!std::filesystem::exists(_assetsRoot))
        {
            Logger::Warn("Assets directory not found");
            return result;
        }

        std::unordered_set<ResourcePath, ResourcePath::Hash> seen;
        ScanDirectory(_assetsRoot, result, seen);

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
            it = _metadata.erase(it);
        }

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
        for (const auto &entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (!entry.is_regular_file())
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
    }

    AssetMetadata ResourceLoader::CreateOrUpdateMetadata(const std::filesystem::path &sourcePath)
    {
        const uint64_t timestamp = UnixSeconds(std::filesystem::last_write_time(sourcePath));
        auto fileSize = std::filesystem::file_size(sourcePath);

        ResourcePath resourcePath = MakeResourcePath(sourcePath);
        std::filesystem::path metaPath = GetMetadataPath(sourcePath);

        AssetMetadata meta;

        bool haveMeta = false;
        if (std::filesystem::exists(metaPath))
        {
            try
            {
                meta = AssetMetadata::FromFile(metaPath);
                haveMeta = true;
            }
            catch (const std::exception &e)
            {
                // A corrupt .meta used to throw out of Initialize; regenerate it instead (the UUID is
                // derived from the path, so references to the asset keep resolving)
                Logger::Warn(std::format("Corrupt metadata {} ({}); regenerating", PathToUtf8(metaPath), e.what()));
            }
        }

        if (haveMeta)
        {
            // Verify UUID is deterministic
            Math::UUID expectedUUID = ResourceUUID::FromPath(resourcePath);
            if (meta.uuid != expectedUUID)
            {
                Logger::Warn(std::format("UUID mismatch for {}. Regenerating.",
                                         resourcePath.ToString()));
                meta.uuid = expectedUUID;
                meta.SaveToFile(metaPath);
            }

            // A .meta written before its extension was known (or matched case-insensitively, as Music.WAV
            // now is) said "Unknown"; give it its type now
            if (meta.resourceType == "Unknown")
            {
                if (std::string type = ResourceTypeFor(sourcePath); type != "Unknown")
                {
                    meta.resourceType = std::move(type);
                    meta.SaveToFile(metaPath);
                }
            }

            if (meta.lastModified != timestamp || meta.fileSize != fileSize)
            {
                meta.lastModified = timestamp;
                meta.fileSize = fileSize;
                meta.SaveToFile(metaPath);
                Logger::Info(std::format("Asset modified: {}", resourcePath.ToString()));
            }
        }
        else
        {
            meta.uuid = ResourceUUID::FromPath(resourcePath);
            meta.resourcePath = resourcePath;
            meta.lastModified = timestamp;
            meta.fileSize = fileSize;

            meta.resourceType = ResourceTypeFor(sourcePath);
            meta.SaveToFile(metaPath);
            Logger::Info(std::format("New asset: {}", resourcePath.ToString()));
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

        const uint64_t timestamp = UnixSeconds(std::filesystem::last_write_time(sourcePath));

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
        AssetMetadata refreshed = CreateOrUpdateMetadata(sourcePath);
        _metadata[resourcePath] = refreshed;
        _uuidToPath[refreshed.uuid] = resourcePath;

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
                saved = meta.SaveToFile(metaPath);
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
        for (Base::SubAssetRef &sub : subAssets)
        {
            if (!sub.asset)
            {
                continue;
            }
            const Math::UUID uuid = ResourceUUID::FromSubAsset(parent, sub.key);
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
            // Re-derived, not trusted from the file: a moved file's index names its old path's UUIDs
            _subAssets[ResourceUUID::FromSubAsset(meta.resourcePath, item.key())] =
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
}
