#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <renderer/common/SceneLighting.hpp>

namespace N2Engine::Rendering
{
    using Renderer::Common::ColorSpace;

    /// What a Material that has a user shader is drawn with when its shader cannot be (not cooked, stale, a compile error, or a backend
    /// without user shaders such as the software renderer): the editor wants it obvious, a shipped game wants nothing flashing
    enum class UserShaderErrorStyle : std::uint8_t
    {
        Unlit,   ///< the built-in unlit shader with the material's base colour and texture (the default: players)
        Magenta, ///< the built-in unlit shader in flat magenta (the editor host that edits)
    };

    /**
     * Engine-wide rendering settings, read by the lit shaders through Scene::CollectLighting. They come from a
     * project's `rendering` settings block (ApplyProjectSettings) or are set directly. Main thread.
     *
     * The colour space picks how the standard lit shader works with colour:
     *  - Gamma (the default; what the engine always did): colours, textures and lights are used as they are,
     *    and the lit result is written as it is.
     *  - Linear: textures whose Texture settings say `srgb` are decoded to linear light, lighting is added in
     *    linear light, and the result is encoded to sRGB for display. Numeric colours (base colour, vertex
     *    colour, emissive, light colours, ambient) are taken to be linear already, as glTF stores them.
     * Unlit and text drawing, and so the UI, are the same in both. Blending happens on the encoded result.
     */
    class RenderSettings
    {
    public:
        [[nodiscard]] static ColorSpace GetColorSpace();
        static void SetColorSpace(ColorSpace colorSpace);

        /// Linear lighting on OpenGL: have the lit shader encode its own output instead of the framebuffer
        /// (GL_FRAMEBUFFER_SRGB), even for a target that says it is sRGB. An escape hatch for a driver that
        /// reports the wrong encoding; off by default. The project setting is rendering.forceShaderEncode.
        [[nodiscard]] static bool GetForceShaderEncode();
        static void SetForceShaderEncode(bool force);

        /// How a material whose user shader cannot be drawn looks (UserShaderErrorStyle); Unlit until a host sets Magenta
        [[nodiscard]] static UserShaderErrorStyle GetUserShaderErrorStyle();
        static void SetUserShaderErrorStyle(UserShaderErrorStyle style);

        /// Whether a user shader may be compiled here when it has no usable cooked payload (stage 7 of user shaders). Off until a
        /// host or the project turns it on (the editor host does; a player's project setting is rendering.runtimeShaderCompile). It
        /// only matters for Vulkan, which needs the glslang of N2ENGINE_RUNTIME_SHADER_COMPILER: the OpenGL text needs no compiler
        [[nodiscard]] static bool GetRuntimeShaderCompile();
        static void SetRuntimeShaderCompile(bool enabled);

        /// The folder compiled shaders are kept in (one folder per key; see UserShaderCompiler). Empty (the default): not kept,
        /// compiled again whenever a Material needs it
        [[nodiscard]] static std::filesystem::path GetShaderCacheDirectory();
        static void SetShaderCacheDirectory(std::filesystem::path directory);

        /// "gamma" or "linear" (case-insensitive); nullopt for anything else
        [[nodiscard]] static std::optional<ColorSpace> ParseColorSpace(std::string_view name);
        [[nodiscard]] static std::string_view ColorSpaceName(ColorSpace colorSpace);
    };
}
