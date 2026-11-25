#include "engine/rendering/RenderTexture.hpp"

namespace N2Engine::Rendering
{
    std::shared_ptr<RenderTexture> RenderTexture::Create(const std::uint32_t width, const std::uint32_t height)
    {
        std::shared_ptr<Texture> texture = Texture::CreateRenderTarget(width, height);
        if (!texture)
        {
            return nullptr;
        }
        return std::make_shared<RenderTexture>(std::move(texture));
    }

    RenderTexture::~RenderTexture()
    {
        _handle.Release(true);
    }

    bool RenderTexture::Capture(Renderer::Common::IRenderer &renderer)
    {
        if (!_handle.Holds(&renderer))
        {
            _handle = GpuCache::AcquireTexture(renderer, _texture);
        }
        Renderer::Common::ITexture *texture = _handle.GetTexture();
        if (texture == nullptr || !renderer.CaptureFrame(texture))
        {
            return false;
        }
        ++_captures;
        return true;
    }

    Renderer::Common::ITexture *RenderTexture::GetGpuTexture(const Renderer::Common::IRenderer &renderer) const
    {
        return _handle.Holds(&renderer) ? _handle.GetTexture() : nullptr;
    }
}
