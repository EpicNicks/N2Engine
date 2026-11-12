#include "engine/io/Vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>

#include "engine/io/ResourcePath.hpp"

namespace N2Engine::IO
{
    std::uint64_t FileTimeToUnixMilliseconds(const std::filesystem::file_time_type fileTime)
    {
#ifdef N2ENGINE_PLATFORM_WEB
        // libc++ (Emscripten) has no clock_cast; its file clock counts from the Unix epoch
        const std::chrono::sys_time<std::filesystem::file_time_type::duration> systemTime(fileTime.time_since_epoch());
#else
        const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
#endif
        const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(systemTime).time_since_epoch().count();
        return milliseconds > 0 ? static_cast<std::uint64_t>(milliseconds) : 0u;
    }

    namespace
    {
        /// The text a path is compared as: absolute, lexically normal, '/'-separated; folded to lower case on Windows,
        /// whose file system ignores case
        std::string KeyOf(const std::filesystem::path &path)
        {
            std::error_code error;
            std::filesystem::path absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
            if (error)
            {
                absolute = path;
            }
            std::string key = PathToUtf8(absolute.lexically_normal());
            while (key.size() > 1 && key.ends_with('/') && !key.ends_with(":/"))
            {
                key.pop_back();
            }
#ifdef _WIN32
            std::ranges::transform(key, key.begin(), [](const char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
#endif
            return key;
        }

        std::optional<VfsEntry> DiskStat(const std::filesystem::path &path)
        {
            std::error_code error;
            const auto status = std::filesystem::status(path, error);
            if (error || !std::filesystem::exists(status))
            {
                return std::nullopt;
            }
            VfsEntry entry;
            entry.path = PathToUtf8(path.filename());
            entry.directory = std::filesystem::is_directory(status);
            if (!entry.directory)
            {
                const auto size = std::filesystem::file_size(path, error);
                entry.size = error ? 0 : size;
            }
            const auto time = std::filesystem::last_write_time(path, error);
            entry.modified = error ? 0 : FileTimeToUnixMilliseconds(time);
            return entry;
        }

        std::expected<VfsBytes, std::string> DiskRead(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                return std::unexpected("Couldn't open " + PathToUtf8(path));
            }
            const std::streamoff size = file.tellg();
            if (size < 0)
            {
                return std::unexpected("Couldn't read " + PathToUtf8(path));
            }
            VfsBytes bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), size))
            {
                return std::unexpected("Couldn't read " + PathToUtf8(path));
            }
            return bytes;
        }

        /// Files and folders under directory on disk; paths relative to `base`
        void DiskList(const std::filesystem::path &directory, const std::filesystem::path &base, const bool recursive,
                      std::vector<VfsEntry> &out)
        {
            std::error_code error;
            for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error))
            {
                const auto &dirEntry = *it;
                VfsEntry entry;
                entry.path = PathToUtf8(dirEntry.path().lexically_relative(base));
                std::error_code entryError;
                entry.directory = dirEntry.is_directory(entryError);
                if (!entry.directory)
                {
                    const auto size = dirEntry.file_size(entryError);
                    entry.size = entryError ? 0 : size;
                }
                const auto time = dirEntry.last_write_time(entryError);
                entry.modified = entryError ? 0 : FileTimeToUnixMilliseconds(time);
                out.push_back(entry);
                if (entry.directory && recursive)
                {
                    DiskList(dirEntry.path(), base, true, out);
                }
            }
        }

        /// A folder on disk as a mount
        class FolderMount final : public IMount
        {
        public:
            explicit FolderMount(std::filesystem::path root) : _root(std::move(root)) {}

            std::string_view Kind() const override { return "folder"; }

            std::optional<VfsEntry> Stat(const std::string_view path) const override
            {
                const auto full = Full(path);
                if (!full)
                {
                    return std::nullopt;
                }
                auto entry = DiskStat(*full);
                if (entry)
                {
                    entry->path = std::string(path);
                }
                return entry;
            }

            std::expected<VfsBytes, std::string> Read(const std::string_view path) const override
            {
                const auto full = Full(path);
                if (!full)
                {
                    return std::unexpected(std::string(path) + " is not in the folder");
                }
                return DiskRead(*full);
            }

            std::vector<VfsEntry> List(const std::string_view directory, const bool recursive) const override
            {
                std::vector<VfsEntry> entries;
                if (const auto full = Full(directory))
                {
                    DiskList(*full, _root, recursive, entries);
                }
                return entries;
            }

        private:
            /// The disk path of a mount path; nullopt for one that is not a plain relative path (it would leave the root)
            std::optional<std::filesystem::path> Full(const std::string_view path) const
            {
                if (path.empty())
                {
                    return _root;
                }
                const auto normal = Pack::NormalizeEntryName(path);
                if (!normal)
                {
                    return std::nullopt;
                }
                return _root / PathFromUtf8(*normal);
            }

            std::filesystem::path _root;
        };
    }

    std::optional<std::string> Pack::NormalizeEntryName(std::string_view name)
    {
        if (name.ends_with('/'))
        {
            name.remove_suffix(1);
        }
        if (name.empty() || name.starts_with('/'))
        {
            return std::nullopt;
        }
        std::string result;
        result.reserve(name.size());
        std::size_t start = 0;
        while (start <= name.size())
        {
            const std::size_t slash = name.find('/', start);
            const std::string_view segment = name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (segment.empty() || segment == "." || segment == "..")
            {
                return std::nullopt;
            }
            for (const char c : segment)
            {
                const auto byte = static_cast<unsigned char>(c);
                // A backslash would be a separator on Windows; ':' a drive or a stream; control characters are no file names
                if (byte < 0x20 || byte == 0x7f || c == '\\' || c == ':')
                {
                    return std::nullopt;
                }
            }
            if (!result.empty())
            {
                result += '/';
            }
            result += segment;
            if (slash == std::string_view::npos)
            {
                break;
            }
            start = slash + 1;
        }
        return result;
    }

    std::expected<std::unique_ptr<IMount>, std::string> Pack::Open(const std::filesystem::path &file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream.is_open())
        {
            return std::unexpected("Couldn't open the pack " + PathToUtf8(file));
        }
        std::array<char, 6> magic{};
        stream.read(magic.data(), magic.size());
        const auto read = stream.gcount();
        static constexpr std::array<char, 6> SevenZipMagic{'7', 'z', '\xBC', '\xAF', '\x27', '\x1C'};
        if (read == 6 && magic == SevenZipMagic)
        {
            return OpenSevenZip(file);
        }
        if (read >= 4 && magic[0] == 'P' && magic[1] == 'K')
        {
            return OpenZip(file);
        }
        return std::unexpected(PathToUtf8(file) + " is not a pack (neither zip nor 7z)");
    }

    std::unique_ptr<IMount> MakeFolderMount(const std::filesystem::path &root)
    {
        return std::make_unique<FolderMount>(root);
    }

    Vfs &Vfs::Instance()
    {
        static Vfs instance;
        return instance;
    }

    void Vfs::Mount(const std::filesystem::path &at, std::unique_ptr<IMount> mount)
    {
        std::unique_lock lock(_mutex);
        std::string key = KeyOf(at);
        std::erase_if(_mounts, [&](const MountPoint &m) { return m.key == key; });
        std::error_code error;
        std::filesystem::path root = at.is_absolute() ? at : std::filesystem::absolute(at, error);
        _mounts.push_back({root.lexically_normal(), std::move(key), std::shared_ptr<IMount>(std::move(mount))});
        // The deepest root first, so a mount inside another wins for its own paths
        std::ranges::sort(_mounts, [](const MountPoint &a, const MountPoint &b) { return a.key.size() > b.key.size(); });
    }

    bool Vfs::Unmount(const std::filesystem::path &at)
    {
        std::unique_lock lock(_mutex);
        const std::string key = KeyOf(at);
        return std::erase_if(_mounts, [&](const MountPoint &m) { return m.key == key; }) > 0;
    }

    void Vfs::UnmountAll()
    {
        std::unique_lock lock(_mutex);
        _mounts.clear();
    }

    std::shared_ptr<IMount> Vfs::Find(const std::filesystem::path &path, std::string &relative) const
    {
        std::shared_lock lock(_mutex);
        if (_mounts.empty())
        {
            return nullptr;
        }
        const std::string key = KeyOf(path);
        for (const MountPoint &m : _mounts)
        {
            if (key == m.key)
            {
                relative.clear();
                return m.mount;
            }
            const bool rootEndsWithSlash = m.key.ends_with('/');
            if (key.size() > m.key.size() && key.starts_with(m.key) && (rootEndsWithSlash || key[m.key.size()] == '/'))
            {
                // The original spelling's case, not the folded key's: pack entry names keep their case
                const std::string spelled = PathToUtf8(
                    (path.is_absolute() ? path : std::filesystem::absolute(path)).lexically_normal().lexically_relative(m.root));
                relative = spelled;
                return m.mount;
            }
        }
        return nullptr;
    }

    bool Vfs::IsMounted(const std::filesystem::path &path) const
    {
        std::string relative;
        return Find(path, relative) != nullptr;
    }

    std::string Vfs::MountKindOf(const std::filesystem::path &path) const
    {
        std::string relative;
        const auto mount = Find(path, relative);
        return mount ? std::string(mount->Kind()) : std::string("disk");
    }

    std::optional<VfsEntry> Vfs::Stat(const std::filesystem::path &path) const
    {
        std::string relative;
        if (const auto mount = Find(path, relative))
        {
            if (relative == ".")
            {
                relative.clear();
            }
            return mount->Stat(relative);
        }
        // Lexically normal first: on POSIX "a/../b" fails when "a" does not exist (a mount point, say)
        return DiskStat(path.lexically_normal());
    }

    bool Vfs::IsDirectory(const std::filesystem::path &path) const
    {
        const auto entry = Stat(path);
        return entry && entry->directory;
    }

    bool Vfs::IsRegularFile(const std::filesystem::path &path) const
    {
        const auto entry = Stat(path);
        return entry && !entry->directory;
    }

    std::expected<VfsBytes, std::string> Vfs::ReadBytes(const std::filesystem::path &path) const
    {
        std::string relative;
        if (const auto mount = Find(path, relative))
        {
            if (relative.empty() || relative == ".")
            {
                return std::unexpected(PathToUtf8(path) + " is a folder");
            }
            return mount->Read(relative);
        }
        return DiskRead(path.lexically_normal());
    }

    std::expected<VfsBytes, std::string> Vfs::ReadBytesCapped(const std::filesystem::path &path, const std::uint64_t maxBytes) const
    {
        const auto entry = Stat(path);
        if (!entry || entry->directory)
        {
            return std::unexpected(PathToUtf8(path) + " was not found");
        }
        if (entry->size > maxBytes)
        {
            return std::unexpected(PathToUtf8(path) + " is " + std::to_string(entry->size) + " bytes, over the " +
                                   std::to_string(maxBytes) + "-byte limit");
        }
        return ReadBytes(path);
    }

    std::expected<std::string, std::string> Vfs::ReadText(const std::filesystem::path &path) const
    {
        auto bytes = ReadBytes(path);
        if (!bytes)
        {
            return std::unexpected(std::move(bytes.error()));
        }
        return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
    }

    std::vector<VfsEntry> Vfs::ListFiles(const std::filesystem::path &directory, const bool recursive) const
    {
        std::vector<VfsEntry> entries;
        std::string relative;
        if (const auto mount = Find(directory, relative))
        {
            if (relative == ".")
            {
                relative.clear();
            }
            const std::string prefix = relative.empty() ? std::string() : relative + "/";
            for (VfsEntry &entry : mount->List(relative, recursive))
            {
                if (entry.directory || !entry.path.starts_with(prefix))
                {
                    continue;
                }
                entry.path = entry.path.substr(prefix.size());
                entries.push_back(std::move(entry));
            }
        }
        else
        {
            std::vector<VfsEntry> all;
            const std::filesystem::path normal = directory.lexically_normal();
            DiskList(normal, normal, recursive, all);
            for (VfsEntry &entry : all)
            {
                if (!entry.directory)
                {
                    entries.push_back(std::move(entry));
                }
            }
        }
        std::ranges::sort(entries, [](const VfsEntry &a, const VfsEntry &b) { return a.path < b.path; });
        return entries;
    }
}
