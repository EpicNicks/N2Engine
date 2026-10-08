#pragma once

#include <optional>
#include <string_view>

#include <renderer/common/SceneLighting.hpp>

namespace N2Engine::Rendering
{
    using Renderer::Common::ColorSpace;

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

        /// "gamma" or "linear" (case-insensitive); nullopt for anything else
        [[nodiscard]] static std::optional<ColorSpace> ParseColorSpace(std::string_view name);
        [[nodiscard]] static std::string_view ColorSpaceName(ColorSpace colorSpace);
    };
}
