#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::IO
{
    /**
     * The engine's virtual filesystem (build targets stage 4; docs/packs.html). Every read the engine makes of a game's files
     * (project.n2proj, the assets, their .meta files, scripts, shaders) goes through Vfs: a path inside a mount is served
     * by that mount, any other path by the disk, as before. A mount makes a folder, a pack (zip or 7z) or a platform bundle
     * appear at an absolute path, which need not exist on disk: a player mounts game.n2pack at <its folder>/game and then
     * opens <its folder>/game as the project, exactly as it opens a folder.
     *
     * Paths are the engine's ordinary std::filesystem paths, so nothing above this layer changes its spelling. Writes are not
     * part of it: a game writes only to user://, which is always a folder on disk.
     */

    /// One file or folder: its path relative to the folder listed (or the mount root), '/'-separated
    struct VfsEntry
    {
        std::string path;
        std::uint64_t size = 0;
        /// Modification time in milliseconds since the Unix epoch (a pack's stored time, or 0 when it has none)
        std::uint64_t modified = 0;
        bool directory = false;
    };

    using VfsBytes = std::vector<std::uint8_t>;

    /// A file time as milliseconds since the Unix epoch (0 before it)
    [[nodiscard]] std::uint64_t FileTimeToUnixMilliseconds(std::filesystem::file_time_type fileTime);

    /// What a mount serves. Paths given to it are normalised and relative to its root ("" is the root itself).
    class IMount
    {
    public:
        virtual ~IMount() = default;
        /// "folder", "zip" or "7z"
        [[nodiscard]] virtual std::string_view Kind() const = 0;
        [[nodiscard]] virtual std::optional<VfsEntry> Stat(std::string_view path) const = 0;
        /// The whole file. Bounded (Pack::MaxEntrySize) and checked (a pack's CRC); an error names the file and the reason
        [[nodiscard]] virtual std::expected<VfsBytes, std::string> Read(std::string_view path) const = 0;
        /// The files and folders in a folder (recursive: everything under it), paths relative to the mount root
        [[nodiscard]] virtual std::vector<VfsEntry> List(std::string_view directory, bool recursive) const = 0;
    };

    namespace Pack
    {
        /// The largest file a pack may hold, uncompressed (a larger declared size is refused before anything is read)
        inline constexpr std::uint64_t MaxEntrySize = 1ull << 30;
        /// The most entries a pack may hold
        inline constexpr std::size_t MaxEntries = 1'000'000;

        /**
         * An entry name as a mount path: '/'-separated, no empty, "." or ".." segment, no root or drive, no backslash and
         * no control character. nullopt for a name that could reach outside the pack (zip slip) or is not a plain path; a
         * pack holding one is refused whole. A trailing '/' (a zip folder entry) is dropped.
         */
        [[nodiscard]] std::optional<std::string> NormalizeEntryName(std::string_view name);

        /// A zip (stored or deflate) or 7z pack, told apart by its first bytes
        [[nodiscard]] std::expected<std::unique_ptr<IMount>, std::string> Open(const std::filesystem::path &file);
        [[nodiscard]] std::expected<std::unique_ptr<IMount>, std::string> OpenZip(const std::filesystem::path &file);
        [[nodiscard]] std::expected<std::unique_ptr<IMount>, std::string> OpenSevenZip(const std::filesystem::path &file);
    }

    /// A folder on disk as a mount (reads confined to it: a path that leaves it is not found)
    [[nodiscard]] std::unique_ptr<IMount> MakeFolderMount(const std::filesystem::path &root);

    class Vfs
    {
    public:
        static Vfs &Instance();

        /// Makes `mount`'s files appear at `at` (absolute; need not exist), replacing a mount already there
        void Mount(const std::filesystem::path &at, std::unique_ptr<IMount> mount);
        /// Whether a mount was there
        bool Unmount(const std::filesystem::path &at);
        void UnmountAll();

        /// Whether path is inside a mount (so not on disk)
        [[nodiscard]] bool IsMounted(const std::filesystem::path &path) const;
        /// The kind of mount serving path ("folder", "zip", "7z"), or "disk"
        [[nodiscard]] std::string MountKindOf(const std::filesystem::path &path) const;

        [[nodiscard]] std::optional<VfsEntry> Stat(const std::filesystem::path &path) const;
        [[nodiscard]] bool Exists(const std::filesystem::path &path) const { return Stat(path).has_value(); }
        [[nodiscard]] bool IsDirectory(const std::filesystem::path &path) const;
        [[nodiscard]] bool IsRegularFile(const std::filesystem::path &path) const;

        [[nodiscard]] std::expected<VfsBytes, std::string> ReadBytes(const std::filesystem::path &path) const;
        [[nodiscard]] std::expected<std::string, std::string> ReadText(const std::filesystem::path &path) const;
        /// ReadBytes, refused without reading when the file is larger than maxBytes
        [[nodiscard]] std::expected<VfsBytes, std::string> ReadBytesCapped(const std::filesystem::path &path,
                                                                           std::uint64_t maxBytes) const;

        /// The files (not folders) in directory, recursively unless told otherwise, paths relative to it, sorted
        [[nodiscard]] std::vector<VfsEntry> ListFiles(const std::filesystem::path &directory, bool recursive = true) const;

    private:
        Vfs() = default;

        struct MountPoint
        {
            std::filesystem::path root; ///< lexically normal, absolute
            std::string key;            ///< root as the text prefixes are compared with
            std::shared_ptr<IMount> mount;
        };
        /// The mount serving path and path relative to its root, or nullptr (the disk)
        [[nodiscard]] std::shared_ptr<IMount> Find(const std::filesystem::path &path, std::string &relative) const;

        mutable std::shared_mutex _mutex;
        std::vector<MountPoint> _mounts;
    };
}
