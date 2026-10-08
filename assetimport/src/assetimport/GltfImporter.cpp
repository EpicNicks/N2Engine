#include "assetimport/GltfImporter.hpp"
#include "assetimport/TangentGeneration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cgltf/cgltf.h>

namespace N2Engine::AssetImport
{
    namespace
    {
        namespace fs = std::filesystem;

        using ImportResult = std::expected<ImportedScene, ModelImportError>;

        constexpr std::uint32_t kGlbMagic = 0x46546C67;      // "glTF"
        constexpr std::uint32_t kGlbJsonChunk = 0x4E4F534A;  // "JSON"
        constexpr std::uint32_t kGlbBinChunk = 0x004E4942;   // "BIN\0"
        constexpr std::size_t kGlbHeaderBytes = 12;
        constexpr std::size_t kGlbChunkHeaderBytes = 8;

        std::unexpected<ModelImportError> Fail(const ModelImportErrorCode code, std::string message)
        {
            return std::unexpected(ModelImportError{code, std::move(message)});
        }

        struct CgltfFree
        {
            void operator()(cgltf_data *data) const { cgltf_free(data); }
        };
        using CgltfData = std::unique_ptr<cgltf_data, CgltfFree>;

        /**
         * The memory cgltf may allocate, counted: while parsing it is capped at kMaxParseMemoryBytes (a small JSON
         * can declare millions of empty objects, each allocated in full); afterwards the cap is lifted for the
         * buffers this importer allocates through it (capped on their own). Every block carries its size in a
         * 16-byte header, since cgltf's free callback isn't told the size.
         */
        struct ParseAllocator
        {
            std::size_t used = 0;
            std::size_t limit = kMaxParseMemoryBytes;
        };

        constexpr std::size_t kAllocationHeader = 16;

        void *CountingAlloc(void *user, const cgltf_size size)
        {
            auto &state = *static_cast<ParseAllocator *>(user);
            if (size > state.limit || state.used > state.limit - size ||
                size > std::numeric_limits<std::size_t>::max() - kAllocationHeader)
            {
                return nullptr;
            }
            auto *base = static_cast<unsigned char *>(std::malloc(size + kAllocationHeader));
            if (!base)
            {
                return nullptr;
            }
            const std::size_t recorded = size;
            std::memcpy(base, &recorded, sizeof recorded);
            state.used += size;
            return base + kAllocationHeader;
        }

        void CountingFree(void *user, void *pointer)
        {
            if (!pointer)
            {
                return;
            }
            auto &state = *static_cast<ParseAllocator *>(user);
            unsigned char *base = static_cast<unsigned char *>(pointer) - kAllocationHeader;
            std::size_t size = 0;
            std::memcpy(&size, base, sizeof size);
            state.used -= std::min(size, state.used);
            std::free(base);
        }

        /// Each warning is kept once, in the order first raised
        class Warnings
        {
        public:
            explicit Warnings(std::vector<std::string> &out) : _out(out) {}

            void Add(std::string message)
            {
                if (_seen.insert(message).second)
                {
                    _out.push_back(std::move(message));
                }
            }

        private:
            std::vector<std::string> &_out;
            std::set<std::string> _seen;
        };

        std::uint32_t ReadU32(const std::uint8_t *bytes)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes, sizeof value); // GLB is little-endian, as every supported target is
            return value;
        }

        std::string ResultName(const cgltf_result result)
        {
            switch (result)
            {
            case cgltf_result_success: return "success";
            case cgltf_result_data_too_short: return "the data is too short (truncated?)";
            case cgltf_result_unknown_format: return "not glTF or GLB";
            case cgltf_result_invalid_json: return "invalid JSON";
            case cgltf_result_invalid_gltf: return "invalid glTF";
            case cgltf_result_invalid_options: return "invalid options";
            case cgltf_result_file_not_found: return "file not found";
            case cgltf_result_io_error: return "I/O error";
            case cgltf_result_out_of_memory: return "out of memory";
            case cgltf_result_legacy_gltf: return "glTF 1.0, which isn't supported (only 2.0)";
            default: return "unknown error";
            }
        }

        std::string NameOr(const char *name, const std::string_view kind, const std::size_t index)
        {
            if (name && *name)
            {
                return name;
            }
            return std::format("{} {}", kind, index);
        }

        /// The GLB container checked before cgltf sees it: the version, every chunk inside the file, and the caps
        std::optional<ModelImportError> CheckGlb(std::span<const std::uint8_t> &bytes)
        {
            if (bytes.size() < kGlbHeaderBytes)
            {
                return ModelImportError{ModelImportErrorCode::ParseFailed, "the GLB header is truncated"};
            }
            const std::uint32_t version = ReadU32(bytes.data() + 4);
            if (version != 2)
            {
                return ModelImportError{ModelImportErrorCode::ParseFailed,
                                        std::format("GLB version {} isn't supported (only 2)", version)};
            }
            const std::size_t length = ReadU32(bytes.data() + 8);
            if (length > bytes.size())
            {
                return ModelImportError{ModelImportErrorCode::ParseFailed,
                                        std::format("the GLB says it is {} bytes but only {} are there (truncated)",
                                                    length, bytes.size())};
            }
            if (length < kGlbHeaderBytes + kGlbChunkHeaderBytes)
            {
                return ModelImportError{ModelImportErrorCode::ParseFailed, "the GLB has no JSON chunk"};
            }
            bytes = bytes.first(length); // anything after the declared length isn't part of the file

            const std::size_t jsonLength = ReadU32(bytes.data() + kGlbHeaderBytes);
            const std::uint32_t jsonType = ReadU32(bytes.data() + kGlbHeaderBytes + 4);
            if (jsonType != kGlbJsonChunk)
            {
                return ModelImportError{ModelImportErrorCode::ParseFailed, "the GLB's first chunk isn't JSON"};
            }
            const std::size_t afterJsonHeader = bytes.size() - kGlbHeaderBytes - kGlbChunkHeaderBytes;
            if (jsonLength > afterJsonHeader)
            {
                return ModelImportError{ModelImportErrorCode::ParseFailed, "the GLB's JSON chunk runs past the file"};
            }
            if (jsonLength > kMaxModelJsonBytes)
            {
                return ModelImportError{ModelImportErrorCode::TooLarge,
                                        std::format("the GLB's JSON chunk is {} bytes, over the {}-byte limit",
                                                    jsonLength, kMaxModelJsonBytes)};
            }
            const std::size_t rest = afterJsonHeader - jsonLength;
            if (rest >= kGlbChunkHeaderBytes)
            {
                const std::uint8_t *binHeader = bytes.data() + kGlbHeaderBytes + kGlbChunkHeaderBytes + jsonLength;
                const std::size_t binLength = ReadU32(binHeader);
                if (binLength > rest - kGlbChunkHeaderBytes)
                {
                    return ModelImportError{ModelImportErrorCode::ParseFailed, "the GLB's BIN chunk runs past the file"};
                }
                if (ReadU32(binHeader + 4) != kGlbBinChunk)
                {
                    return ModelImportError{ModelImportErrorCode::ParseFailed, "the GLB's second chunk isn't BIN"};
                }
                if (binLength > kMaxModelBufferBytes)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("the GLB's BIN chunk is {} bytes, over the {}-byte limit",
                                                        binLength, kMaxModelBufferBytes)};
                }
            }
            return std::nullopt;
        }

        /// A path as UTF-8 text for messages (path::string() can throw for characters the ANSI code page lacks)
        std::string PathText(const fs::path &path)
        {
            const std::u8string utf8 = path.u8string();
            std::string text;
            text.reserve(utf8.size());
            for (const char8_t c : utf8)
            {
                text.push_back(static_cast<char>(c));
            }
            return text;
        }

        /// Reads up to `maxBytes` of a file, or nullopt (with the reason in `error`)
        std::optional<std::vector<std::uint8_t>> ReadFileCapped(const fs::path &path, const std::size_t wanted,
                                                                const std::size_t maxBytes, std::string &error)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                error = std::format("can't open {}", PathText(path));
                return std::nullopt;
            }
            const std::streamoff end = file.tellg();
            if (end < 0)
            {
                error = std::format("can't read {}", PathText(path));
                return std::nullopt;
            }
            const auto size = static_cast<std::size_t>(end);
            // wanted == 0: the whole file
            const std::size_t count = wanted == 0 ? size : wanted;
            if (count > maxBytes)
            {
                error = std::format("{} is {} bytes, over the {}-byte limit", PathText(path), count, maxBytes);
                return std::nullopt;
            }
            if (size < count)
            {
                error = std::format("{} is {} bytes, shorter than the {} the model says", PathText(path), size, count);
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(count);
            file.seekg(0);
            if (count > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(count)))
            {
                error = std::format("can't read {}", PathText(path));
                return std::nullopt;
            }
            return bytes;
        }

        /// Standard base64 (padding optional, no whitespace)
        std::optional<std::vector<std::uint8_t>> DecodeBase64(const std::string_view text, const std::size_t maxBytes)
        {
            std::string_view body = text;
            while (!body.empty() && body.back() == '=')
            {
                body.remove_suffix(1);
            }
            if (body.size() / 4 * 3 > maxBytes)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> out;
            out.reserve(body.size() / 4 * 3 + 3);
            std::uint32_t buffer = 0;
            int bits = 0;
            for (const char ch : body)
            {
                int value = -1;
                if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
                else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
                else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
                else if (ch == '+' || ch == '-') value = 62;
                else if (ch == '/' || ch == '_') value = 63;
                if (value < 0)
                {
                    return std::nullopt;
                }
                buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
                }
            }
            return out;
        }

        /// The bytes after "data:...;base64," of a data URI, or nullopt when it isn't a base64 data URI
        std::optional<std::string_view> Base64Payload(const std::string_view uri)
        {
            if (!uri.starts_with("data:"))
            {
                return std::nullopt;
            }
            const std::size_t comma = uri.find(',');
            if (comma == std::string_view::npos || !uri.substr(0, comma).ends_with(";base64"))
            {
                return std::nullopt;
            }
            return uri.substr(comma + 1);
        }

        // ===== Buffers =====

        /// Gives every buffer its data: the GLB BIN chunk, a base64 data: URI, or an external file under
        /// baseDirectory. cgltf_load_buffers isn't used: it would open any path the file names.
        std::optional<ModelImportError> LoadBuffers(cgltf_data &data, const fs::path &baseDirectory)
        {
            std::size_t total = 0;
            for (cgltf_size i = 0; i < data.buffers_count; ++i)
            {
                cgltf_buffer &buffer = data.buffers[i];
                if (buffer.data || buffer.size == 0)
                {
                    continue;
                }
                if (buffer.size > kMaxModelBufferBytes)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("buffer {} is {} bytes, over the {}-byte limit", i, buffer.size,
                                                        kMaxModelBufferBytes)};
                }
                total += buffer.size;
                if (total > kMaxModelTotalBufferBytes)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("the buffers come to over the {}-byte limit",
                                                        kMaxModelTotalBufferBytes)};
                }

                if (!buffer.uri)
                {
                    // Only a GLB's first buffer may have no URI: it is the BIN chunk
                    if (i == 0 && data.bin)
                    {
                        if (data.bin_size < buffer.size)
                        {
                            return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                                    std::format("the GLB's BIN chunk is {} bytes, shorter than buffer 0's {}",
                                                                data.bin_size, buffer.size)};
                        }
                        buffer.data = const_cast<void *>(data.bin);
                        buffer.data_free_method = cgltf_data_free_method_none;
                        continue;
                    }
                    return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                            std::format("buffer {} has no data (no URI and no GLB BIN chunk)", i)};
                }

                const std::string_view uri(buffer.uri);
                std::vector<std::uint8_t> bytes;
                if (uri.starts_with("data:"))
                {
                    const auto payload = Base64Payload(uri);
                    if (!payload)
                    {
                        return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                                std::format("buffer {}'s data: URI isn't base64", i)};
                    }
                    auto decoded = DecodeBase64(*payload, kMaxModelBufferBytes);
                    if (!decoded || decoded->size() < buffer.size)
                    {
                        return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                                std::format("buffer {}'s data: URI is malformed or shorter than its {} bytes",
                                                            i, buffer.size)};
                    }
                    bytes = std::move(*decoded);
                }
                else
                {
                    const std::string decodedUri = PercentDecode(uri);
                    if (baseDirectory.empty())
                    {
                        return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                                std::format("buffer {} is in another file ('{}') but the model has no folder",
                                                            i, decodedUri)};
                    }
                    const fs::path path = ResolveModelUri(baseDirectory, decodedUri);
                    if (path.empty())
                    {
                        return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                                std::format("buffer {}'s URI '{}' is rejected: only relative paths inside "
                                                            "the model's folder are read", i, decodedUri)};
                    }
                    std::string error;
                    auto read = ReadFileCapped(path, buffer.size, kMaxModelBufferBytes, error);
                    if (!read)
                    {
                        return ModelImportError{ModelImportErrorCode::BufferLoadFailed,
                                                std::format("buffer {}: {}", i, error)};
                    }
                    bytes = std::move(*read);
                }

                // Through the data's own allocator, which cgltf_free releases it with (data_free_method_memory_free)
                void *copy = data.memory.alloc_func(data.memory.user_data, buffer.size);
                if (!copy)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("out of memory for buffer {} ({} bytes)", i, buffer.size)};
                }
                std::memcpy(copy, bytes.data(), buffer.size);
                buffer.data = copy;
                buffer.data_free_method = cgltf_data_free_method_memory_free;
            }
            return std::nullopt;
        }

        /// A buffer view's bytes, checked to lie inside its buffer's loaded data; empty with `ok` false otherwise
        std::span<const std::uint8_t> ViewBytes(const cgltf_buffer_view *view, bool &ok)
        {
            ok = false;
            if (!view || !view->buffer || !view->buffer->data)
            {
                return {};
            }
            const cgltf_buffer &buffer = *view->buffer;
            if (view->offset > buffer.size || view->size > buffer.size - view->offset)
            {
                return {};
            }
            ok = true;
            return {static_cast<const std::uint8_t *>(buffer.data) + view->offset, view->size};
        }

        // ===== Accessors =====

        float ReadComponent(const std::uint8_t *p, const cgltf_component_type type, const bool normalized)
        {
            switch (type)
            {
            case cgltf_component_type_r_8:
            {
                std::int8_t v = 0;
                std::memcpy(&v, p, 1);
                return normalized ? std::max(static_cast<float>(v) / 127.0f, -1.0f) : static_cast<float>(v);
            }
            case cgltf_component_type_r_8u:
                return normalized ? static_cast<float>(*p) / 255.0f : static_cast<float>(*p);
            case cgltf_component_type_r_16:
            {
                std::int16_t v = 0;
                std::memcpy(&v, p, 2);
                return normalized ? std::max(static_cast<float>(v) / 32767.0f, -1.0f) : static_cast<float>(v);
            }
            case cgltf_component_type_r_16u:
            {
                std::uint16_t v = 0;
                std::memcpy(&v, p, 2);
                return normalized ? static_cast<float>(v) / 65535.0f : static_cast<float>(v);
            }
            case cgltf_component_type_r_32u:
            {
                std::uint32_t v = 0;
                std::memcpy(&v, p, 4);
                return static_cast<float>(v);
            }
            case cgltf_component_type_r_32f:
            {
                float v = 0.0f;
                std::memcpy(&v, p, 4);
                return v;
            }
            default:
                return 0.0f;
            }
        }

        std::uint32_t ReadIndexComponent(const std::uint8_t *p, const cgltf_component_type type)
        {
            switch (type)
            {
            case cgltf_component_type_r_8u:
                return *p;
            case cgltf_component_type_r_16u:
            {
                std::uint16_t v = 0;
                std::memcpy(&v, p, 2);
                return v;
            }
            case cgltf_component_type_r_32u:
            {
                std::uint32_t v = 0;
                std::memcpy(&v, p, 4);
                return v;
            }
            default:
                return std::numeric_limits<std::uint32_t>::max();
            }
        }

        bool IsIndexType(const cgltf_component_type type)
        {
            return type == cgltf_component_type_r_8u || type == cgltf_component_type_r_16u ||
                   type == cgltf_component_type_r_32u;
        }

        /// Whether `count` elements of `elementSize` bytes, `stride` apart from `offset`, fit in `available` bytes
        bool Fits(const std::size_t offset, const std::size_t stride, const std::size_t count,
                  const std::size_t elementSize, const std::size_t available)
        {
            if (count == 0)
            {
                return offset <= available;
            }
            if (offset > available || elementSize > available - offset)
            {
                return false;
            }
            const std::size_t room = available - offset - elementSize; // for the (count - 1) strides after the first
            return stride == 0 ? true : (count - 1) <= room / stride;
        }

        /**
         * Calls write(i, element) for every element (element is null for an accessor without a buffer view: all
         * zeros), then write(index, value) for every sparse substitution. Every byte read is checked to be inside
         * its buffer view first. An error message, or nullopt.
         */
        template <typename Write>
        std::optional<std::string> VisitAccessor(const cgltf_accessor &accessor, const std::size_t elementSize,
                                                  const std::string_view what, Write &&write)
        {
            if (accessor.count > kMaxAccessorElements)
            {
                return std::format("{} has {} elements, over the {} limit", what, accessor.count, kMaxAccessorElements);
            }
            if (accessor.buffer_view)
            {
                bool ok = false;
                const auto bytes = ViewBytes(accessor.buffer_view, ok);
                if (!ok)
                {
                    return std::format("{}'s buffer view is outside its buffer, or the buffer has no data", what);
                }
                const std::size_t stride = accessor.stride;
                if (stride < elementSize)
                {
                    return std::format("{} has a byte stride of {}, less than its {}-byte elements", what, stride,
                                       elementSize);
                }
                if (!Fits(accessor.offset, stride, accessor.count, elementSize, bytes.size()))
                {
                    return std::format("{} ({} elements of {} bytes, stride {}, offset {}) runs past its {}-byte buffer view",
                                       what, accessor.count, elementSize, stride, accessor.offset, bytes.size());
                }
                for (std::size_t i = 0; i < accessor.count; ++i)
                {
                    write(i, bytes.data() + accessor.offset + i * stride);
                }
            }
            else
            {
                for (std::size_t i = 0; i < accessor.count; ++i)
                {
                    write(i, static_cast<const std::uint8_t *>(nullptr));
                }
            }

            if (!accessor.is_sparse)
            {
                return std::nullopt;
            }
            const cgltf_accessor_sparse &sparse = accessor.sparse;
            if (sparse.count > accessor.count)
            {
                return std::format("{}'s sparse substitution has {} entries, more than its {} elements", what,
                                   sparse.count, accessor.count);
            }
            if (!IsIndexType(sparse.indices_component_type))
            {
                return std::format("{}'s sparse indices aren't unsigned 8, 16 or 32-bit", what);
            }
            const std::size_t indexSize = cgltf_component_size(sparse.indices_component_type);
            bool indicesOk = false;
            bool valuesOk = false;
            const auto indexBytes = ViewBytes(sparse.indices_buffer_view, indicesOk);
            const auto valueBytes = ViewBytes(sparse.values_buffer_view, valuesOk);
            if (!indicesOk || !valuesOk ||
                !Fits(sparse.indices_byte_offset, indexSize, sparse.count, indexSize, indexBytes.size()) ||
                !Fits(sparse.values_byte_offset, elementSize, sparse.count, elementSize, valueBytes.size()))
            {
                return std::format("{}'s sparse indices or values run past their buffer views", what);
            }
            for (std::size_t i = 0; i < sparse.count; ++i)
            {
                const std::uint32_t index =
                    ReadIndexComponent(indexBytes.data() + sparse.indices_byte_offset + i * indexSize,
                                       sparse.indices_component_type);
                if (index >= accessor.count)
                {
                    return std::format("{}'s sparse index {} is past its {} elements", what, index, accessor.count);
                }
                // Sparse values are tightly packed (the glTF rule), whatever the base view's stride
                write(index, valueBytes.data() + sparse.values_byte_offset + i * elementSize);
            }
            return std::nullopt;
        }

        /// Reads a float (or normalised integer) accessor with `components` components into out (count * components)
        std::optional<std::string> ReadFloats(const cgltf_accessor &accessor, const std::size_t components,
                                              const std::string_view what, std::vector<float> &out, bool &nonFinite)
        {
            if (cgltf_num_components(accessor.type) != components)
            {
                return std::format("{} has {} components per element, expected {}", what,
                                   cgltf_num_components(accessor.type), components);
            }
            const std::size_t componentSize = cgltf_component_size(accessor.component_type);
            if (componentSize == 0)
            {
                return std::format("{} has an invalid component type", what);
            }
            const std::size_t elementSize = componentSize * components;
            out.assign(accessor.count * components, 0.0f);
            const cgltf_component_type type = accessor.component_type;
            const bool normalized = accessor.normalized != 0;
            return VisitAccessor(accessor, elementSize, what,
                                 [&](const std::size_t i, const std::uint8_t *element)
                                 {
                                     for (std::size_t c = 0; c < components; ++c)
                                     {
                                         float value = element ? ReadComponent(element + c * componentSize, type, normalized)
                                                               : 0.0f;
                                         if (!std::isfinite(value))
                                         {
                                             value = 0.0f;
                                             nonFinite = true;
                                         }
                                         out[i * components + c] = value;
                                     }
                                 });
        }

        std::optional<std::string> ReadIndices(const cgltf_accessor &accessor, std::vector<std::uint32_t> &out)
        {
            if (accessor.type != cgltf_type_scalar || !IsIndexType(accessor.component_type))
            {
                return std::string("the indices aren't unsigned 8, 16 or 32-bit scalars");
            }
            const std::size_t size = cgltf_component_size(accessor.component_type);
            if (accessor.buffer_view && accessor.stride != size)
            {
                return std::format("the indices have a byte stride of {}; indices must be tightly packed ({})",
                                   accessor.stride, size);
            }
            out.assign(accessor.count, 0u);
            const cgltf_component_type type = accessor.component_type;
            return VisitAccessor(accessor, size, "the index accessor",
                                 [&](const std::size_t i, const std::uint8_t *element)
                                 {
                                     out[i] = element ? ReadIndexComponent(element, type) : 0u;
                                 });
        }

        /**
         * Every buffer view inside its buffer, and every accessor (and sparse substitution) inside its views, with
         * overflow-safe arithmetic. Runs before cgltf_validate, which reads sparse and index data before it checks
         * the views (and whose sums can wrap on huge offsets), so it never sees out-of-range data.
         */
        std::optional<ModelImportError> CheckRanges(const cgltf_data &data)
        {
            for (cgltf_size i = 0; i < data.buffer_views_count; ++i)
            {
                const cgltf_buffer_view &view = data.buffer_views[i];
                if (!view.buffer || view.offset > view.buffer->size || view.size > view.buffer->size - view.offset)
                {
                    return ModelImportError{ModelImportErrorCode::InvalidData,
                                            std::format("buffer view {} runs past its buffer", i)};
                }
            }
            for (cgltf_size i = 0; i < data.accessors_count; ++i)
            {
                const cgltf_accessor &accessor = data.accessors[i];
                if (accessor.count > kMaxAccessorElements)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("accessor {} has {} elements, over the {} limit", i,
                                                        accessor.count, kMaxAccessorElements)};
                }
                const std::size_t elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
                if (elementSize == 0)
                {
                    return ModelImportError{ModelImportErrorCode::InvalidData,
                                            std::format("accessor {} has an invalid type or component type", i)};
                }
                if (accessor.buffer_view &&
                    !Fits(accessor.offset, accessor.stride, accessor.count, elementSize, accessor.buffer_view->size))
                {
                    return ModelImportError{ModelImportErrorCode::InvalidData,
                                            std::format("accessor {} runs past its buffer view", i)};
                }
                if (accessor.is_sparse)
                {
                    const cgltf_accessor_sparse &sparse = accessor.sparse;
                    const std::size_t indexSize = cgltf_component_size(sparse.indices_component_type);
                    if (!IsIndexType(sparse.indices_component_type) || sparse.count > accessor.count ||
                        !sparse.indices_buffer_view || !sparse.values_buffer_view ||
                        !Fits(sparse.indices_byte_offset, indexSize, sparse.count, indexSize,
                              sparse.indices_buffer_view->size) ||
                        !Fits(sparse.values_byte_offset, elementSize, sparse.count, elementSize,
                              sparse.values_buffer_view->size))
                    {
                        return ModelImportError{ModelImportErrorCode::InvalidData,
                                                std::format("accessor {}'s sparse substitution is malformed or runs past "
                                                            "its buffer views", i)};
                    }
                }
            }
            return std::nullopt;
        }

        // ===== Meshes =====

        /// Triangle-list indices from a primitive's indices, by its mode (strips and fans become lists, as the
        /// glTF spec orders them, so the winding is kept). Degenerate strip/fan triangles are dropped.
        std::vector<std::uint32_t> Triangulate(const cgltf_primitive_type mode, const std::vector<std::uint32_t> &in)
        {
            std::vector<std::uint32_t> out;
            const auto add = [&out](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c)
            {
                if (a == b || b == c || a == c)
                {
                    return;
                }
                out.push_back(a);
                out.push_back(b);
                out.push_back(c);
            };
            switch (mode)
            {
            case cgltf_primitive_type_triangles:
                out.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(in.size() / 3 * 3));
                break;
            case cgltf_primitive_type_triangle_strip:
                // Triangle i is {v(i), v(i + 1 + i % 2), v(i + 2 - i % 2)}
                for (std::size_t i = 0; i + 2 < in.size(); ++i)
                {
                    if (i % 2 == 0)
                        add(in[i], in[i + 1], in[i + 2]);
                    else
                        add(in[i], in[i + 2], in[i + 1]);
                }
                break;
            case cgltf_primitive_type_triangle_fan:
                // Triangle i is {v(i + 1), v(i + 2), v(0)}
                for (std::size_t i = 0; i + 2 < in.size(); ++i)
                {
                    add(in[i + 1], in[i + 2], in[0]);
                }
                break;
            default:
                break;
            }
            return out;
        }

        std::array<float, 3> FaceNormal(const ImportedVertex &a, const ImportedVertex &b, const ImportedVertex &c)
        {
            const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
            const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
            std::array<float, 3> n = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                      e1[0] * e2[1] - e1[1] * e2[0]};
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 1e-20f) || !std::isfinite(length))
            {
                return {0.0f, 0.0f, 1.0f};
            }
            return {n[0] / length, n[1] / length, n[2] / length};
        }

        /// What the whole model has read so far, against kMaxSceneVertices and kMaxSceneIndices. Charged before
        /// the memory is allocated, so a small file whose accessors are reused (or have no buffer view) can't ask
        /// for gigabytes.
        struct Budget
        {
            std::size_t vertices = 0;
            std::size_t indices = 0;

            bool AddVertices(const std::size_t count)
            {
                if (count > kMaxSceneVertices - vertices)
                    return false;
                vertices += count;
                return true;
            }

            bool AddIndices(const std::size_t count)
            {
                if (count > kMaxSceneIndices - indices)
                    return false;
                indices += count;
                return true;
            }
        };

        struct MeshContext
        {
            const cgltf_data &data;
            const ModelImportSettings &settings;
            float scale = 1.0f;
            Warnings &warnings;
            Budget &budget;
        };

        ModelImportError OverBudget(const std::string_view what)
        {
            return ModelImportError{ModelImportErrorCode::TooLarge,
                                    std::format("the model has too many {} in all (over {} vertices or {} indices)",
                                                what, kMaxSceneVertices, kMaxSceneIndices)};
        }

        /// Vertices a mesh already holds for one set of attribute accessors: primitives of the same mesh that share
        /// their attributes (one vertex buffer, several index lists, as exporters often write) share the vertices
        struct SharedVertices
        {
            const cgltf_accessor *positions = nullptr;
            const cgltf_accessor *normals = nullptr;
            const cgltf_accessor *texCoords = nullptr;
            const cgltf_accessor *colors = nullptr;
            const cgltf_accessor *tangents = nullptr;
            std::size_t base = 0;
        };

        std::optional<ModelImportError> ImportPrimitive(const MeshContext &context, std::vector<SharedVertices> &shared,
                                                        const cgltf_primitive &primitive,
                                                        const std::string &meshName, const std::size_t primitiveIndex,
                                                        ImportedMesh &mesh)
        {
            const std::string where = std::format("mesh '{}' primitive {}", meshName, primitiveIndex);
            const auto invalid = [&where](const std::string &message)
            {
                return ModelImportError{ModelImportErrorCode::InvalidData, std::format("{}: {}", where, message)};
            };

            if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip &&
                primitive.type != cgltf_primitive_type_triangle_fan)
            {
                context.warnings.Add("points and lines are not imported (only triangles, strips and fans)");
                return std::nullopt;
            }
            if (primitive.targets_count > 0)
            {
                context.warnings.Add("morph targets are ignored");
            }

            const cgltf_accessor *positions = nullptr;
            const cgltf_accessor *normals = nullptr;
            const cgltf_accessor *texCoords = nullptr;
            const cgltf_accessor *colors = nullptr;
            const cgltf_accessor *tangents = nullptr;
            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
            {
                const cgltf_attribute &attribute = primitive.attributes[i];
                switch (attribute.type)
                {
                case cgltf_attribute_type_position:
                    if (attribute.index == 0) positions = attribute.data;
                    break;
                case cgltf_attribute_type_normal:
                    if (attribute.index == 0) normals = attribute.data;
                    break;
                case cgltf_attribute_type_tangent:
                    if (attribute.index == 0) tangents = attribute.data;
                    break;
                case cgltf_attribute_type_texcoord:
                    if (attribute.index == 0)
                        texCoords = attribute.data;
                    else
                        context.warnings.Add("TEXCOORD_1 and later texture coordinate sets are ignored");
                    break;
                case cgltf_attribute_type_color:
                    if (attribute.index == 0)
                        colors = attribute.data;
                    else
                        context.warnings.Add("COLOR_1 and later vertex colour sets are ignored");
                    break;
                case cgltf_attribute_type_joints:
                case cgltf_attribute_type_weights:
                    context.warnings.Add("skins are ignored (the model is imported in its bind pose)");
                    break;
                default:
                    break; // custom attributes
                }
            }
            if (!positions)
            {
                context.warnings.Add(std::format("{} has no POSITION and is skipped", where));
                return std::nullopt;
            }
            if (positions->count > kMaxAccessorElements)
            {
                return ModelImportError{ModelImportErrorCode::TooLarge,
                                        std::format("{}: {} vertices, over the {} limit", where, positions->count,
                                                    kMaxAccessorElements)};
            }
            const std::size_t vertexCount = positions->count;
            bool nonFinite = false;
            // Charged before anything is read: the attribute arrays below are proportional to it
            if (!context.budget.AddVertices(vertexCount))
            {
                return OverBudget("vertices");
            }

            std::vector<float> positionData;
            if (auto error = ReadFloats(*positions, 3, "POSITION", positionData, nonFinite))
            {
                return invalid(*error);
            }
            const bool generate = context.settings.generateNormals == NormalGeneration::Always ||
                                  (!normals && context.settings.generateNormals == NormalGeneration::IfMissing);
            std::vector<float> normalData;
            if (normals && !generate)
            {
                if (normals->count != vertexCount)
                    return invalid("NORMAL has a different element count from POSITION");
                if (auto error = ReadFloats(*normals, 3, "NORMAL", normalData, nonFinite))
                    return invalid(*error);
            }
            std::vector<float> uvData;
            if (texCoords)
            {
                if (texCoords->count != vertexCount)
                    return invalid("TEXCOORD_0 has a different element count from POSITION");
                if (auto error = ReadFloats(*texCoords, 2, "TEXCOORD_0", uvData, nonFinite))
                    return invalid(*error);
            }
            std::vector<float> colorData;
            std::size_t colorComponents = 0;
            if (colors)
            {
                colorComponents = cgltf_num_components(colors->type);
                if (colors->count != vertexCount)
                    return invalid("COLOR_0 has a different element count from POSITION");
                if (colorComponents != 3 && colorComponents != 4)
                    return invalid("COLOR_0 must be VEC3 or VEC4");
                if (auto error = ReadFloats(*colors, colorComponents, "COLOR_0", colorData, nonFinite))
                    return invalid(*error);
            }
            // The file's tangents are kept unless they are to be generated: with generateTangents = Always, or when this
            // primitive's normals are (the file's tangents belong to its own normals)
            std::vector<float> tangentData;
            if (tangents && !generate && context.settings.generateTangents != TangentGeneration::Always)
            {
                if (cgltf_num_components(tangents->type) != 4)
                    return invalid("TANGENT must be VEC4");
                if (tangents->count != vertexCount)
                    return invalid("TANGENT has a different element count from POSITION");
                if (auto error = ReadFloats(*tangents, 4, "TANGENT", tangentData, nonFinite))
                    return invalid(*error);
            }
            if (nonFinite)
            {
                context.warnings.Add("some vertex values were not finite numbers and were read as 0");
            }

            const std::size_t sourceCount = primitive.indices ? primitive.indices->count : vertexCount;
            // The indices as read, and the triangle list made from them (a strip or fan makes up to three per index)
            const std::size_t listCount = primitive.type == cgltf_primitive_type_triangles
                                              ? sourceCount / 3 * 3
                                              : (sourceCount >= 3 ? (sourceCount - 2) * 3 : 0);
            if (!context.budget.AddIndices(sourceCount) || !context.budget.AddIndices(listCount))
            {
                return OverBudget("indices");
            }
            std::vector<std::uint32_t> source;
            if (primitive.indices)
            {
                if (auto error = ReadIndices(*primitive.indices, source))
                    return invalid(*error);
                for (std::size_t i = 0; i < source.size(); ++i)
                {
                    if (source[i] >= vertexCount)
                    {
                        return invalid(std::format("index {} is {}, past the last of its {} vertices", i, source[i],
                                                   vertexCount));
                    }
                }
            }
            else
            {
                source.resize(vertexCount);
                for (std::size_t i = 0; i < vertexCount; ++i)
                {
                    source[i] = static_cast<std::uint32_t>(i);
                }
            }
            const std::vector<std::uint32_t> triangles = Triangulate(primitive.type, source);
            if (triangles.empty())
            {
                context.warnings.Add(std::format("{} has no triangles and is skipped", where));
                return std::nullopt;
            }

            std::vector<ImportedVertex> vertices(vertexCount);
            for (std::size_t v = 0; v < vertexCount; ++v)
            {
                ImportedVertex &vertex = vertices[v];
                for (int c = 0; c < 3; ++c)
                {
                    vertex.position[c] = positionData[v * 3 + static_cast<std::size_t>(c)] * context.scale;
                }
                if (!normalData.empty())
                {
                    for (int c = 0; c < 3; ++c)
                        vertex.normal[c] = normalData[v * 3 + static_cast<std::size_t>(c)];
                }
                if (!uvData.empty())
                {
                    vertex.texCoord[0] = uvData[v * 2];
                    vertex.texCoord[1] = 1.0f - uvData[v * 2 + 1]; // glTF's v runs down the image; the engine's up
                }
                if (!colorData.empty())
                {
                    for (std::size_t c = 0; c < colorComponents; ++c)
                        vertex.color[c] = colorData[v * colorComponents + c];
                }
                if (!tangentData.empty())
                {
                    // glTF's tangent is along +u and its w the bitangent's sign, cross(normal, tangent) * w: along
                    // decreasing glTF v, which is the engine's increasing (flipped) v. So it is used as it is.
                    for (std::size_t c = 0; c < 3; ++c)
                        vertex.tangent[c] = tangentData[v * 4 + c];
                    vertex.tangent[3] = tangentData[v * 4 + 3] < 0.0f ? -1.0f : 1.0f;
                }
            }

            const std::size_t base = mesh.vertices.size();
            const std::size_t firstIndex = mesh.indices.size();
            if (generate)
            {
                // Flat normals: every triangle gets its own three vertices
                if (!context.budget.AddVertices(triangles.size()))
                {
                    return OverBudget("vertices");
                }
                if (base + triangles.size() > kMaxMeshElements || firstIndex + triangles.size() > kMaxMeshElements)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("mesh '{}' is over the {}-vertex limit", meshName, kMaxMeshElements)};
                }
                for (std::size_t t = 0; t < triangles.size(); t += 3)
                {
                    const std::array<float, 3> n =
                        FaceNormal(vertices[triangles[t]], vertices[triangles[t + 1]], vertices[triangles[t + 2]]);
                    for (std::size_t k = 0; k < 3; ++k)
                    {
                        ImportedVertex vertex = vertices[triangles[t + k]];
                        vertex.normal[0] = n[0];
                        vertex.normal[1] = n[1];
                        vertex.normal[2] = n[2];
                        mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                        mesh.vertices.push_back(vertex);
                    }
                }
                mesh.generatedNormals = true;
            }
            else
            {
                if (base + vertexCount > kMaxMeshElements || firstIndex + triangles.size() > kMaxMeshElements)
                {
                    return ModelImportError{ModelImportErrorCode::TooLarge,
                                            std::format("mesh '{}' is over the {}-vertex limit", meshName, kMaxMeshElements)};
                }
                // An earlier primitive of this mesh with the same attributes already added these vertices
                const cgltf_accessor *usedNormals = normalData.empty() ? nullptr : normals;
                const cgltf_accessor *usedTangents = tangentData.empty() ? nullptr : tangents;
                std::size_t start = base;
                const auto same = std::ranges::find_if(shared, [&](const SharedVertices &run)
                {
                    return run.positions == positions && run.normals == usedNormals && run.texCoords == texCoords &&
                           run.colors == colors && run.tangents == usedTangents;
                });
                if (same != shared.end())
                {
                    start = same->base;
                }
                else
                {
                    mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());
                    shared.push_back(SharedVertices{positions, usedNormals, texCoords, colors, usedTangents, base});
                }
                for (const std::uint32_t index : triangles)
                {
                    mesh.indices.push_back(static_cast<std::uint32_t>(start + index));
                }
            }

            ImportedSubmesh submesh;
            submesh.firstIndex = static_cast<std::uint32_t>(firstIndex);
            submesh.indexCount = static_cast<std::uint32_t>(mesh.indices.size() - firstIndex);
            if (context.settings.importMaterials && primitive.material)
            {
                submesh.materialIndex = static_cast<std::int32_t>(primitive.material - context.data.materials);
            }
            mesh.submeshes.push_back(submesh);
            return std::nullopt;
        }

        /// Gives a mesh's vertices without a tangent MikkTSpace tangents (all of them with generateTangents = Always),
        /// from the mesh's own positions, normals and texture coordinates. A vertex whose triangles need different
        /// tangents is duplicated; the duplicates count against the scene's vertex budget. A mesh with no texture
        /// coordinates at all keeps zero tangents (it has nothing for a normal map to map).
        std::optional<ModelImportError> FinishTangents(const MeshContext &context, const std::string &meshName,
                                                       ImportedMesh &mesh)
        {
            if (context.settings.generateTangents == TangentGeneration::Never || mesh.vertices.empty())
            {
                return std::nullopt;
            }
            const bool hasUvs = std::ranges::any_of(mesh.vertices, [](const ImportedVertex &vertex)
            {
                return vertex.texCoord[0] != 0.0f || vertex.texCoord[1] != 0.0f;
            });
            if (!hasUvs)
            {
                return std::nullopt;
            }
            // Duplicates are within what the scene may still hold, and the mesh's own limit, so the budget below
            // can't fail
            const std::size_t sceneRoom = kMaxSceneVertices - context.budget.vertices;
            const std::size_t limit = std::min(kMaxMeshElements, mesh.vertices.size() + sceneRoom);
            const TangentResult result = GenerateTangents(
                mesh.vertices, mesh.indices,
                context.settings.generateTangents == TangentGeneration::Always ? TangentSelect::All : TangentSelect::Missing,
                limit);
            if (!result.ok)
            {
                context.warnings.Add(std::format("mesh '{}': tangents could not be generated (it keeps what it had)", meshName));
                return std::nullopt;
            }
            if (!context.budget.AddVertices(result.verticesAdded))
            {
                return OverBudget("vertices");
            }
            if (result.verticesWritten > 0)
            {
                mesh.generatedTangents = true;
            }
            return std::nullopt;
        }

        /// Groups the submeshes by material (in order of first use): one submesh per material
        void MergeByMaterial(ImportedMesh &mesh)
        {
            std::vector<std::int32_t> order;
            for (const ImportedSubmesh &submesh : mesh.submeshes)
            {
                if (std::ranges::find(order, submesh.materialIndex) == order.end())
                {
                    order.push_back(submesh.materialIndex);
                }
            }
            if (order.size() == mesh.submeshes.size())
            {
                return; // nothing shared
            }
            std::vector<std::uint32_t> indices;
            indices.reserve(mesh.indices.size());
            std::vector<ImportedSubmesh> merged;
            for (const std::int32_t material : order)
            {
                ImportedSubmesh joined;
                joined.firstIndex = static_cast<std::uint32_t>(indices.size());
                joined.materialIndex = material;
                for (const ImportedSubmesh &submesh : mesh.submeshes)
                {
                    if (submesh.materialIndex == material)
                    {
                        indices.insert(indices.end(), mesh.indices.begin() + submesh.firstIndex,
                                       mesh.indices.begin() + submesh.firstIndex + submesh.indexCount);
                    }
                }
                joined.indexCount = static_cast<std::uint32_t>(indices.size() - joined.firstIndex);
                merged.push_back(joined);
            }
            mesh.indices = std::move(indices);
            mesh.submeshes = std::move(merged);
        }

        // ===== Nodes =====

        /// Splits a column-major 4 x 4 matrix (no shear, as glTF requires) into translation, rotation and scale. A
        /// mirroring matrix (negative determinant) gets a negative x scale.
        bool Decompose(const float *m, ImportedNode &node)
        {
            node.translation[0] = m[12];
            node.translation[1] = m[13];
            node.translation[2] = m[14];
            const float c0[3] = {m[0], m[1], m[2]};
            const float c1[3] = {m[4], m[5], m[6]};
            const float c2[3] = {m[8], m[9], m[10]};
            const auto length = [](const float *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
            float sx = length(c0);
            const float sy = length(c1);
            const float sz = length(c2);
            const float det = c0[0] * (c1[1] * c2[2] - c1[2] * c2[1]) - c0[1] * (c1[0] * c2[2] - c1[2] * c2[0]) +
                              c0[2] * (c1[0] * c2[1] - c1[1] * c2[0]);
            if (det < 0.0f)
            {
                sx = -sx;
            }
            node.scale[0] = sx;
            node.scale[1] = sy;
            node.scale[2] = sz;
            if (!std::isfinite(det) || !std::isfinite(node.translation[0]) || !std::isfinite(node.translation[1]) ||
                !std::isfinite(node.translation[2]))
            {
                return false;
            }
            if (std::abs(sx) < 1e-20f || sy < 1e-20f || sz < 1e-20f)
            {
                // A zero scale has no rotation to recover
                node.rotation[0] = node.rotation[1] = node.rotation[2] = 0.0f;
                node.rotation[3] = 1.0f;
                return true;
            }
            // r[row][column] of the pure rotation
            const float r00 = c0[0] / sx, r10 = c0[1] / sx, r20 = c0[2] / sx;
            const float r01 = c1[0] / sy, r11 = c1[1] / sy, r21 = c1[2] / sy;
            const float r02 = c2[0] / sz, r12 = c2[1] / sz, r22 = c2[2] / sz;
            float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
            const float trace = r00 + r11 + r22;
            if (trace > 0.0f)
            {
                const float s = std::sqrt(trace + 1.0f) * 2.0f;
                w = 0.25f * s;
                x = (r21 - r12) / s;
                y = (r02 - r20) / s;
                z = (r10 - r01) / s;
            }
            else if (r00 > r11 && r00 > r22)
            {
                const float s = std::sqrt(std::max(1.0f + r00 - r11 - r22, 1e-12f)) * 2.0f;
                w = (r21 - r12) / s;
                x = 0.25f * s;
                y = (r01 + r10) / s;
                z = (r02 + r20) / s;
            }
            else if (r11 > r22)
            {
                const float s = std::sqrt(std::max(1.0f + r11 - r00 - r22, 1e-12f)) * 2.0f;
                w = (r02 - r20) / s;
                x = (r01 + r10) / s;
                y = 0.25f * s;
                z = (r12 + r21) / s;
            }
            else
            {
                const float s = std::sqrt(std::max(1.0f + r22 - r00 - r11, 1e-12f)) * 2.0f;
                w = (r10 - r01) / s;
                x = (r02 + r20) / s;
                y = (r12 + r21) / s;
                z = 0.25f * s;
            }
            const float norm = std::sqrt(w * w + x * x + y * y + z * z);
            if (!(norm > 0.0f) || !std::isfinite(norm))
            {
                return false;
            }
            node.rotation[0] = x / norm;
            node.rotation[1] = y / norm;
            node.rotation[2] = z / norm;
            node.rotation[3] = w / norm;
            return true;
        }

        void NormalizeRotation(ImportedNode &node, Warnings &warnings)
        {
            float *q = node.rotation;
            const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(norm > 0.0f) || !std::isfinite(norm))
            {
                warnings.Add("a node's rotation is not a valid quaternion and was reset");
                q[0] = q[1] = q[2] = 0.0f;
                q[3] = 1.0f;
                return;
            }
            for (int i = 0; i < 4; ++i)
            {
                q[i] /= norm;
            }
        }

        // ===== Materials and images =====

        struct ImageUse
        {
            std::vector<bool> samplerSet;
        };

        std::int32_t ImageOf(const cgltf_data &data, const cgltf_texture_view &view, const bool colour,
                             ImportedScene &scene, ImageUse &use, Warnings &warnings)
        {
            if (!view.texture)
            {
                return -1;
            }
            if (view.has_transform)
            {
                warnings.Add("KHR_texture_transform is ignored (texture offsets, rotations and scales are not applied)");
            }
            if (view.texcoord != 0)
            {
                warnings.Add("textures that use TEXCOORD_1 are drawn with TEXCOORD_0");
            }
            const cgltf_texture &texture = *view.texture;
            if (!texture.image)
            {
                warnings.Add("textures without a PNG or JPEG image (KTX2, WebP) are ignored");
                return -1;
            }
            const auto index = static_cast<std::size_t>(texture.image - data.images);
            if (index >= scene.images.size())
            {
                return -1;
            }
            ImportedModelImage &image = scene.images[index];
            image.colour = image.colour || colour;
            if (!use.samplerSet[index])
            {
                use.samplerSet[index] = true;
                if (texture.sampler)
                {
                    image.nearest = texture.sampler->mag_filter == cgltf_filter_type_nearest;
                    image.clamp = texture.sampler->wrap_s == cgltf_wrap_mode_clamp_to_edge ||
                                  texture.sampler->wrap_t == cgltf_wrap_mode_clamp_to_edge;
                }
            }
            return static_cast<std::int32_t>(index);
        }

        float Unit(const float value, const float fallback)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
        }

        ImportedMaterial ImportMaterial(const cgltf_data &data, const cgltf_material &source, ImportedScene &scene,
                                        ImageUse &use, Warnings &warnings)
        {
            ImportedMaterial material;
            material.name = source.name ? source.name : "";
            material.unlit = source.unlit != 0;
            if (source.has_pbr_metallic_roughness)
            {
                const cgltf_pbr_metallic_roughness &pbr = source.pbr_metallic_roughness;
                for (int c = 0; c < 4; ++c)
                {
                    material.baseColor[c] = std::isfinite(pbr.base_color_factor[c]) ? std::max(pbr.base_color_factor[c], 0.0f)
                                                                                     : 1.0f;
                }
                material.baseColorTexture = ImageOf(data, pbr.base_color_texture, true, scene, use, warnings);
                material.metallic = Unit(pbr.metallic_factor, 1.0f);
                material.smoothness = 1.0f - Unit(pbr.roughness_factor, 1.0f);
                material.metallicRoughnessTexture = ImageOf(data, pbr.metallic_roughness_texture, false, scene, use, warnings);
            }
            material.normalTexture = ImageOf(data, source.normal_texture, false, scene, use, warnings);
            // cgltf keeps normalTexture.scale in the view's scale (1 by default; only read for a view in the file)
            material.normalScale = source.normal_texture.texture && std::isfinite(source.normal_texture.scale)
                                       ? std::clamp(source.normal_texture.scale, 0.0f, 4.0f)
                                       : 1.0f;
            material.occlusionTexture = ImageOf(data, source.occlusion_texture, false, scene, use, warnings);
            // cgltf keeps the occlusion texture's strength in the view's scale
            // (zero without a texture: cgltf only sets it for a view that is in the file)
            material.occlusionStrength = source.occlusion_texture.texture ? Unit(source.occlusion_texture.scale, 1.0f) : 1.0f;
            material.emissiveTexture = ImageOf(data, source.emissive_texture, true, scene, use, warnings);
            for (int c = 0; c < 3; ++c)
            {
                material.emissive[c] = std::isfinite(source.emissive_factor[c]) ? std::max(source.emissive_factor[c], 0.0f)
                                                                                : 0.0f;
            }
            switch (source.alpha_mode)
            {
            case cgltf_alpha_mode_mask:
                material.alphaMode = ImportedAlphaMode::Mask;
                break;
            case cgltf_alpha_mode_blend:
                material.alphaMode = ImportedAlphaMode::Blend;
                break;
            default:
                material.alphaMode = ImportedAlphaMode::Opaque;
                break;
            }
            material.alphaCutoff = Unit(source.alpha_cutoff, 0.5f);
            material.doubleSided = source.double_sided != 0;
            return material;
        }

        ModelImportError ImagesTooLarge()
        {
            return ModelImportError{ModelImportErrorCode::TooLarge,
                                    std::format("the model's images come to over the {}-byte limit",
                                                kMaxModelTotalImageBytes)};
        }

        /// Reads every image's bytes. One image over kMaxModelImageBytes, or that can't be read, is skipped with a
        /// warning; all of them over kMaxModelTotalImageBytes is an error (many images can name one big view).
        std::optional<ModelImportError> ReadImages(const cgltf_data &data, const fs::path &baseDirectory,
                                                   ImportedScene &scene, Warnings &warnings)
        {
            std::size_t total = 0;
            const auto charge = [&total](const std::size_t size)
            {
                if (size > kMaxModelTotalImageBytes - total)
                    return false;
                total += size;
                return true;
            };
            scene.images.resize(data.images_count);
            for (cgltf_size i = 0; i < data.images_count; ++i)
            {
                const cgltf_image &source = data.images[i];
                ImportedModelImage &image = scene.images[i];
                image.name = source.name ? source.name : "";
                image.mimeType = source.mime_type ? source.mime_type : "";
                const std::string label = NameOr(source.name, "image", i);
                if (source.buffer_view)
                {
                    bool ok = false;
                    const auto bytes = ViewBytes(source.buffer_view, ok);
                    if (!ok)
                    {
                        warnings.Add(std::format("image '{}' is outside its buffer and is skipped", label));
                    }
                    else if (bytes.size() > kMaxModelImageBytes)
                    {
                        warnings.Add(std::format("image '{}' is over the {}-byte limit and is skipped", label,
                                                 kMaxModelImageBytes));
                    }
                    else
                    {
                        if (!charge(bytes.size()))
                            return ImagesTooLarge();
                        image.bytes.assign(bytes.begin(), bytes.end());
                    }
                    continue;
                }
                if (!source.uri)
                {
                    warnings.Add(std::format("image '{}' has no data and is skipped", label));
                    continue;
                }
                const std::string_view uri(source.uri);
                if (uri.starts_with("data:"))
                {
                    const auto payload = Base64Payload(uri);
                    if (payload && !charge(payload->size() / 4 * 3))
                        return ImagesTooLarge();
                    auto decoded = payload ? DecodeBase64(*payload, kMaxModelImageBytes) : std::nullopt;
                    if (!decoded)
                    {
                        warnings.Add(std::format("image '{}' has a data: URI that isn't valid base64 (or is too large) "
                                                 "and is skipped", label));
                        continue;
                    }
                    image.bytes = std::move(*decoded);
                    continue;
                }
                image.uri = PercentDecode(uri);
                if (baseDirectory.empty())
                {
                    warnings.Add(std::format("image '{}' is in another file but the model has no folder; skipped", label));
                    continue;
                }
                const fs::path path = ResolveModelUri(baseDirectory, image.uri);
                if (path.empty())
                {
                    warnings.Add(std::format("image '{}' URI '{}' is rejected: only relative paths inside the model's "
                                             "folder are read", label, image.uri));
                    continue;
                }
                std::error_code sizeError;
                const std::uintmax_t fileSize = fs::file_size(path, sizeError);
                if (!sizeError && fileSize <= kMaxModelImageBytes && !charge(static_cast<std::size_t>(fileSize)))
                {
                    return ImagesTooLarge();
                }
                std::string error;
                auto read = ReadFileCapped(path, 0, kMaxModelImageBytes, error);
                if (!read)
                {
                    warnings.Add(std::format("image '{}' is skipped: {}", label, error));
                    continue;
                }
                image.bytes = std::move(*read);
            }
            return std::nullopt;
        }

        // ===== The whole file =====

        std::optional<ModelImportError> CheckExtensions(const cgltf_data &data, Warnings &warnings)
        {
            const auto check = [&](const char *name, const bool required) -> std::optional<ModelImportError>
            {
                const std::string_view extension = name ? name : "";
                if (extension == "KHR_draco_mesh_compression" || extension == "EXT_meshopt_compression" ||
                    extension == "KHR_meshopt_compression")
                {
                    return ModelImportError{ModelImportErrorCode::Unsupported,
                                            std::format("{} isn't supported: export without mesh compression", extension)};
                }
                if (extension == "KHR_materials_unlit" || extension == "KHR_mesh_quantization")
                {
                    return std::nullopt;
                }
                if (extension == "KHR_texture_transform")
                {
                    warnings.Add("KHR_texture_transform is ignored (texture offsets, rotations and scales are not applied)");
                    return std::nullopt;
                }
                warnings.Add(std::format("the {}extension {} isn't supported and is ignored", required ? "required " : "",
                                         extension));
                return std::nullopt;
            };
            for (cgltf_size i = 0; i < data.extensions_required_count; ++i)
            {
                if (auto error = check(data.extensions_required[i], true))
                    return error;
            }
            for (cgltf_size i = 0; i < data.extensions_used_count; ++i)
            {
                bool alsoRequired = false;
                for (cgltf_size j = 0; j < data.extensions_required_count; ++j)
                {
                    alsoRequired = alsoRequired || (data.extensions_used[i] && data.extensions_required[j] &&
                                                    std::strcmp(data.extensions_used[i], data.extensions_required[j]) == 0);
                }
                if (alsoRequired)
                {
                    continue;
                }
                if (auto error = check(data.extensions_used[i], false))
                    return error;
            }
            // Belt and braces: compression used without being declared
            for (cgltf_size i = 0; i < data.buffer_views_count; ++i)
            {
                if (data.buffer_views[i].has_meshopt_compression)
                {
                    return ModelImportError{ModelImportErrorCode::Unsupported,
                                            "EXT_meshopt_compression isn't supported: export without mesh compression"};
                }
            }
            for (cgltf_size m = 0; m < data.meshes_count; ++m)
            {
                for (cgltf_size p = 0; p < data.meshes[m].primitives_count; ++p)
                {
                    if (data.meshes[m].primitives[p].has_draco_mesh_compression)
                    {
                        return ModelImportError{ModelImportErrorCode::Unsupported,
                                                "KHR_draco_mesh_compression isn't supported: export without mesh compression"};
                    }
                }
            }
            return std::nullopt;
        }

        std::optional<ModelImportError> BuildNodes(const cgltf_data &data, const float scale, ImportedScene &scene,
                                                   Warnings &warnings)
        {
            scene.nodes.resize(data.nodes_count);
            for (cgltf_size i = 0; i < data.nodes_count; ++i)
            {
                const cgltf_node &source = data.nodes[i];
                ImportedNode &node = scene.nodes[i];
                node.name = source.name ? source.name : "";
                if (source.has_matrix)
                {
                    if (!Decompose(source.matrix, node))
                    {
                        warnings.Add("a node's matrix is not a valid transform and was reset");
                        node = ImportedNode{};
                        node.name = source.name ? source.name : "";
                    }
                }
                else
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        node.translation[c] = std::isfinite(source.translation[c]) ? source.translation[c] : 0.0f;
                        node.scale[c] = std::isfinite(source.scale[c]) ? source.scale[c] : 1.0f;
                    }
                    for (int c = 0; c < 4; ++c)
                    {
                        node.rotation[c] = source.rotation[c];
                    }
                    NormalizeRotation(node, warnings);
                }
                for (float &t : node.translation)
                {
                    t *= scale;
                }
                if (source.mesh)
                {
                    node.meshIndex = static_cast<std::int32_t>(source.mesh - data.meshes);
                }
                if (source.camera)
                    warnings.Add("cameras are ignored");
                if (source.light)
                    warnings.Add("lights are ignored");
                if (source.skin)
                    warnings.Add("skins are ignored (the model is imported in its bind pose)");
                if (source.weights_count > 0)
                    warnings.Add("morph targets are ignored");
                if (source.has_mesh_gpu_instancing)
                    warnings.Add("EXT_mesh_gpu_instancing is ignored (each instanced node is drawn once)");
                for (cgltf_size c = 0; c < source.children_count; ++c)
                {
                    const cgltf_node *child = source.children[c];
                    // cgltf records one parent per node: a node listed under two parents stays under the one it
                    // recorded, so the result is a tree
                    if (child && child->parent == &source)
                    {
                        node.children.push_back(static_cast<std::uint32_t>(child - data.nodes));
                    }
                }
            }

            const cgltf_scene *sceneSource = data.scene ? data.scene : (data.scenes_count > 0 ? &data.scenes[0] : nullptr);
            std::vector<bool> isRoot(data.nodes_count, false);
            const auto addRoot = [&](const cgltf_node *node)
            {
                if (!node || node->parent)
                {
                    return;
                }
                const auto index = static_cast<std::size_t>(node - data.nodes);
                if (index < data.nodes_count && !isRoot[index])
                {
                    isRoot[index] = true;
                    scene.rootNodes.push_back(static_cast<std::uint32_t>(index));
                }
            };
            if (sceneSource)
            {
                for (cgltf_size i = 0; i < sceneSource->nodes_count; ++i)
                {
                    addRoot(sceneSource->nodes[i]);
                }
            }
            else
            {
                for (cgltf_size i = 0; i < data.nodes_count; ++i)
                {
                    addRoot(&data.nodes[i]);
                }
            }

            // Depth (and, defensively, cycles): iterative, so a deep file can't overflow the stack here
            std::vector<std::pair<std::uint32_t, std::size_t>> stack;
            std::vector<bool> visited(data.nodes_count, false);
            for (const std::uint32_t root : scene.rootNodes)
            {
                stack.emplace_back(root, 1);
            }
            while (!stack.empty())
            {
                const auto [index, depth] = stack.back();
                stack.pop_back();
                if (depth > kMaxNodeDepth)
                {
                    return ModelImportError{ModelImportErrorCode::InvalidData,
                                            std::format("the node tree is deeper than {} levels", kMaxNodeDepth)};
                }
                if (visited[index])
                {
                    return ModelImportError{ModelImportErrorCode::InvalidData, "the node hierarchy isn't a tree"};
                }
                visited[index] = true;
                for (const std::uint32_t child : scene.nodes[index].children)
                {
                    stack.emplace_back(child, depth + 1);
                }
            }
            return std::nullopt;
        }

        ImportResult ImportGltf(std::span<const std::uint8_t> bytes, const fs::path &baseDirectory,
                                const ModelImportSettings &settings)
        {
            if (bytes.empty())
            {
                return Fail(ModelImportErrorCode::EmptyInput, "the model data is empty");
            }
            if (bytes.size() > kMaxModelFileBytes)
            {
                return Fail(ModelImportErrorCode::TooLarge,
                            std::format("the model is {} bytes, over the {}-byte limit", bytes.size(), kMaxModelFileBytes));
            }
            const bool isGlb = bytes.size() >= 4 && ReadU32(bytes.data()) == kGlbMagic;
            if (isGlb)
            {
                if (auto error = CheckGlb(bytes))
                {
                    return std::unexpected(std::move(*error));
                }
            }
            else if (bytes.size() > kMaxModelJsonBytes)
            {
                return Fail(ModelImportErrorCode::TooLarge, std::format("the glTF JSON is {} bytes, over the {}-byte limit",
                                                                        bytes.size(), kMaxModelJsonBytes));
            }

            // Declared before the data, so it outlives every block cgltf_free hands back to it
            ParseAllocator allocator;
            cgltf_options options{};
            options.type = isGlb ? cgltf_file_type_glb : cgltf_file_type_gltf;
            options.memory.alloc_func = &CountingAlloc;
            options.memory.free_func = &CountingFree;
            options.memory.user_data = &allocator;
            cgltf_data *raw = nullptr;
            const cgltf_result parsed = cgltf_parse(&options, bytes.data(), bytes.size(), &raw);
            if (parsed == cgltf_result_out_of_memory)
            {
                return Fail(ModelImportErrorCode::TooLarge,
                            std::format("parsing the glTF JSON needs over {} bytes of memory (too many objects)",
                                        kMaxParseMemoryBytes));
            }
            if (parsed != cgltf_result_success || !raw)
            {
                return Fail(ModelImportErrorCode::ParseFailed, std::format("not a readable glTF 2.0 file: {}", ResultName(parsed)));
            }
            const CgltfData data(raw);
            allocator.limit = std::numeric_limits<std::size_t>::max(); // buffers have their own caps

            ImportedScene scene;
            Warnings warnings(scene.warnings);
            if (auto error = CheckExtensions(*data, warnings))
            {
                return std::unexpected(std::move(*error));
            }
            if (auto error = LoadBuffers(*data, baseDirectory))
            {
                return std::unexpected(std::move(*error));
            }
            if (auto error = CheckRanges(*data))
            {
                return std::unexpected(std::move(*error));
            }
            if (const cgltf_result valid = cgltf_validate(data.get()); valid != cgltf_result_success)
            {
                return Fail(ModelImportErrorCode::InvalidData, std::format("the glTF file is inconsistent: {}", ResultName(valid)));
            }

            if (data->cameras_count > 0)
                warnings.Add("cameras are ignored");
            if (data->lights_count > 0)
                warnings.Add("lights are ignored");
            if (data->skins_count > 0)
                warnings.Add("skins are ignored (the model is imported in its bind pose)");
            if (data->animations_count > 0)
                warnings.Add("animations are ignored");

            const float scale = std::isfinite(settings.scale) && settings.scale > 0.0f ? settings.scale : 1.0f;

            // Images and materials first, so materials can mark how each image is used
            ImageUse use;
            if (settings.importMaterials)
            {
                if (auto error = ReadImages(*data, baseDirectory, scene, warnings))
                {
                    return std::unexpected(std::move(*error));
                }
                use.samplerSet.assign(scene.images.size(), false);
                scene.materials.reserve(data->materials_count);
                for (cgltf_size i = 0; i < data->materials_count; ++i)
                {
                    scene.materials.push_back(ImportMaterial(*data, data->materials[i], scene, use, warnings));
                }
            }

            Budget budget;
            const MeshContext context{*data, settings, scale, warnings, budget};
            scene.meshes.resize(data->meshes_count);
            for (cgltf_size m = 0; m < data->meshes_count; ++m)
            {
                const cgltf_mesh &source = data->meshes[m];
                ImportedMesh &mesh = scene.meshes[m];
                mesh.name = source.name ? source.name : "";
                const std::string label = NameOr(source.name, "mesh", m);
                std::vector<SharedVertices> shared;
                for (cgltf_size p = 0; p < source.primitives_count; ++p)
                {
                    if (auto error = ImportPrimitive(context, shared, source.primitives[p], label, p, mesh))
                    {
                        return std::unexpected(std::move(*error));
                    }
                }
                if (auto error = FinishTangents(context, label, mesh))
                {
                    return std::unexpected(std::move(*error));
                }
                if (settings.mergeSubmeshesByMaterial)
                {
                    MergeByMaterial(mesh);
                }
            }

            if (auto error = BuildNodes(*data, scale, scene, warnings))
            {
                return std::unexpected(std::move(*error));
            }
            return scene;
        }
    }

    std::string ToString(const ModelImportErrorCode code)
    {
        switch (code)
        {
        case ModelImportErrorCode::EmptyInput: return "EmptyInput";
        case ModelImportErrorCode::TooLarge: return "TooLarge";
        case ModelImportErrorCode::ParseFailed: return "ParseFailed";
        case ModelImportErrorCode::InvalidData: return "InvalidData";
        case ModelImportErrorCode::Unsupported: return "Unsupported";
        case ModelImportErrorCode::BufferLoadFailed: return "BufferLoadFailed";
        }
        return "Unknown";
    }

    std::string PercentDecode(const std::string_view uri)
    {
        const auto hex = [](const char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(uri.size());
        for (std::size_t i = 0; i < uri.size(); ++i)
        {
            if (uri[i] == '%' && i + 2 < uri.size())
            {
                const int high = hex(uri[i + 1]);
                const int low = hex(uri[i + 2]);
                if (high >= 0 && low >= 0)
                {
                    out.push_back(static_cast<char>(high * 16 + low));
                    i += 2;
                    continue;
                }
            }
            out.push_back(uri[i]);
        }
        return out;
    }

    namespace
    {
        /// Whether `path` is `base` plus at least one more element, none of them ".." (both lexically normal)
        bool IsStrictlyInside(const fs::path &base, const fs::path &path)
        {
            std::vector<fs::path> baseParts;
            for (const fs::path &part : base)
            {
                if (!part.empty() && part != ".")
                {
                    baseParts.push_back(part);
                }
            }
            std::vector<fs::path> pathParts;
            for (const fs::path &part : path)
            {
                if (!part.empty() && part != ".")
                {
                    pathParts.push_back(part);
                }
            }
            if (pathParts.size() <= baseParts.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < baseParts.size(); ++i)
            {
                if (pathParts[i] != baseParts[i])
                {
                    return false;
                }
            }
            for (std::size_t i = baseParts.size(); i < pathParts.size(); ++i)
            {
                if (pathParts[i] == "..")
                {
                    return false;
                }
            }
            return true;
        }

        /// CON, PRN, AUX, NUL, COM1-9, LPT1-9, CONIN$ and CONOUT$, whatever the case and extension (the part
        /// before the first dot, with trailing spaces ignored, as Windows matches them)
        bool IsReservedDeviceName(const fs::path &part)
        {
            const std::u8string utf8 = part.u8string();
            std::string stem;
            for (const char8_t c : utf8)
            {
                if (c == u8'.')
                {
                    break;
                }
                stem.push_back(static_cast<char>(c >= u8'a' && c <= u8'z' ? c - (u8'a' - u8'A') : c));
            }
            while (!stem.empty() && stem.back() == ' ')
            {
                stem.pop_back();
            }
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" ||
                stem == "CONOUT$")
            {
                return true;
            }
            return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
                   stem[3] <= '9';
        }
    }

    fs::path ResolveModelUri(const fs::path &baseDirectory, const std::string_view decodedUri)
    {
        if (baseDirectory.empty() || decodedUri.empty())
        {
            return {};
        }
        // No NUL (it would cut the path short), no scheme or drive letter ("http:", "file:", "C:"), no
        // alternate data stream, and no absolute path
        if (decodedUri.find('\0') != std::string_view::npos || decodedUri.find(':') != std::string_view::npos ||
            decodedUri.front() == '/' || decodedUri.front() == '\\')
        {
            return {};
        }
        std::u8string utf8;
        utf8.reserve(decodedUri.size());
        for (const char c : decodedUri)
        {
            utf8.push_back(static_cast<char8_t>(c));
        }
        const fs::path relative(utf8);
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        {
            return {};
        }
        // Windows reserved device names, in any case and with any extension ("nul.bin", "COM1.png"), name devices
        // rather than files, in any folder
        for (const fs::path &part : relative)
        {
            if (IsReservedDeviceName(part))
            {
                return {};
            }
        }
        const fs::path base = baseDirectory.lexically_normal();
        const fs::path resolved = (base / relative).lexically_normal();
        if (!IsStrictlyInside(base, resolved))
        {
            return {};
        }
        // Lexically inside; an existing file must also be inside once symbolic links and junctions are followed
        std::error_code existsError;
        if (fs::exists(resolved, existsError))
        {
            std::error_code baseError;
            std::error_code targetError;
            const fs::path canonicalBase = fs::weakly_canonical(base, baseError);
            const fs::path canonicalTarget = fs::weakly_canonical(resolved, targetError);
            if (baseError || targetError || !IsStrictlyInside(canonicalBase, canonicalTarget))
            {
                return {};
            }
        }
        return resolved;
    }

    std::expected<ImportedScene, ModelImportError> GltfImporter::Import(const std::span<const std::uint8_t> bytes,
                                                                        const fs::path &baseDirectory,
                                                                        const ModelImportSettings &settings) const
    {
        // The caps keep allocations bounded; this is the backstop, so a failed allocation is an error value too
        try
        {
            return ImportGltf(bytes, baseDirectory, settings);
        }
        catch (const std::bad_alloc &)
        {
            return Fail(ModelImportErrorCode::TooLarge, "out of memory while importing the model");
        }
        catch (const std::length_error &)
        {
            return Fail(ModelImportErrorCode::TooLarge, "the model needs a container larger than the library allows");
        }
    }

    bool GltfImporter::HandlesExtension(const std::string_view extension) const
    {
        return extension == ".gltf" || extension == ".glb";
    }
}
