#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <renderer/common/TextureOptions.hpp>

#include "engine/base/Asset.hpp"

namespace N2Engine::Rendering
{
    /**
     * A texture asset's import settings, from its .meta file's customData:
     *   {"texture": {"srgb": true, "filter": "linear", "wrap": "repeat", "mipmaps": true, "flipY": true}}
     * Every key is optional; the defaults are below.
     */
    struct TextureSettings
    {
        /// Whether the colours are sRGB-encoded. Recorded only: nothing reads it until the engine gets a
        /// linear-lighting option (#3 P4a), so it has no effect on drawing yet.
        bool srgb = true;
        /// "linear" (the default) or "nearest"
        Renderer::Common::TextureFilter filter = Renderer::Common::TextureFilter::Linear;
        /// "repeat" (the default) or "clamp" (clamp to edge)
        Renderer::Common::TextureWrap wrap = Renderer::Common::TextureWrap::Repeat;
        /// Generate mipmaps (OpenGL; the software renderer ignores them)
        bool mipmaps = true;
        /// Store the rows bottom to top, so the first row of pixel data (v = 0) is the bottom of the picture
        /// and images show upright on the engine's v-up UVs. Turn it off only for data meant to be read top
        /// row first.
        bool flipY = true;

        /// filter, wrap and mipmaps as IRenderer::CreateTexture takes them
        [[nodiscard]] Renderer::Common::TextureOptions ToTextureOptions() const;

        friend bool operator==(const TextureSettings &, const TextureSettings &) = default;
    };

    /**
     * An image asset (.png, .jpg/.jpeg, .tga, .bmp): CPU pixels plus settings, never a GPU handle, so it
     * loads headless. Renderers get their own texture from it through the engine's GPU cache (one per
     * renderer and texture, shared by every user), or a caller creates one with IRenderer::CreateTexture
     * from GetPixels().
     *
     * The pixels are always 8-bit RGBA (grey, grey + alpha and RGB files are expanded), width * height * 4
     * bytes, rows bottom to top when flipY is on (the default; see TextureSettings::flipY). A texture never
     * changes once made: a reload makes a new Texture.
     */
    class Texture final : public Base::Asset
    {
    public:
        Texture() = default;

        /// A texture from RGBA8 pixels made at runtime (procedural textures). `rgbaPixels` is width * height * 4
        /// bytes, the first row being v = 0 (the bottom). nullptr (and an error logged) for a zero size or a
        /// pixel count that doesn't match. srgb and flipY keep their defaults.
        [[nodiscard]] static std::shared_ptr<Texture> Create(
            std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgbaPixels,
            const Renderer::Common::TextureOptions &options = Renderer::Common::TextureOptions::Default());

        /// A texture decoded from an image file's bytes (PNG, JPEG, TGA or BMP) with `settings`. nullptr, and an
        /// error logged naming `debugName`, if it doesn't decode or is over the size caps.
        [[nodiscard]] static std::shared_ptr<Texture> CreateFromEncoded(std::span<const std::uint8_t> fileBytes,
                                                                        const TextureSettings &settings = {},
                                                                        std::string_view debugName = "texture");

        /// The settings in a .meta customData object (its "texture" member), on top of `defaults`. Missing
        /// keys keep the default; a key with the wrong type or an unknown value is ignored with a warning.
        [[nodiscard]] static TextureSettings ParseSettings(const nlohmann::json &customData,
                                                           const TextureSettings &defaults = {});

        /// Registers the .png/.jpg/.jpeg/.tga/.bmp loader with Resources (and so ResourceLoader). Texture.cpp
        /// already does this at static initialisation, and ResourceLoader::Initialize calls it too; calling it
        /// again is harmless.
        static void RegisterLoader();

        /// Loads an image file, with settings from its .meta when ResourceLoader tracks it
        bool Load(const std::filesystem::path &path) override;
        /// What GetResourceType() returns, for code that needs it without an instance (asset metadata, a field's asset type)
        static constexpr std::string_view ResourceTypeName = "Texture";
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        [[nodiscard]] bool IsLoaded() const { return !_pixels.empty(); }
        [[nodiscard]] std::uint32_t GetWidth() const { return _width; }
        [[nodiscard]] std::uint32_t GetHeight() const { return _height; }
        /// Always 4 (RGBA)
        [[nodiscard]] static constexpr std::uint32_t GetChannels() { return 4; }
        /// width * height * 4 bytes, the first row being v = 0; empty if not loaded
        [[nodiscard]] const std::vector<std::uint8_t> &GetPixels() const { return _pixels; }
        [[nodiscard]] const TextureSettings &GetSettings() const { return _settings; }
        /// The settings' filter, wrap and mipmaps, for IRenderer::CreateTexture
        [[nodiscard]] Renderer::Common::TextureOptions GetTextureOptions() const { return _settings.ToTextureOptions(); }
        /// The file this texture was loaded from (empty for Create and CreateFromEncoded)
        [[nodiscard]] const std::string &GetSourcePath() const { return _sourcePath; }

    private:
        bool Decode(std::span<const std::uint8_t> fileBytes, const TextureSettings &settings, std::string_view debugName);

        std::uint32_t _width = 0;
        std::uint32_t _height = 0;
        std::vector<std::uint8_t> _pixels;
        TextureSettings _settings;
        std::string _sourcePath;
    };

    /// The loader registered for .png, .jpg, .jpeg, .tga and .bmp
    std::shared_ptr<Base::Asset> LoadTextureFromFile(const std::filesystem::path &path);
}
