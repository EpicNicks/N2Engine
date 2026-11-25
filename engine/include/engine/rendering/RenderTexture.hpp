#pragma once

#include <cstdint>
#include <memory>

#include <renderer/common/Renderer.hpp>

#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Texture.hpp"

namespace N2Engine::Rendering
{
    /**
     * A texture a frame can be drawn into (issue #445; docs/post-processing.html#render-textures): a render-target Texture
     * (Texture::CreateRenderTarget) plus this object's share of its GPU texture, so what was captured stays on the GPU while
     * the RenderTexture lives, whether or not anything draws it. Show it with Sprite2D::SetTexture or UI::Image::SetSprite of
     * GetTexture() (Lua: Sprite2D:SetRenderTexture, Image:SetRenderTexture). Fill it with Capture, which the Application calls
     * for a camera's target texture every frame and for Camera:RenderToTexture once.
     *
     * Main thread only. The GPU texture is freed with the last user: this object and every sprite or image showing it.
     */
    class RenderTexture
    {
    public:
        /// A width x height render texture, transparent black until the first capture; nullptr (and an error logged) for a size
        /// Texture::CreateRenderTarget refuses
        [[nodiscard]] static std::shared_ptr<RenderTexture> Create(std::uint32_t width, std::uint32_t height);

        explicit RenderTexture(std::shared_ptr<Texture> texture) : _texture(std::move(texture)) {}
        /// Gives up its GPU share, freeing the GPU texture on its renderer (while that is alive) if nothing else shows it
        ~RenderTexture();
        RenderTexture(const RenderTexture &) = delete;
        RenderTexture &operator=(const RenderTexture &) = delete;

        [[nodiscard]] const std::shared_ptr<Texture> &GetTexture() const { return _texture; }
        [[nodiscard]] std::uint32_t GetWidth() const { return _texture->GetWidth(); }
        [[nodiscard]] std::uint32_t GetHeight() const { return _texture->GetHeight(); }

        /// Copies what `renderer`'s open frame has drawn so far into the texture (IRenderer::CaptureFrame), making the GPU
        /// texture there first if needed. False when the renderer cannot (no frame open, no capture support)
        bool Capture(Renderer::Common::IRenderer &renderer);
        /// How many captures succeeded (tests and diagnostics)
        [[nodiscard]] std::uint64_t GetCaptureCount() const { return _captures; }
        /// The GPU texture on `renderer`, or null before the first capture there (tests: read it back)
        [[nodiscard]] Renderer::Common::ITexture *GetGpuTexture(const Renderer::Common::IRenderer &renderer) const;

    private:
        std::shared_ptr<Texture> _texture;
        GpuCache::Handle _handle;
        std::uint64_t _captures = 0;
    };
}
