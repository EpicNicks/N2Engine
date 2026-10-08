#pragma once

#include <cstdint>

namespace Renderer::Common
{
    /// How a texture is sampled between texels
    enum class TextureFilter : std::uint8_t
    {
        Nearest,
        Linear
    };

    /// What sampling outside 0..1 returns
    enum class TextureWrap : std::uint8_t
    {
        Repeat,
        ClampToEdge
    };

    /**
     * How IRenderer::CreateTexture sets a texture up. The pixel format still comes from the channel
     * count: 1 channel is a single-channel (R8) texture, 2 is RG8, 3 is RGB8 and 4 is RGBA8.
     *
     * The defaults are what CreateTexture did before options existed (linear filtering, repeat, mipmaps),
     * so the overload without options is unchanged.
     */
    struct TextureOptions
    {
        TextureFilter filter = TextureFilter::Linear;
        TextureWrap wrap = TextureWrap::Repeat;
        /// Generate a mipmap chain (and, with Linear, sample it trilinearly when minifying)
        bool mipmaps = true;
        /// The pixels are sRGB colour (3 and 4 channels): OpenGL stores them as SRGB8 / SRGB8_ALPHA8, so every
        /// shader reads them as linear light, filtered in linear light; the software renderer keeps the bytes and
        /// its unlit and lit shaders decode them through a table after filtering. Either way it applies in any
        /// colour space, to whatever draws with the texture. Off by default, which is what every texture was made
        /// with before; the engine makes only a lit material's colour textures with it, in linear lighting.
        bool srgb = false;

        /// What the overload without options uses
        [[nodiscard]] static constexpr TextureOptions Default() { return TextureOptions{}; }

        /// For a single-channel SDF glyph atlas: linear (the shader thresholds the interpolated distance),
        /// clamped (glyphs at the atlas edge mustn't sample the opposite edge) and without mipmaps
        /// (averaging distances across glyphs at smaller mip levels would blur glyphs into each other)
        [[nodiscard]] static constexpr TextureOptions SdfAtlas()
        {
            TextureOptions options;
            options.filter = TextureFilter::Linear;
            options.wrap = TextureWrap::ClampToEdge;
            options.mipmaps = false;
            return options;
        }

        friend constexpr bool operator==(const TextureOptions &, const TextureOptions &) = default;
    };
}
