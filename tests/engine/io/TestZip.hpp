#pragma once

// A stored-only zip writer for the pack tests (docs/packs.html): whatever names, sizes and CRCs a test wants, including
// wrong ones. The engine reads deflate too; the deflate cases use committed packs (tests/assets/packs).
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace N2Engine::Tests
{
    inline std::uint32_t Crc32(const std::string &data)
    {
        std::uint32_t crc = 0xffffffffu;
        for (const unsigned char c : data)
        {
            crc ^= c;
            for (int k = 0; k < 8; ++k)
            {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
        }
        return ~crc;
    }

    struct ZipEntry
    {
        std::string name;
        std::string data;
        std::uint32_t crcOverride = 0;            ///< non-zero: the CRC written instead of the real one
        std::uint32_t declaredSize = 0xffffffffu; ///< not 0xffffffff: the size written instead of the real one
    };

    namespace Detail
    {
        inline void Put16(std::string &out, const std::uint32_t v)
        {
            out += static_cast<char>(v & 0xff);
            out += static_cast<char>((v >> 8) & 0xff);
        }
        inline void Put32(std::string &out, const std::uint32_t v)
        {
            Put16(out, v & 0xffff);
            Put16(out, v >> 16);
        }
    }

    /// The bytes of a stored zip holding `entries`
    inline std::string MakeZip(const std::vector<ZipEntry> &entries)
    {
        using Detail::Put16;
        using Detail::Put32;
        std::string out;
        std::string directory;
        for (const ZipEntry &e : entries)
        {
            const auto offset = static_cast<std::uint32_t>(out.size());
            const std::uint32_t crc = e.crcOverride ? e.crcOverride : Crc32(e.data);
            const std::uint32_t size = e.declaredSize != 0xffffffffu ? e.declaredSize : static_cast<std::uint32_t>(e.data.size());
            Put32(out, 0x04034b50);
            Put16(out, 20);
            Put16(out, 0x0800); // UTF-8 names
            Put16(out, 0);      // stored
            Put16(out, 0);
            Put16(out, 0);
            Put32(out, crc);
            Put32(out, size);
            Put32(out, size);
            Put16(out, static_cast<std::uint32_t>(e.name.size()));
            Put16(out, 0);
            out += e.name;
            out += e.data;

            Put32(directory, 0x02014b50);
            Put16(directory, 20);
            Put16(directory, 20);
            Put16(directory, 0x0800);
            Put16(directory, 0);
            Put16(directory, 0);
            Put16(directory, 0);
            Put32(directory, crc);
            Put32(directory, size);
            Put32(directory, size);
            Put16(directory, static_cast<std::uint32_t>(e.name.size()));
            Put16(directory, 0);
            Put16(directory, 0);
            Put16(directory, 0);
            Put16(directory, 0);
            Put32(directory, 0);
            Put32(directory, offset);
            directory += e.name;
        }
        const auto directoryOffset = static_cast<std::uint32_t>(out.size());
        out += directory;
        Put32(out, 0x06054b50);
        Put16(out, 0);
        Put16(out, 0);
        Put16(out, static_cast<std::uint32_t>(entries.size()));
        Put16(out, static_cast<std::uint32_t>(entries.size()));
        Put32(out, static_cast<std::uint32_t>(directory.size()));
        Put32(out, directoryOffset);
        Put16(out, 0);
        return out;
    }

    /// Every file under `folder`'s subfolders `parts` (and the files named in `files`) as zip entries, '/'-separated
    inline std::vector<ZipEntry> FolderEntries(const std::filesystem::path &folder, const std::vector<std::string> &subfolders,
                                               const std::vector<std::string> &files)
    {
        std::vector<ZipEntry> entries;
        const auto add = [&](const std::filesystem::path &path) {
            std::ifstream in(path, std::ios::binary);
            ZipEntry entry;
            const std::u8string name = path.lexically_relative(folder).generic_u8string();
            entry.name.assign(name.begin(), name.end());
            entry.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            entries.push_back(std::move(entry));
        };
        for (const std::string &file : files)
        {
            if (std::filesystem::is_regular_file(folder / file))
            {
                add(folder / file);
            }
        }
        for (const std::string &sub : subfolders)
        {
            if (!std::filesystem::is_directory(folder / sub))
            {
                continue;
            }
            for (const auto &entry : std::filesystem::recursive_directory_iterator(folder / sub))
            {
                if (entry.is_regular_file())
                {
                    add(entry.path());
                }
            }
        }
        return entries;
    }
}
