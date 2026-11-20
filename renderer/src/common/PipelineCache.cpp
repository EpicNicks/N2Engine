#include "renderer/common/PipelineCache.hpp"

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace N2Engine::Renderer::Common::PipelineCacheFile
{
    namespace
    {
        std::mutex &DirectoryMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::filesystem::path &Directory()
        {
            static std::filesystem::path directory;
            return directory;
        }

        std::uint32_t ReadU32(const std::uint8_t *at)
        {
            return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) | (static_cast<std::uint32_t>(at[2]) << 16) |
                (static_cast<std::uint32_t>(at[3]) << 24);
        }
    }

    void SetDirectory(std::filesystem::path directory)
    {
        const std::lock_guard lock(DirectoryMutex());
        Directory() = std::move(directory);
    }

    std::filesystem::path GetDirectory()
    {
        const std::lock_guard lock(DirectoryMutex());
        return Directory();
    }

    std::string FileName(const std::uint32_t vendorId, const std::uint32_t deviceId)
    {
        return std::format("vk-pipelines-{:08x}-{:08x}.bin", vendorId, deviceId);
    }

    bool HeaderMatches(const std::uint8_t *data, const std::size_t size, const std::uint32_t vendorId, const std::uint32_t deviceId,
                       const std::uint8_t (&uuid)[16])
    {
        if (data == nullptr || size < 32)
        {
            return false;
        }
        // VkPipelineCacheHeaderVersionOne: headerSize, headerVersion (1 = VK_PIPELINE_CACHE_HEADER_VERSION_ONE), vendorID, deviceID, UUID
        return ReadU32(data) >= 32 && ReadU32(data + 4) == 1 && ReadU32(data + 8) == vendorId && ReadU32(data + 12) == deviceId &&
            std::memcmp(data + 16, uuid, 16) == 0;
    }

    std::optional<std::vector<std::uint8_t>> Load(const std::filesystem::path &file, const std::uint32_t vendorId,
                                                  const std::uint32_t deviceId, const std::uint8_t (&uuid)[16])
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error || size == 0 || size > MaxBytes)
        {
            return std::nullopt;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!HeaderMatches(bytes.data(), bytes.size(), vendorId, deviceId, uuid))
        {
            return std::nullopt;
        }
        return bytes;
    }

    bool Save(const std::filesystem::path &file, const std::uint8_t *data, const std::size_t size)
    {
        if (size == 0 || size > MaxBytes)
        {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        std::filesystem::path temporary = file;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                return false;
            }
            out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
            if (!out)
            {
                return false;
            }
        }
        std::filesystem::rename(temporary, file, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }
}
