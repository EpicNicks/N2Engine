#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Editor::Tooling
{
    inline constexpr std::size_t MaxSyntaxBytes = 4 * 1024 * 1024;
    inline constexpr std::size_t MaxPreviewFileBytes = 64 * 1024 * 1024;
    inline constexpr std::size_t MaxPreviewSampleValues = 64 * 1024 * 1024;
    inline constexpr std::uint32_t MaxPreviewSize = 256;

    // Compiles text only in a fresh, allocation-bounded Lua state, without opening libraries or invoking the chunk.
    // A syntax error is a successful diagnostic result; a limit/allocation failure is an Error response.
    [[nodiscard]] std::expected<nlohmann::json, std::string> CheckLuaSyntax(std::string_view text,
                                                                            std::string_view chunkName);
    struct AssetPreview
    {
        nlohmann::json info;
        // Texture only: tightly packed RGBA8, top row first. Empty for an audio waveform.
        std::vector<std::uint8_t> pixels;
    };

    [[nodiscard]] std::expected<AssetPreview, std::string> PreviewImage(std::span<const std::uint8_t> encoded,
                                                                        std::uint32_t maxSize);
    [[nodiscard]] std::expected<AssetPreview, std::string> PreviewAudio(std::span<const std::uint8_t> encoded,
                                                                        std::string_view extension,
                                                                        std::uint32_t maxSize);
    // The host resolves and authorizes the path first. This function opens no GPU or audio device and caches nothing.
    [[nodiscard]] std::expected<AssetPreview, std::string> BuildAssetPreview(const std::filesystem::path &file,
                                                                             std::string_view resourceType,
                                                                             std::uint32_t maxSize);
} // namespace N2Engine::Editor::Tooling
