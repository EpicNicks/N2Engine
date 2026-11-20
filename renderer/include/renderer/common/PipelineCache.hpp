#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Renderer::Common::PipelineCacheFile
{
    /// The folder the Vulkan renderer keeps its serialised VkPipelineCache in (one file per vendor and device, see FileName), set by
    /// the engine to `user://cache`. Empty (the default): the cache lives for the session only. Advisory: a missing, unreadable,
    /// corrupt or foreign file only means pipelines are made again
    void SetDirectory(std::filesystem::path directory);
    [[nodiscard]] std::filesystem::path GetDirectory();

    /// "vk-pipelines-<vendor id>-<device id>.bin"
    [[nodiscard]] std::string FileName(std::uint32_t vendorId, std::uint32_t deviceId);

    /// Whether `data` begins with a VkPipelineCacheHeaderVersionOne (32 bytes: header size, version 1, vendor id, device id, the
    /// 16-byte pipelineCacheUUID) that names this device and this driver build (the UUID): the check Vulkan asks an application to
    /// make before it hands saved data back. A cache from another GPU or another driver is not used
    [[nodiscard]] bool HeaderMatches(const std::uint8_t *data, std::size_t size, std::uint32_t vendorId, std::uint32_t deviceId,
                                     const std::uint8_t (&uuid)[16]);

    /// The most a cache file is read from or written to (a cap on a file the engine did not write this run)
    inline constexpr std::size_t MaxBytes = 64u * 1024u * 1024u;

    /// The file's bytes when it exists, is at most MaxBytes and its header matches; nullopt otherwise
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Load(const std::filesystem::path &file, std::uint32_t vendorId,
                                                               std::uint32_t deviceId, const std::uint8_t (&uuid)[16]);

    /// Writes through a temporary file renamed over the target; false when the folder or the file cannot be written
    bool Save(const std::filesystem::path &file, const std::uint8_t *data, std::size_t size);
}
