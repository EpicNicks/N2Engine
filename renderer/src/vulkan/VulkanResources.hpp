#pragma once

// PRIVATE to renderer/src/vulkan/: the Vulkan backend's GPU resources (stage A of #43) and the bookkeeping the renderer
// keeps for them. Includes VMA through VulkanVma.hpp, so it must never be included from a public header.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "VulkanVma.hpp"

#include "renderer/common/IMesh.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/common/TextureOptions.hpp"

namespace Renderer::Vulkan
{
    /// A mesh: a device-local vertex buffer of Common::Vertex (64 bytes each) and a uint32 index buffer. The index
    /// buffer is null (indexCount 0) for a mesh whose data had vertices but no indices: Vulkan buffers cannot be empty.
    /// UpdateMesh swaps the buffers for new ones, so a draw (stage B) must read them under the renderer's lock.
    class VulkanMesh final : public Common::IMesh
    {
    public:
        [[nodiscard]] bool IsValid() const override { return vertexBuffer != VK_NULL_HANDLE; }
        [[nodiscard]] uint32_t GetIndexCount() const override { return indexCount; }
        [[nodiscard]] uint32_t GetVertexCount() const override { return vertexCount; }

        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAllocation = nullptr;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAllocation = nullptr;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };

    /// How a cached sampler is made; the key of the sampler cache
    struct SamplerKey
    {
        VkFilter filter = VK_FILTER_LINEAR;
        VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        friend bool operator==(const SamplerKey&, const SamplerKey&) = default;
    };

    /// A texture: a device-local image with all its mip levels in SHADER_READ_ONLY_OPTIMAL, a view with the channel
    /// swizzle, and the (cached, renderer-owned) sampler for its options
    class VulkanTexture final : public Common::ITexture
    {
    public:
        [[nodiscard]] bool IsValid() const override { return image != VK_NULL_HANDLE; }
        [[nodiscard]] uint32_t GetWidth() const override { return width; }
        [[nodiscard]] uint32_t GetHeight() const override { return height; }
        [[nodiscard]] uint32_t GetChannels() const override { return channels; }

        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkSampler sampler = VK_NULL_HANDLE; // owned by the sampler cache
        SamplerKey samplerKey;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkComponentMapping swizzle{};
        VkImageUsageFlags usage = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 0; // as the caller gave them (a 3-channel texture is stored as RGBA8)
        uint32_t mipLevels = 1;
        Common::TextureOptions options;
    };

    /// GPU objects waiting for the frame that may still read them to finish (deferred deletion)
    struct RetiredResource
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
    };

    /// Everything stage A adds to the renderer, behind one pointer so the public header names none of it
    struct ResourceState
    {
        VmaAllocator allocator = nullptr;
        // The registries own the CPU objects. A pointer that is not a key is not this renderer's: foreign pointers are
        // only ever compared, never dereferenced
        std::unordered_map<const Common::IMesh*, std::unique_ptr<VulkanMesh>> meshes;
        std::unordered_map<const Common::ITexture*, std::unique_ptr<VulkanTexture>> textures;
        std::vector<std::pair<SamplerKey, VkSampler>> samplers;
        std::vector<std::vector<RetiredResource>> retired; // one list per frame slot
    };
}
