#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/Logger.hpp"
#include <fstream>

namespace N2Engine::IO
{
    void ResourceLoader::Initialize(const std::filesystem::path& projectRoot)
    {
        _projectRoot = projectRoot;
        _assetsRoot = projectRoot / "assets";
        _metadataRoot = projectRoot / ".import";
        _userDataRoot = GetUserDataPath();

        std::filesystem::create_directories(_metadataRoot);
        std::filesystem::create_directories(_userDataRoot);

        Logger::Info(std::format("ResourceLoader initialized: {}", projectRoot.string()));

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
        char *home = nullptr;
        size_t len = 0;
        if (_dupenv_s(&home, &len, "HOME") == 0 && home != nullptr)
        {
            std::filesystem::path path(home);
            free(home);
            return path / ".n2engine";
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

            AssetMetadata meta = CreateOrUpdateMetadata(entry.path());

            _metadata[meta.resourcePath] = meta;
            _uuidToPath[meta.uuid] = meta.resourcePath;
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

        if (std::filesystem::exists(metaPath))
        {
            meta = AssetMetadata::FromFile(metaPath);

            // Verify UUID is deterministic
            Math::UUID expectedUUID = ResourceUUID::FromPath(resourcePath);
            if (meta.uuid != expectedUUID)
            {
                Logger::Warn(std::format("UUID mismatch for {}. Regenerating.",
                                         resourcePath.ToString()));
                meta.uuid = expectedUUID;
                meta.SaveToFile(metaPath);
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

            std::string ext = sourcePath.extension().string();
            if (ext == ".lua")
                meta.resourceType = "LuaScript";
            else if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
                meta.resourceType = "AudioClip";
            else if (ext == ".png" || ext == ".jpg")
                meta.resourceType = "Texture";
            else
                meta.resourceType = "Unknown";

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

    std::filesystem::path ResourceLoader::Resolve(const ResourcePath &resourcePath) const
    {
        switch (resourcePath.GetType())
        {
        case PathType::Resource:
            return _assetsRoot / resourcePath.GetPath();
        case PathType::User:
            return _userDataRoot / resourcePath.GetPath();
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
        CreateOrUpdateMetadata(sourcePath);

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
            if (it->second.use_count() <= 1)
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
        _loaders[extension] = std::move(loader);
    }
}
