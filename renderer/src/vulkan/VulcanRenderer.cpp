#include "VulkanRenderer.hpp"

#include <cstring>

using namespace Renderer::Vulkan;

VulkanRenderer::VulkanRenderer()
{
    std::memset(&_ubo, 0, sizeof(_ubo));

    for (std::size_t i = 0; i < 4; i++)
    {
        _ubo.model[i * 4 + i] = 1.0f;
        _ubo.view[i * 4 + i] = 1.0f;
        _ubo.proj[i * 4 + i] = 1.0f;
    }
}