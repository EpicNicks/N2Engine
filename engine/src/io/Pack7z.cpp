// 7z packs (docs/packs.html), read with the LZMA SDK's decoder. The cook step writes them non-solid (7z -ms=off): each file
// is its own block, so reading one file decodes only that file. A solid pack still reads (its block is decoded once and kept
// until another block is asked for), only more slowly and with more memory. The SDK checks every file's CRC.
#include <cstdio>
#include <map>
#include <mutex>
#include <ranges>
#include <set>

#include "engine/io/ResourcePath.hpp"
#include "engine/io/Vfs.hpp"

extern "C"
{
#include <7z.h>
#include <7zAlloc.h>
#include <7zCrc.h>
}

namespace N2Engine::IO
{
    namespace
    {
        const ISzAlloc AllocMain = {SzAlloc, SzFree};
        const ISzAlloc AllocTemp = {SzAllocTemp, SzFreeTemp};
        constexpr std::size_t LookBufferSize = 1 << 16;

        std::FILE *OpenRead(const std::filesystem::path &path)
        {
#ifdef _WIN32
            return _wfopen(path.c_str(), L"rb");
#else
            return std::fopen(path.c_str(), "rb");
#endif
        }

        /// The SDK's seekable stream over a FILE*; vt first, so the SDK's pointer is this object's
        struct FileStream
        {
            ISeekInStream vt{};
            std::FILE *file = nullptr;

            static SRes Read(ISeekInStreamPtr p, void *buffer, size_t *size)
            {
                auto *self = reinterpret_cast<FileStream *>(const_cast<ISeekInStream *>(p));
                if (*size == 0)
                {
                    return SZ_OK;
                }
                *size = std::fread(buffer, 1, *size, self->file);
                return std::ferror(self->file) ? SZ_ERROR_READ : SZ_OK;
            }

            static SRes Seek(ISeekInStreamPtr p, Int64 *position, ESzSeek origin)
            {
                auto *self = reinterpret_cast<FileStream *>(const_cast<ISeekInStream *>(p));
                const int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
                if (_fseeki64(self->file, *position, whence) != 0)
                {
                    return SZ_ERROR_READ;
                }
                *position = _ftelli64(self->file);
#else
                if (fseeko(self->file, static_cast<off_t>(*position), whence) != 0)
                {
                    return SZ_ERROR_READ;
                }
                *position = static_cast<Int64>(ftello(self->file));
#endif
                return SZ_OK;
            }
        };

        std::string Utf16ToUtf8(const std::vector<UInt16> &text)
        {
            std::string out;
            for (std::size_t i = 0; i < text.size() && text[i] != 0; ++i)
            {
                std::uint32_t c = text[i];
                if (c >= 0xd800 && c < 0xdc00 && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000)
                {
                    c = 0x10000 + ((c - 0xd800) << 10) + (text[i + 1] - 0xdc00);
                    ++i;
                }
                else if (c >= 0xd800 && c < 0xe000)
                {
                    c = 0xfffd; // a lone surrogate
                }
                if (c < 0x80)
                {
                    out += static_cast<char>(c);
                }
                else if (c < 0x800)
                {
                    out += static_cast<char>(0xc0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3f));
                }
                else if (c < 0x10000)
                {
                    out += static_cast<char>(0xe0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (c & 0x3f));
                }
                else
                {
                    out += static_cast<char>(0xf0 | (c >> 18));
                    out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (c & 0x3f));
                }
            }
            return out;
        }

        std::string SdkError(const SRes result)
        {
            switch (result)
            {
            case SZ_ERROR_DATA: return "its data is damaged";
            case SZ_ERROR_MEM: return "out of memory";
            case SZ_ERROR_CRC: return "a CRC does not match (the pack is damaged)";
            case SZ_ERROR_UNSUPPORTED: return "it uses a method this reader does not have (a pack is LZMA or LZMA2)";
            case SZ_ERROR_INPUT_EOF: return "it is cut short";
            case SZ_ERROR_ARCHIVE: return "its headers are damaged";
            case SZ_ERROR_NO_ARCHIVE: return "it is not a 7z archive";
            case SZ_ERROR_READ: return "it could not be read";
            default: return "error " + std::to_string(result);
            }
        }

        struct SevenZipFile
        {
            UInt32 index = 0;
            std::uint64_t size = 0;
            std::uint64_t modified = 0;
        };

        class SevenZipMount final : public IMount
        {
        public:
            SevenZipMount() = default;
            SevenZipMount(const SevenZipMount &) = delete;
            SevenZipMount &operator=(const SevenZipMount &) = delete;

            ~SevenZipMount() override
            {
                if (_opened)
                {
                    SzArEx_Free(&_db, &AllocMain);
                }
                if (_outBuffer)
                {
                    ISzAlloc_Free(&AllocMain, _outBuffer);
                }
                if (_look.buf)
                {
                    ISzAlloc_Free(&AllocMain, _look.buf);
                }
                if (_stream.file)
                {
                    std::fclose(_stream.file);
                }
            }

            /// Opens the archive and indexes its names; an error says why
            std::expected<void, std::string> Open(const std::filesystem::path &path)
            {
                _path = path;
                const std::string name = PathToUtf8(path);
                _stream.file = OpenRead(path);
                if (!_stream.file)
                {
                    return std::unexpected("Couldn't open the pack " + name);
                }
                _stream.vt.Read = FileStream::Read;
                _stream.vt.Seek = FileStream::Seek;
                LookToRead2_CreateVTable(&_look, False);
                _look.buf = static_cast<Byte *>(ISzAlloc_Alloc(&AllocMain, LookBufferSize));
                if (!_look.buf)
                {
                    return std::unexpected("Out of memory opening " + name);
                }
                _look.bufSize = LookBufferSize;
                _look.realStream = &_stream.vt;
                LookToRead2_INIT(&_look);

                static std::once_flag crcTable;
                std::call_once(crcTable, [] { CrcGenerateTable(); });
                SzArEx_Init(&_db);
                const SRes result = SzArEx_Open(&_db, &_look.vt, &AllocMain, &AllocTemp);
                if (result != SZ_OK)
                {
                    return std::unexpected(name + ": " + SdkError(result));
                }
                _opened = true;
                if (_db.NumFiles > Pack::MaxEntries)
                {
                    return std::unexpected(name + " holds more than " + std::to_string(Pack::MaxEntries) + " entries");
                }

                std::map<UInt32, int> filesPerBlock;
                std::vector<UInt16> utf16;
                for (UInt32 i = 0; i < _db.NumFiles; ++i)
                {
                    utf16.assign(SzArEx_GetFileNameUtf16(&_db, i, nullptr), 0);
                    SzArEx_GetFileNameUtf16(&_db, i, utf16.data());
                    const std::string raw = Utf16ToUtf8(utf16);
                    if (SzArEx_IsDir(&_db, i))
                    {
                        if (const auto folder = Pack::NormalizeEntryName(raw))
                        {
                            _folders.insert(*folder);
                            continue;
                        }
                        return std::unexpected(name + " holds a folder whose name could leave the pack: " + raw);
                    }
                    const auto normal = Pack::NormalizeEntryName(raw);
                    if (!normal)
                    {
                        return std::unexpected(name + " holds an entry whose name could leave the pack: " + raw);
                    }
                    SevenZipFile file;
                    file.index = i;
                    file.size = SzArEx_GetFileSize(&_db, i);
                    if (file.size > Pack::MaxEntrySize)
                    {
                        return std::unexpected(name + ": " + *normal + " is larger than a pack entry may be");
                    }
                    if (SzBitWithVals_Check(&_db.MTime, i))
                    {
                        const CNtfsFileTime t = _db.MTime.Vals[i];
                        const std::uint64_t ticks = (static_cast<std::uint64_t>(t.High) << 32) | t.Low;
                        constexpr std::uint64_t UnixEpochTicks = 116444736000000000ull;
                        file.modified = ticks > UnixEpochTicks ? (ticks - UnixEpochTicks) / 10000u : 0u;
                    }
                    if (_db.FileToFolder[i] != static_cast<UInt32>(-1))
                    {
                        ++filesPerBlock[_db.FileToFolder[i]];
                    }
                    if (!_files.emplace(*normal, file).second)
                    {
                        return std::unexpected(name + " holds " + *normal + " twice");
                    }
                    for (std::size_t slash = normal->find('/'); slash != std::string::npos; slash = normal->find('/', slash + 1))
                    {
                        _folders.insert(normal->substr(0, slash));
                    }
                }
                for (const auto &count : filesPerBlock | std::views::values)
                {
                    _solid = _solid || count > 1;
                }
                return {};
            }

            /// Whether some block holds more than one file (a solid pack: slower random reads)
            [[nodiscard]] bool IsSolid() const { return _solid; }

            std::string_view Kind() const override { return "7z"; }

            std::optional<VfsEntry> Stat(const std::string_view path) const override
            {
                if (path.empty() || _folders.contains(std::string(path)))
                {
                    return VfsEntry{std::string(path), 0, 0, true};
                }
                const auto it = _files.find(std::string(path));
                if (it == _files.end())
                {
                    return std::nullopt;
                }
                return VfsEntry{it->first, it->second.size, it->second.modified, false};
            }

            std::vector<VfsEntry> List(const std::string_view directory, const bool recursive) const override
            {
                std::vector<VfsEntry> out;
                const std::string prefix = directory.empty() ? std::string() : std::string(directory) + "/";
                const auto direct = [&](const std::string &n) {
                    return n.starts_with(prefix) && (recursive || n.find('/', prefix.size()) == std::string::npos);
                };
                for (const auto &folder : _folders)
                {
                    if (direct(folder))
                    {
                        out.push_back({folder, 0, 0, true});
                    }
                }
                for (const auto &[n, file] : _files)
                {
                    if (direct(n))
                    {
                        out.push_back({n, file.size, file.modified, false});
                    }
                }
                return out;
            }

            std::expected<VfsBytes, std::string> Read(const std::string_view path) const override
            {
                const auto it = _files.find(std::string(path));
                if (it == _files.end())
                {
                    return std::unexpected(std::string(path) + " is not in the pack");
                }
                const SevenZipFile &file = it->second;
                if (file.size == 0)
                {
                    return VfsBytes{};
                }
                std::lock_guard lock(_readMutex);
                std::size_t offset = 0;
                std::size_t processed = 0;
                const SRes result = SzArEx_Extract(&_db, &_look.vt, file.index, &_blockIndex, &_outBuffer, &_outBufferSize,
                                                   &offset, &processed, &AllocMain, &AllocTemp);
                if (result != SZ_OK)
                {
                    return std::unexpected(std::string(path) + " in " + PathToUtf8(_path) + ": " + SdkError(result));
                }
                VfsBytes bytes(_outBuffer + offset, _outBuffer + offset + processed);
                if (!_solid)
                {
                    // A non-solid block is this one file: keeping it would only hold memory
                    ISzAlloc_Free(&AllocMain, _outBuffer);
                    _outBuffer = nullptr;
                    _outBufferSize = 0;
                    _blockIndex = static_cast<UInt32>(-1);
                }
                return bytes;
            }

        private:
            std::filesystem::path _path;
            FileStream _stream;
            mutable CLookToRead2 _look{};
            mutable CSzArEx _db{};
            bool _opened = false;
            bool _solid = false;
            std::map<std::string, SevenZipFile> _files;
            std::set<std::string> _folders;
            mutable std::mutex _readMutex;
            /// The SDK's cache of the last decoded block
            mutable UInt32 _blockIndex = static_cast<UInt32>(-1);
            mutable Byte *_outBuffer = nullptr;
            mutable std::size_t _outBufferSize = 0;
        };
    }

    std::expected<std::unique_ptr<IMount>, std::string> Pack::OpenSevenZip(const std::filesystem::path &file)
    {
        auto mount = std::make_unique<SevenZipMount>();
        if (auto opened = mount->Open(file); !opened)
        {
            return std::unexpected(std::move(opened.error()));
        }
        return mount;
    }
}
