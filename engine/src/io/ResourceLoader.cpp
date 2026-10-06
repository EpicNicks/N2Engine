#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/Logger.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/text/Font.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace N2Engine::IO
{
    namespace
    {
        /// The .meta resourceType for a file, from its extension, case-insensitively (as the loader lookup
        /// is): Music.WAV is an AudioClip like music.wav
        std::string ResourceTypeFor(const std::filesystem::path &sourcePath)
        {
            std::string ext = sourcePath.extension().string();
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
            return "Unknown";
        }
    }

    void ResourceLoader::Initialize(const std::filesystem::path& projectRoot)
    {
        // Font.cpp's and Texture.cpp's own static registrars only run if the linker keeps those files, which
        // a program that names neither type wouldn't; registering here (idempotent) makes .ttf/.otf and the
        // image extensions scan and load anyway. Done before the roots change, so a first registration
        // doesn't rescan anything.
        Text::Font::RegisterLoader();
        Rendering::Texture::RegisterLoader();

        _projectRoot = projectRoot;
        _assetsRoot = projectRoot / "assets";
        _metadataRoot = projectRoot / ".import";
        _userDataRoot = GetUserDataPath();

        std::filesystem::create_directories(_metadataRoot);
        std::filesystem::create_directories(_userDataRoot);

        Logger::Info(std::format("ResourceLoader initialized: {}", projectRoot.string()));

        // Everything here is keyed by res:// paths, which don't include the root: entries from a
        // previous root would otherwise pass for this one's files
        _metadata.clear();
        _uuidToPath.clear();
        ClearCache();

        RescanAssets();

        Logger::Info(std::format("Found {} assets", _metadata.size()));
    }

    std::filesystem::path ResourceLoader::GetUserDataPath() const
    {
#ifdef _WIN32
        char *appData = nullptr;
        size_t len = 0;
        if (_dupenv_s(&appData, &len, "APPDATA") == 0 && appData != nullptr)
        {
            std::filesystem::path path(appData);
            free(appData);
            return path / "N2Engine";
        }
        return std::filesystem::path(".");
#else
        // _dupenv_s is MSVC-only
        if (const char *home = std::getenv("HOME"); home != nullptr)
        {
            return std::filesystem::path(home) / ".n2engine";
        }
        return std::filesystem::path(".");
#endif
    }

    void ResourceLoader::RescanAssets()
    {
        if (!std::filesystem::exists(_assetsRoot))
        {
            Logger::Warn("Assets directory not found");
            return;
        }

        ScanDirectory(_assetsRoot);
    }

    void ResourceLoader::ScanDirectory(const std::filesystem::path &directory)
    {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (!entry.is_regular_file())
                continue;

            if (entry.path().extension() == ".meta")
                continue;

            std::string ext = entry.path().extension().string();
            std::ranges::transform(ext, ext.begin(), ::tolower);

            if (_loaders.find(ext) == _loaders.end())
                continue;

            try
            {
                AssetMetadata meta = CreateOrUpdateMetadata(entry.path());
                _metadata[meta.resourcePath] = meta;
                _uuidToPath[meta.uuid] = meta.resourcePath;
            }
            catch (const std::exception &e)
            {
                Logger::Warn(std::format("Skipping asset {}: {}", entry.path().string(), e.what()));
            }
        }
    }

    AssetMetadata ResourceLoader::CreateOrUpdateMetadata(const std::filesystem::path &sourcePath)
    {
        auto lastWrite = std::filesystem::last_write_time(sourcePath);
        auto fileSize = std::filesystem::file_size(sourcePath);

        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            lastWrite - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now()
        );
        uint64_t timestamp = std::chrono::system_clock::to_time_t(sctp);

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
                Logger::Warn(std::format("Corrupt metadata {} ({}); regenerating", metaPath.string(), e.what()));
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
        return _metadataRoot / relative.parent_path() / (relative.filename().string() + ".meta");
    }

    namespace
    {
        // Joins a relative resource path onto its root and rejects anything that normalizes to
        // outside it (e.g. res://../../secrets), which used to resolve to wherever ".." led
        std::filesystem::path ResolveUnder(const std::filesystem::path &root, const std::string &relative)
        {
            const std::filesystem::path normalizedRoot = root.lexically_normal();
            const std::filesystem::path resolved = (root / relative).lexically_normal();
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
        if (!relative.string().starts_with(".."))
        {
            return ResourcePath(PathType::Resource, relative.string());
        }

        relative = std::filesystem::relative(physicalPath, _userDataRoot);
        if (!relative.string().starts_with(".."))
        {
            return ResourcePath(PathType::User, relative.string());
        }

        return ResourcePath(PathType::Absolute, physicalPath.string());
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

        auto lastWrite = std::filesystem::last_write_time(sourcePath);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            lastWrite - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now()
        );
        uint64_t timestamp = std::chrono::system_clock::to_time_t(sctp);

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
            if (it->second.use_count() <= (inUUIDCache ? 2 : 1))
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
}
