// Zip packs (docs/packs.html): the central directory is read once when the pack opens; each read seeks to one entry,
// checks its local header, reads its bytes and, for deflate, inflates them with miniz's tinfl, then checks the CRC-32.
// Every offset and size is checked against the file before anything is read, a declared size is bounded
// (Pack::MaxEntrySize), and a name that could leave the pack refuses the whole pack. Stored (method 0) and deflate (8) are
// read; encrypted entries and other methods are refused with the reason.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <ranges>
#include <set>

#include <miniz.h>

#include "engine/io/ResourcePath.hpp"
#include "engine/io/Vfs.hpp"

namespace N2Engine::IO
{
    namespace
    {
        std::uint16_t U16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
        std::uint32_t U32(const std::uint8_t *p)
        {
            return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }
        std::uint64_t U64(const std::uint8_t *p) { return U32(p) | (static_cast<std::uint64_t>(U32(p + 4)) << 32); }

        constexpr std::uint32_t LocalHeaderSignature = 0x04034b50;
        constexpr std::uint32_t CentralHeaderSignature = 0x02014b50;
        constexpr std::uint32_t EndSignature = 0x06054b50;
        constexpr std::uint32_t Zip64EndSignature = 0x06064b50;
        constexpr std::uint32_t Zip64LocatorSignature = 0x07064b50;

        /// A DOS date and time (local time, no zone) as Unix milliseconds, read as if UTC
        std::uint64_t DosTimeToUnixMilliseconds(const std::uint16_t time, const std::uint16_t date)
        {
            if (date == 0)
            {
                return 0;
            }
            const int year = 1980 + (date >> 9);
            const unsigned month = (date >> 5) & 0x0f;
            const unsigned day = date & 0x1f;
            const std::chrono::year_month_day ymd{std::chrono::year(year), std::chrono::month(month), std::chrono::day(day)};
            if (!ymd.ok())
            {
                return 0;
            }
            const auto days = std::chrono::sys_days(ymd).time_since_epoch();
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(days).count() + (time >> 11) * 3600 +
                                 ((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;
            return seconds > 0 ? static_cast<std::uint64_t>(seconds) * 1000u : 0u;
        }

        struct ZipEntry
        {
            std::uint64_t localHeaderOffset = 0;
            std::uint64_t compressedSize = 0;
            std::uint64_t size = 0;
            std::uint32_t crc = 0;
            std::uint16_t method = 0;
            std::uint16_t flags = 0;
            std::uint64_t modified = 0;
        };

        class ZipMount final : public IMount
        {
        public:
            ZipMount(std::filesystem::path file, std::uint64_t fileSize, std::map<std::string, ZipEntry> entries)
                : _file(std::move(file)), _fileSize(fileSize), _entries(std::move(entries))
            {
                for (const auto &name : _entries | std::views::keys)
                {
                    for (std::size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1))
                    {
                        _folders.insert(name.substr(0, slash));
                    }
                }
            }

            std::string_view Kind() const override { return "zip"; }

            std::optional<VfsEntry> Stat(const std::string_view path) const override
            {
                if (path.empty() || _folders.contains(std::string(path)))
                {
                    return VfsEntry{std::string(path), 0, 0, true};
                }
                const auto it = _entries.find(std::string(path));
                if (it == _entries.end())
                {
                    return std::nullopt;
                }
                return VfsEntry{it->first, it->second.size, it->second.modified, false};
            }

            std::vector<VfsEntry> List(const std::string_view directory, const bool recursive) const override
            {
                std::vector<VfsEntry> out;
                const std::string prefix = directory.empty() ? std::string() : std::string(directory) + "/";
                const auto direct = [&](const std::string &name) {
                    return name.starts_with(prefix) && (recursive || name.find('/', prefix.size()) == std::string::npos);
                };
                for (const auto &folder : _folders)
                {
                    if (direct(folder))
                    {
                        out.push_back({folder, 0, 0, true});
                    }
                }
                for (const auto &[name, entry] : _entries)
                {
                    if (direct(name))
                    {
                        out.push_back({name, entry.size, entry.modified, false});
                    }
                }
                return out;
            }

            std::expected<VfsBytes, std::string> Read(const std::string_view path) const override
            {
                const auto it = _entries.find(std::string(path));
                if (it == _entries.end())
                {
                    return std::unexpected(std::string(path) + " is not in the pack");
                }
                const ZipEntry &entry = it->second;
                const std::string where = std::string(path) + " in " + PathToUtf8(_file);
                if (entry.flags & 0x1)
                {
                    return std::unexpected(where + " is encrypted");
                }
                if (entry.method != 0 && entry.method != 8)
                {
                    return std::unexpected(where + " uses compression method " + std::to_string(entry.method) +
                                           " (a pack is stored or deflate)");
                }
                std::ifstream file(_file, std::ios::binary);
                if (!file.is_open())
                {
                    return std::unexpected("Couldn't open " + PathToUtf8(_file));
                }
                std::uint8_t header[30];
                if (entry.localHeaderOffset + sizeof header > _fileSize)
                {
                    return std::unexpected(where + ": its header is past the end of the pack");
                }
                file.seekg(static_cast<std::streamoff>(entry.localHeaderOffset));
                if (!file.read(reinterpret_cast<char *>(header), sizeof header) || U32(header) != LocalHeaderSignature)
                {
                    return std::unexpected(where + ": no local header where the directory says");
                }
                const std::uint64_t dataOffset = entry.localHeaderOffset + sizeof header + U16(header + 26) + U16(header + 28);
                if (dataOffset > _fileSize || entry.compressedSize > _fileSize - dataOffset)
                {
                    return std::unexpected(where + ": its data is past the end of the pack");
                }
                VfsBytes compressed(static_cast<std::size_t>(entry.compressedSize));
                file.seekg(static_cast<std::streamoff>(dataOffset));
                if (!compressed.empty() && !file.read(reinterpret_cast<char *>(compressed.data()), static_cast<std::streamsize>(compressed.size())))
                {
                    return std::unexpected(where + ": couldn't read its data");
                }
                VfsBytes bytes;
                if (entry.method == 0)
                {
                    if (entry.compressedSize != entry.size)
                    {
                        return std::unexpected(where + ": a stored entry whose sizes differ");
                    }
                    bytes = std::move(compressed);
                }
                else
                {
                    bytes.resize(static_cast<std::size_t>(entry.size));
                    const std::size_t inflated = tinfl_decompress_mem_to_mem(bytes.data(), bytes.size(), compressed.data(), compressed.size(), 0);
                    if (inflated == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || inflated != entry.size)
                    {
                        return std::unexpected(where + ": its deflate data is damaged");
                    }
                }
                const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()));
                if (crc != entry.crc)
                {
                    return std::unexpected(where + ": its CRC does not match (the pack is damaged)");
                }
                return bytes;
            }

        private:
            std::filesystem::path _file;
            std::uint64_t _fileSize;
            std::map<std::string, ZipEntry> _entries;
            std::set<std::string> _folders;
        };
    }

    std::expected<std::unique_ptr<IMount>, std::string> Pack::OpenZip(const std::filesystem::path &path)
    {
        const std::string name = PathToUtf8(path);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            return std::unexpected("Couldn't open the pack " + name);
        }
        const std::uint64_t fileSize = static_cast<std::uint64_t>(file.tellg());
        if (fileSize < 22)
        {
            return std::unexpected(name + " is too short to be a zip pack");
        }
        // The end record is in the last 22 + 65535 bytes (its comment is at most 65535 bytes)
        const std::uint64_t tailSize = std::min<std::uint64_t>(fileSize, 22 + 65535 + 20);
        VfsBytes tail(static_cast<std::size_t>(tailSize));
        file.seekg(static_cast<std::streamoff>(fileSize - tailSize));
        if (!file.read(reinterpret_cast<char *>(tail.data()), static_cast<std::streamsize>(tailSize)))
        {
            return std::unexpected("Couldn't read " + name);
        }
        std::size_t end = std::string::npos;
        for (std::size_t i = tailSize - 22 + 1; i-- > 0;)
        {
            if (U32(tail.data() + i) == EndSignature && i + 22 + U16(tail.data() + i + 20) == tailSize)
            {
                end = i;
                break;
            }
        }
        if (end == std::string::npos)
        {
            return std::unexpected(name + " has no zip directory (not a zip, or cut short)");
        }
        const std::uint8_t *record = tail.data() + end;
        std::uint64_t count = U16(record + 10);
        std::uint64_t directorySize = U32(record + 12);
        std::uint64_t directoryOffset = U32(record + 16);
        // ZIP64: the locator sits just before the end record and points at the ZIP64 end record
        if (count == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff)
        {
            if (end < 20 || U32(record - 20) != Zip64LocatorSignature)
            {
                return std::unexpected(name + ": a ZIP64 pack without its locator");
            }
            const std::uint64_t zip64End = U64(record - 20 + 8);
            std::uint8_t z[56];
            if (zip64End + sizeof z > fileSize)
            {
                return std::unexpected(name + ": its ZIP64 directory record is past the end");
            }
            file.seekg(static_cast<std::streamoff>(zip64End));
            if (!file.read(reinterpret_cast<char *>(z), sizeof z) || U32(z) != Zip64EndSignature)
            {
                return std::unexpected(name + ": no ZIP64 directory record where the locator says");
            }
            count = U64(z + 32);
            directorySize = U64(z + 40);
            directoryOffset = U64(z + 48);
        }
        if (count > MaxEntries)
        {
            return std::unexpected(name + " holds more than " + std::to_string(MaxEntries) + " entries");
        }
        if (directoryOffset > fileSize || directorySize > fileSize - directoryOffset)
        {
            return std::unexpected(name + ": its directory is past the end of the file");
        }
        VfsBytes directory(static_cast<std::size_t>(directorySize));
        file.seekg(static_cast<std::streamoff>(directoryOffset));
        if (directorySize > 0 && !file.read(reinterpret_cast<char *>(directory.data()), static_cast<std::streamsize>(directorySize)))
        {
            return std::unexpected("Couldn't read the directory of " + name);
        }

        std::map<std::string, ZipEntry> entries;
        std::size_t at = 0;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            if (at + 46 > directory.size() || U32(directory.data() + at) != CentralHeaderSignature)
            {
                return std::unexpected(name + ": its directory is damaged at entry " + std::to_string(i));
            }
            const std::uint8_t *h = directory.data() + at;
            ZipEntry entry;
            entry.flags = U16(h + 8);
            entry.method = U16(h + 10);
            entry.modified = DosTimeToUnixMilliseconds(U16(h + 12), U16(h + 14));
            entry.crc = U32(h + 16);
            entry.compressedSize = U32(h + 20);
            entry.size = U32(h + 24);
            const std::size_t nameLength = U16(h + 28);
            const std::size_t extraLength = U16(h + 30);
            const std::size_t commentLength = U16(h + 32);
            entry.localHeaderOffset = U32(h + 42);
            if (at + 46 + nameLength + extraLength + commentLength > directory.size())
            {
                return std::unexpected(name + ": its directory is damaged at entry " + std::to_string(i));
            }
            const std::string_view rawName(reinterpret_cast<const char *>(h + 46), nameLength);
            // The ZIP64 extra field holds, in order, whichever of the three values are 0xffffffff above
            for (std::size_t e = 0; e + 4 <= extraLength;)
            {
                const std::uint8_t *field = h + 46 + nameLength + e;
                const std::size_t fieldSize = U16(field + 2);
                if (e + 4 + fieldSize > extraLength)
                {
                    break;
                }
                if (U16(field) == 0x0001)
                {
                    std::size_t f = 4;
                    const auto take = [&](std::uint64_t &value) {
                        if (value == 0xffffffff && f + 8 <= 4 + fieldSize)
                        {
                            value = U64(field + f);
                            f += 8;
                        }
                    };
                    take(entry.size);
                    take(entry.compressedSize);
                    take(entry.localHeaderOffset);
                }
                e += 4 + fieldSize;
            }
            at += 46 + nameLength + extraLength + commentLength;

            if (rawName.ends_with('/') && entry.size == 0)
            {
                continue; // a folder entry: folders are implied by the files' names
            }
            const auto normal = NormalizeEntryName(rawName);
            if (!normal)
            {
                return std::unexpected(name + " holds an entry whose name could leave the pack: " + std::string(rawName));
            }
            if (entry.size > MaxEntrySize)
            {
                return std::unexpected(name + ": " + *normal + " is larger than a pack entry may be");
            }
            if (entry.localHeaderOffset >= fileSize)
            {
                return std::unexpected(name + ": " + *normal + " starts past the end of the pack");
            }
            if (!entries.emplace(*normal, entry).second)
            {
                return std::unexpected(name + " holds " + *normal + " twice");
            }
        }
        return std::make_unique<ZipMount>(path, fileSize, std::move(entries));
    }
}
