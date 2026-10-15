#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "renderer/vulkan/VulkanRenderer.hpp"

#include "VulkanDraw.hpp"
#include "VulkanResources.hpp"

// Stage A of the Vulkan backend (#43): meshes, textures, samplers, staging uploads, blit mipmaps and deferred deletion.
//
// Allocation: AMD VulkanMemoryAllocator (VulkanVma.hpp), created after the device and destroyed before it. Buffers and
// images are device-local (VMA_MEMORY_USAGE_AUTO without host access); uploads go through a host-visible staging buffer.
//
// Uploads are SYNCHRONOUS: each CreateMesh / UpdateMesh / CreateTexture records its copies into a one-time command
// buffer (VulkanRenderer::SubmitOneTime) and waits for the queue to go idle before it returns, so the staging buffer
// is freed at once. That costs a queue stall per call (a frame still in flight is waited for too), which is the
// simple, correct choice for stage A; a staging ring recorded into the frame's own command buffer is a later
// optimisation. It is correct inside or outside an open frame (BeginFrame..EndFrame): the one-time submission runs
// before the frame's own command buffer is submitted, and ends with a barrier that makes the data visible to every
// later command on the queue.
//
// Deferred deletion: a buffer or image the GPU may still read is never destroyed or overwritten at once. UpdateMesh
// makes NEW buffers and retires the old ones; DestroyMesh and DestroyTexture retire the whole resource. Retired objects
// go on the list of a frame slot (RetireSlot) and are freed by BeginFrame right after it has waited for that slot's
// fence (FlushRetired); Shutdown frees whatever is left after waiting for the device to go idle. The exception is when
// no frame is open and every frame fence is signalled (CanFreeRetiredNow): nothing can use the object, so it is freed
// at once and does not pile up while no frames run.

using namespace Renderer::Vulkan;
using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Common::Vertex;

namespace
{
    using Recorder = std::function<void(VkCommandBuffer)>;
    using Submitter = std::function<bool(const Recorder&)>;

    constexpr VkPipelineStageFlags SamplingStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
    };

    struct GpuMesh
    {
        GpuBuffer vertex;
        GpuBuffer index;
    };

    bool CreateGpuBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                         const VmaAllocationCreateInfo& allocationInfo, GpuBuffer& out)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const VkResult result =
            vmaCreateBuffer(allocator, &info, &allocationInfo, &out.buffer, &out.allocation, nullptr);
        if (result != VK_SUCCESS)
        {
            out = {};
            std::cerr << "Vulkan renderer: could not allocate a buffer of " << size << " bytes (result " << result << ")"
                      << std::endl;
            return false;
        }
        return true;
    }

    void DestroyGpuBuffer(VmaAllocator allocator, GpuBuffer& buffer)
    {
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
        }
        buffer = {};
    }

    /// Host-visible memory the CPU writes sequentially and the GPU reads once
    bool CreateStagingBuffer(VmaAllocator allocator, VkDeviceSize size, GpuBuffer& out)
    {
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        return CreateGpuBuffer(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, allocationInfo, out);
    }

    /// Host-visible memory the CPU reads back (test hooks)
    bool CreateReadbackStaging(VmaAllocator allocator, VkDeviceSize size, GpuBuffer& out)
    {
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        return CreateGpuBuffer(allocator, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, allocationInfo, out);
    }

    /// Uploads the vertices and indices of `data` into new device-local buffers (the index buffer stays null without
    /// indices). Returns false, leaving `out` empty and nothing allocated, on any failure
    bool UploadMesh(VmaAllocator allocator, const Submitter& submit, const MeshData& data, GpuMesh& out)
    {
        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(data.vertices.size()) * sizeof(Vertex);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(data.indices.size()) * sizeof(uint32_t);

        // One staging buffer holds the vertices, then the indices (vertexBytes is a multiple of 64, so the indices
        // start 4-byte aligned)
        GpuBuffer staging;
        if (!CreateStagingBuffer(allocator, vertexBytes + indexBytes, staging))
        {
            return false;
        }
        bool ok = vmaCopyMemoryToAllocation(allocator, data.vertices.data(), staging.allocation, 0, vertexBytes) ==
            VK_SUCCESS;
        if (ok && indexBytes > 0)
        {
            ok = vmaCopyMemoryToAllocation(allocator, data.indices.data(), staging.allocation, vertexBytes,
                                           indexBytes) == VK_SUCCESS;
        }

        // TRANSFER_SRC as well, so the test hook can read the buffers back
        constexpr VkBufferUsageFlags transfer = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo deviceLocal{};
        deviceLocal.usage = VMA_MEMORY_USAGE_AUTO;
        GpuMesh mesh;
        ok = ok && CreateGpuBuffer(allocator, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | transfer, deviceLocal,
                                   mesh.vertex);
        if (ok && indexBytes > 0)
        {
            ok = CreateGpuBuffer(allocator, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | transfer, deviceLocal,
                                 mesh.index);
        }

        if (ok)
        {
            const VkBuffer stagingBuffer = staging.buffer;
            const VkBuffer vertexBuffer = mesh.vertex.buffer;
            const VkBuffer indexBuffer = mesh.index.buffer;
            ok = submit(
                [=](VkCommandBuffer commandBuffer)
                {
                    VkBufferCopy vertexCopy{};
                    vertexCopy.srcOffset = 0;
                    vertexCopy.dstOffset = 0;
                    vertexCopy.size = vertexBytes;
                    vkCmdCopyBuffer(commandBuffer, stagingBuffer, vertexBuffer, 1, &vertexCopy);
                    if (indexBytes > 0)
                    {
                        VkBufferCopy indexCopy{};
                        indexCopy.srcOffset = vertexBytes;
                        indexCopy.dstOffset = 0;
                        indexCopy.size = indexBytes;
                        vkCmdCopyBuffer(commandBuffer, stagingBuffer, indexBuffer, 1, &indexCopy);
                    }

                    // Makes the copies visible to every vertex fetch recorded after this submission
                    VkMemoryBarrier barrier{};
                    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
                    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
                });
        }

        DestroyGpuBuffer(allocator, staging); // the submission has finished (the queue went idle)
        if (!ok)
        {
            DestroyGpuBuffer(allocator, mesh.vertex);
            DestroyGpuBuffer(allocator, mesh.index);
            return false;
        }
        out = mesh;
        return true;
    }

    VkFormatFeatureFlags FormatFeatures(VkPhysicalDevice device, VkFormat format)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device, format, &properties);
        return properties.optimalTilingFeatures;
    }

    bool HasFeatures(VkFormatFeatureFlags available, VkFormatFeatureFlags wanted)
    {
        return (available & wanted) == wanted;
    }

    uint32_t BytesPerTexel(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_R8_UNORM: return 1;
        case VK_FORMAT_R8G8_UNORM: return 2;
        default: return 4; // RGBA8, UNORM or sRGB
        }
    }

    uint32_t MipCount(uint32_t width, uint32_t height)
    {
        uint32_t levels = 1;
        for (uint32_t size = std::max(width, height); size > 1; size >>= 1)
        {
            ++levels;
        }
        return levels;
    }

    VkImageMemoryBarrier LevelBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess, uint32_t baseMip,
                                      uint32_t levels)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = baseMip;
        barrier.subresourceRange.levelCount = levels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        return barrier;
    }

    void PipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                         const VkImageMemoryBarrier* barriers, uint32_t count)
    {
        vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, count, barriers);
    }

    /// The cached sampler for `key`, made on first use; null on failure
    VkSampler FindOrCreateSampler(VkDevice device, ResourceState& state, const SamplerKey& key)
    {
        for (const auto& [existingKey, sampler] : state.samplers)
        {
            if (existingKey == key)
            {
                return sampler;
            }
        }

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = key.filter;
        info.minFilter = key.filter;
        info.mipmapMode = key.mipmapMode;
        info.addressModeU = key.addressMode;
        info.addressModeV = key.addressMode;
        info.addressModeW = key.addressMode;
        info.mipLodBias = 0.0f;
        info.anisotropyEnable = VK_FALSE; // samplerAnisotropy is not enabled on the device
        info.maxAnisotropy = 1.0f;
        info.compareEnable = VK_FALSE;
        info.compareOp = VK_COMPARE_OP_ALWAYS;
        info.minLod = 0.0f;
        info.maxLod = VK_LOD_CLAMP_NONE; // an image with one level clamps to it
        info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        state.samplers.emplace_back(key, sampler);
        return sampler;
    }

    void DestroyRetired(VkDevice device, VmaAllocator allocator, RetiredResource& resource)
    {
        if (resource.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, resource.view, nullptr);
        }
        if (resource.image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator, resource.image, resource.allocation);
        }
        if (resource.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator, resource.buffer, resource.allocation);
        }
        resource = {};
    }

    RetiredResource RetiredBuffer(VkBuffer buffer, VmaAllocation allocation)
    {
        RetiredResource resource;
        resource.buffer = buffer;
        resource.allocation = allocation;
        return resource;
    }

    // Frees `items` now when `freeNow`, else queues them on `slot`'s list for FlushRetired
    void Dispose(ResourceState& state, VkDevice device, bool freeNow, uint32_t slot, std::vector<RetiredResource>& items)
    {
        for (RetiredResource& item : items)
        {
            if (freeNow)
            {
                DestroyRetired(device, state.allocator, item);
            }
            else
            {
                state.retired[slot].push_back(item);
            }
        }
        items.clear();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The allocator, deferred deletion and shutdown
// ---------------------------------------------------------------------------------------------------------------------

bool VulkanRenderer::CreateAllocator()
{
    VmaAllocatorCreateInfo info{};
    info.vulkanApiVersion = VK_API_VERSION_1_1; // the instance's API version
    info.instance = _instance;
    info.physicalDevice = _physicalDevice;
    info.device = _device;
    const VkResult result = vmaCreateAllocator(&info, &_resources->allocator);
    if (result != VK_SUCCESS)
    {
        _resources->allocator = nullptr;
        return Fail(InitStatus::Failed, "vmaCreateAllocator failed (result " + std::to_string(result) + ")");
    }
    _resources->retired.assign(MAX_FRAMES_IN_FLIGHT, {});
    return true;
}

void VulkanRenderer::DestroyAllocator()
{
    if (_resources->allocator != nullptr)
    {
        vmaDestroyAllocator(_resources->allocator); // every allocation was freed by DestroyAllResources
        _resources->allocator = nullptr;
    }
}

uint32_t VulkanRenderer::RetireSlot() const
{
    // A frame being recorded may use what is retired now, and it signals the fence of _currentFrame; with no frame open
    // the newest frame that can use it is the one submitted last, which used the previous slot
    return _frameOpen ? _currentFrame : (_currentFrame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
}

bool VulkanRenderer::CanFreeRetiredNow() const
{
    // Free at once only when the GPU provably cannot touch the object: no frame is being recorded (it could reference
    // the object and is submitted later), and every frame fence is signalled (a fence is reset only by EndFrame right
    // before the submit that signals it again, so a signalled fence means that slot's frame has finished). The queue is
    // single and in-order, and UpdateMesh's synchronous upload has already waited for the queue to go idle, so after it
    // the fences are signalled too. Any doubt (an unsignalled fence, a query error such as a lost device) keeps the
    // deferred path, which waits for the slot's fence in BeginFrame before freeing.
    if (_frameOpen)
    {
        return false;
    }
    for (const FrameResources& frame : _frames)
    {
        if (frame.fence == VK_NULL_HANDLE || vkGetFenceStatus(_device, frame.fence) != VK_SUCCESS)
        {
            return false;
        }
    }
    return true;
}

void VulkanRenderer::FlushRetired(uint32_t slot)
{
    if (slot >= _resources->retired.size() || _resources->allocator == nullptr)
    {
        return;
    }
    std::vector<RetiredResource>& list = _resources->retired[slot];
    for (RetiredResource& resource : list)
    {
        DestroyRetired(_device, _resources->allocator, resource);
    }
    list.clear();
}

void VulkanRenderer::DestroyAllResources()
{
    ResourceState& state = *_resources;
    if (_device != VK_NULL_HANDLE && state.allocator != nullptr)
    {
        // The device is idle: free what was waiting for a fence, then what the caller never destroyed
        for (std::vector<RetiredResource>& list : state.retired)
        {
            for (RetiredResource& resource : list)
            {
                DestroyRetired(_device, state.allocator, resource);
            }
            list.clear();
        }
        for (auto& [key, mesh] : state.meshes)
        {
            RetiredResource vertex = RetiredBuffer(mesh->vertexBuffer, mesh->vertexAllocation);
            RetiredResource index = RetiredBuffer(mesh->indexBuffer, mesh->indexAllocation);
            DestroyRetired(_device, state.allocator, vertex);
            DestroyRetired(_device, state.allocator, index);
        }
        for (auto& [key, texture] : state.textures)
        {
            RetiredResource resource;
            resource.view = texture->view;
            resource.image = texture->image;
            resource.allocation = texture->allocation;
            DestroyRetired(_device, state.allocator, resource);
        }
    }
    state.meshes.clear();
    state.textures.clear();
    for (const auto& [key, sampler] : state.samplers)
    {
        if (_device != VK_NULL_HANDLE && sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(_device, sampler, nullptr);
        }
    }
    state.samplers.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------------------------------------------------

IMesh* VulkanRenderer::CreateMesh(const MeshData& meshData)
{
    if (meshData.vertices.empty())
    {
        return nullptr; // as the other renderers: a mesh needs vertices
    }
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost || _resources->allocator == nullptr)
    {
        return nullptr;
    }

    GpuMesh gpu;
    const Submitter submit = [this](const Recorder& record) { return SubmitOneTimeFn(record); };
    if (!UploadMesh(_resources->allocator, submit, meshData, gpu))
    {
        return nullptr;
    }

    auto mesh = std::make_unique<VulkanMesh>();
    mesh->vertexBuffer = gpu.vertex.buffer;
    mesh->vertexAllocation = gpu.vertex.allocation;
    mesh->indexBuffer = gpu.index.buffer;
    mesh->indexAllocation = gpu.index.allocation;
    mesh->vertexCount = static_cast<uint32_t>(meshData.vertices.size());
    mesh->indexCount = static_cast<uint32_t>(meshData.indices.size());
    IMesh* raw = mesh.get();
    _resources->meshes.emplace(raw, std::move(mesh));
    return raw;
}

void VulkanRenderer::DestroyMesh(IMesh* mesh)
{
    if (mesh == nullptr)
    {
        return;
    }
    std::lock_guard lock(_mutex);
    const auto it = _resources->meshes.find(mesh);
    if (it == _resources->meshes.end() || _resources->allocator == nullptr)
    {
        return; // not this renderer's (or already destroyed, or the renderer was shut down)
    }

    // A frame in flight may still read the buffers: free them once that frame has finished (at once if none can)
    std::vector<RetiredResource> items;
    items.push_back(RetiredBuffer(it->second->vertexBuffer, it->second->vertexAllocation));
    if (it->second->indexBuffer != VK_NULL_HANDLE)
    {
        items.push_back(RetiredBuffer(it->second->indexBuffer, it->second->indexAllocation));
    }
    Dispose(*_resources, _device, CanFreeRetiredNow(), RetireSlot(), items);
    _resources->meshes.erase(it);
}

bool VulkanRenderer::UpdateMesh(IMesh* mesh, const MeshData& meshData)
{
    if (mesh == nullptr || meshData.vertices.empty())
    {
        return false;
    }
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost || _resources->allocator == nullptr)
    {
        return false;
    }
    const auto it = _resources->meshes.find(mesh);
    if (it == _resources->meshes.end())
    {
        return false;
    }

    // New buffers, never a write into the old ones: a frame in flight may be reading them
    GpuMesh gpu;
    const Submitter submit = [this](const Recorder& record) { return SubmitOneTimeFn(record); };
    if (!UploadMesh(_resources->allocator, submit, meshData, gpu))
    {
        return false; // the mesh is unchanged
    }

    VulkanMesh& target = *it->second;
    std::vector<RetiredResource> items;
    items.push_back(RetiredBuffer(target.vertexBuffer, target.vertexAllocation));
    if (target.indexBuffer != VK_NULL_HANDLE)
    {
        items.push_back(RetiredBuffer(target.indexBuffer, target.indexAllocation));
    }
    Dispose(*_resources, _device, CanFreeRetiredNow(), RetireSlot(), items);
    target.vertexBuffer = gpu.vertex.buffer;
    target.vertexAllocation = gpu.vertex.allocation;
    target.indexBuffer = gpu.index.buffer;
    target.indexAllocation = gpu.index.allocation;
    target.vertexCount = static_cast<uint32_t>(meshData.vertices.size());
    target.indexCount = static_cast<uint32_t>(meshData.indices.size());
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------------------

ITexture* VulkanRenderer::CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels)
{
    return CreateTexture(data, width, height, channels, TextureOptions::Default());
}

ITexture* VulkanRenderer::CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels,
                                        const TextureOptions& options)
{
    if (data == nullptr || width == 0 || height == 0 || channels < 1 || channels > 4)
    {
        return nullptr;
    }
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost || _resources->allocator == nullptr)
    {
        return nullptr;
    }
    std::unique_ptr<VulkanTexture> texture = MakeTextureLocked(data, width, height, channels, options);
    if (!texture)
    {
        return nullptr;
    }
    ITexture* raw = texture.get();
    {
        // A new id from a counter that never repeats: the address may be one a destroyed texture had
        std::lock_guard idLock(_draw->textureIdMutex);
        _draw->textureIds[raw] = _draw->nextTextureId++;
    }
    _resources->textures.emplace(raw, std::move(texture));
    return raw;
}

std::unique_ptr<VulkanTexture> VulkanRenderer::MakeTextureLocked(const uint8_t* data, uint32_t width, uint32_t height,
                                                                 uint32_t channels, const TextureOptions& options)
{
    const VkPhysicalDeviceLimits& limits = _deviceProperties.limits;
    if (width > limits.maxImageDimension2D || height > limits.maxImageDimension2D)
    {
        std::cerr << "Vulkan renderer: a " << width << "x" << height << " texture is over the device's limit of "
                  << limits.maxImageDimension2D << std::endl;
        return nullptr;
    }

    // The format follows the channel count: R8, RG8, and RGBA8 for 3 and 4 (RGB8 is not a sampled format everywhere,
    // so 3 channels are expanded with alpha 255). sRGB applies to 3 and 4 channels only, as in the OpenGL renderer
    // (a 1 or 2 channel texture is data, and R8/RG8 sRGB are optional formats anyway); when the device cannot sample
    // RGBA8 sRGB the texture falls back to UNORM and its colours are no longer decoded on sampling.
    constexpr VkFormatFeatureFlags sampledUpload =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormat format = channels == 1 ? VK_FORMAT_R8_UNORM
        : channels == 2          ? VK_FORMAT_R8G8_UNORM
                                 : VK_FORMAT_R8G8B8A8_UNORM;
    if (channels >= 3 && options.srgb)
    {
        if (HasFeatures(FormatFeatures(_physicalDevice, VK_FORMAT_R8G8B8A8_SRGB), sampledUpload))
        {
            format = VK_FORMAT_R8G8B8A8_SRGB;
        }
        else
        {
            LogOnce("the device cannot sample R8G8B8A8_SRGB: sRGB textures are stored as UNORM (not decoded on sampling)");
        }
    }
    const VkFormatFeatureFlags features = FormatFeatures(_physicalDevice, format);
    if (!HasFeatures(features, sampledUpload))
    {
        LogOnce("the device cannot sample or upload a texture format the renderer needs");
        return nullptr;
    }

    // Mipmaps are blitted, so they need the format to be a linear-filterable blit source and destination
    constexpr VkFormatFeatureFlags blitMips = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    uint32_t mipLevels = 1;
    if (options.mipmaps)
    {
        if (HasFeatures(features, blitMips))
        {
            mipLevels = MipCount(width, height);
        }
        else
        {
            LogOnce("the device cannot linearly blit the texture format: textures get a single mip level");
        }
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (HasFeatures(features, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
    {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // mip blits read it; so does the test hook
    }
    VkImageFormatProperties formatProperties{};
    if (vkGetPhysicalDeviceImageFormatProperties(_physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                 usage, 0, &formatProperties) != VK_SUCCESS ||
        width > formatProperties.maxExtent.width || height > formatProperties.maxExtent.height)
    {
        std::cerr << "Vulkan renderer: the device cannot make a " << width << "x" << height << " texture of this format"
                  << std::endl;
        return nullptr;
    }
    mipLevels = std::min(mipLevels, std::max(1u, formatProperties.maxMipLevels));

    // Pixels in the texture's format, rows tightly packed (so any width works)
    const uint32_t texelBytes = BytesPerTexel(format);
    const VkDeviceSize imageBytes = static_cast<VkDeviceSize>(width) * height * texelBytes;
    std::vector<uint8_t> expanded;
    const uint8_t* pixels = data;
    if (channels == 3)
    {
        const std::size_t count = static_cast<std::size_t>(width) * height;
        expanded.resize(count * 4);
        for (std::size_t i = 0; i < count; ++i)
        {
            expanded[i * 4 + 0] = data[i * 3 + 0];
            expanded[i * 4 + 1] = data[i * 3 + 1];
            expanded[i * 4 + 2] = data[i * 3 + 2];
            expanded[i * 4 + 3] = 255;
        }
        pixels = expanded.data();
    }

    VmaAllocator allocator = _resources->allocator;
    GpuBuffer staging;
    if (!CreateStagingBuffer(allocator, imageBytes, staging))
    {
        return nullptr;
    }
    if (vmaCopyMemoryToAllocation(allocator, pixels, staging.allocation, 0, imageBytes) != VK_SUCCESS)
    {
        DestroyGpuBuffer(allocator, staging);
        return nullptr;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    if (vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image, &allocation, nullptr) != VK_SUCCESS)
    {
        DestroyGpuBuffer(allocator, staging);
        std::cerr << "Vulkan renderer: could not allocate a " << width << "x" << height << " texture" << std::endl;
        return nullptr;
    }

    // A 1 channel texture samples (r, 0, 0, 1) and a 2 channel one (r, g, 0, 1), as in OpenGL and the software renderer
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY};
    if (channels == 1)
    {
        viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                               VK_COMPONENT_SWIZZLE_ONE};
    }
    else if (channels == 2)
    {
        viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO,
                               VK_COMPONENT_SWIZZLE_ONE};
    }
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        vmaDestroyImage(allocator, image, allocation);
        DestroyGpuBuffer(allocator, staging);
        return nullptr;
    }

    // The sampler: nearest or linear min/mag, the mipmap mode to match, repeat or clamp-to-edge. A format that cannot
    // be filtered linearly samples nearest. Anisotropy stays off
    SamplerKey samplerKey;
    const bool canFilter = HasFeatures(features, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    samplerKey.filter = (options.filter == TextureFilter::Linear && canFilter) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerKey.mipmapMode =
        samplerKey.filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerKey.addressMode = options.wrap == TextureWrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                                                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkSampler sampler = FindOrCreateSampler(_device, *_resources, samplerKey);

    // Upload level 0 and build the rest of the chain, every level ending in SHADER_READ_ONLY_OPTIMAL
    bool ok = sampler != VK_NULL_HANDLE;
    if (ok)
    {
        const VkBuffer stagingBuffer = staging.buffer;
        ok = SubmitOneTimeFn(
            [=](VkCommandBuffer commandBuffer)
            {
                VkImageMemoryBarrier toTransferDst =
                    LevelBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, 0, mipLevels);
                PipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                &toTransferDst, 1);

                VkBufferImageCopy region{};
                region.bufferOffset = 0;
                region.bufferRowLength = 0; // tightly packed
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);

                int32_t sourceWidth = static_cast<int32_t>(width);
                int32_t sourceHeight = static_cast<int32_t>(height);
                for (uint32_t level = 1; level < mipLevels; ++level)
                {
                    // The level just written becomes the blit's source
                    VkImageMemoryBarrier toSource =
                        LevelBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, level - 1, 1);
                    PipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    &toSource, 1);

                    const int32_t destinationWidth = std::max(1, sourceWidth / 2);
                    const int32_t destinationHeight = std::max(1, sourceHeight / 2);
                    VkImageBlit blit{};
                    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    blit.srcSubresource.mipLevel = level - 1;
                    blit.srcSubresource.baseArrayLayer = 0;
                    blit.srcSubresource.layerCount = 1;
                    blit.srcOffsets[0] = {0, 0, 0};
                    blit.srcOffsets[1] = {sourceWidth, sourceHeight, 1};
                    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    blit.dstSubresource.mipLevel = level;
                    blit.dstSubresource.baseArrayLayer = 0;
                    blit.dstSubresource.layerCount = 1;
                    blit.dstOffsets[0] = {0, 0, 0};
                    blit.dstOffsets[1] = {destinationWidth, destinationHeight, 1};
                    vkCmdBlitImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                    sourceWidth = destinationWidth;
                    sourceHeight = destinationHeight;
                }

                // Levels 0..n-2 are blit sources (TRANSFER_SRC); the last level is still TRANSFER_DST
                VkImageMemoryBarrier toSampled[2];
                uint32_t barrierCount = 0;
                if (mipLevels > 1)
                {
                    toSampled[barrierCount++] =
                        LevelBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                                     VK_ACCESS_SHADER_READ_BIT, 0, mipLevels - 1);
                }
                toSampled[barrierCount++] =
                    LevelBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, mipLevels - 1, 1);
                PipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, SamplingStages, toSampled,
                                barrierCount);
            });
    }
    DestroyGpuBuffer(allocator, staging); // the submission has finished (the queue went idle), or never ran
    if (!ok)
    {
        vkDestroyImageView(_device, view, nullptr);
        vmaDestroyImage(allocator, image, allocation);
        return nullptr;
    }

    auto texture = std::make_unique<VulkanTexture>();
    texture->image = image;
    texture->view = view;
    texture->allocation = allocation;
    texture->sampler = sampler;
    texture->samplerKey = samplerKey;
    texture->format = format;
    texture->swizzle = viewInfo.components;
    texture->usage = usage;
    texture->width = width;
    texture->height = height;
    texture->channels = channels;
    texture->mipLevels = mipLevels;
    texture->options = options;
    return texture;
}

void VulkanRenderer::DestroyTexture(ITexture* texture)
{
    if (texture == nullptr)
    {
        return;
    }
    std::lock_guard lock(_mutex);
    const auto it = _resources->textures.find(texture);
    if (it == _resources->textures.end() || _resources->allocator == nullptr)
    {
        return; // not this renderer's (or already destroyed, or the renderer was shut down)
    }

    RetiredResource resource;
    resource.view = it->second->view;
    resource.image = it->second->image;
    resource.allocation = it->second->allocation;
    std::vector<RetiredResource> items{resource}; // the sampler stays in the cache
    Dispose(*_resources, _device, CanFreeRetiredNow(), RetireSlot(), items);
    _resources->textures.erase(it);
    {
        std::lock_guard idLock(_draw->textureIdMutex);
        _draw->textureIds.erase(texture);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Test and diagnostic hooks
// ---------------------------------------------------------------------------------------------------------------------

TextureInfoForTest VulkanRenderer::GetTextureInfoForTest(const ITexture* texture) const
{
    TextureInfoForTest info;
    std::lock_guard lock(_mutex);
    const auto it = _resources->textures.find(texture);
    if (it == _resources->textures.end())
    {
        return info;
    }
    const VulkanTexture& t = *it->second;
    info.valid = true;
    info.format = t.format;
    info.width = t.width;
    info.height = t.height;
    info.channels = t.channels;
    info.mipLevels = t.mipLevels;
    info.filter = t.samplerKey.filter;
    info.mipmapMode = t.samplerKey.mipmapMode;
    info.addressMode = t.samplerKey.addressMode;
    info.sampler = t.sampler;
    info.swizzle = t.swizzle;
    return info;
}

bool VulkanRenderer::ReadTextureForTest(const ITexture* texture, uint32_t mipLevel, std::vector<uint8_t>& bytes,
                                        uint32_t* levelWidth, uint32_t* levelHeight)
{
    bytes.clear();
    std::lock_guard lock(_mutex);
    const auto it = _resources->textures.find(texture);
    if (it == _resources->textures.end() || !_initialized || _deviceLost)
    {
        return false;
    }
    const VulkanTexture& t = *it->second;
    if (mipLevel >= t.mipLevels || (t.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
    {
        return false;
    }
    const uint32_t w = std::max(1u, t.width >> mipLevel);
    const uint32_t h = std::max(1u, t.height >> mipLevel);
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * BytesPerTexel(t.format);

    VmaAllocator allocator = _resources->allocator;
    GpuBuffer readback;
    if (!CreateReadbackStaging(allocator, size, readback))
    {
        return false;
    }

    const VkImage image = t.image;
    const VkBuffer readbackBuffer = readback.buffer;
    const bool copied = SubmitOneTimeFn(
        [=](VkCommandBuffer commandBuffer)
        {
            VkImageMemoryBarrier toSource =
                LevelBarrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, mipLevel, 1);
            PipelineBarrier(commandBuffer, SamplingStages, VK_PIPELINE_STAGE_TRANSFER_BIT, &toSource, 1);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = mipLevel;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1,
                                   &region);

            VkImageMemoryBarrier toSampled =
                LevelBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, mipLevel, 1);
            PipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, SamplingStages, &toSampled, 1);

            VkBufferMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = readbackBuffer;
            toHost.offset = 0;
            toHost.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                 nullptr, 1, &toHost, 0, nullptr);
        });

    bool ok = copied;
    if (ok)
    {
        bytes.resize(static_cast<std::size_t>(size));
        ok = vmaCopyAllocationToMemory(allocator, readback.allocation, 0, bytes.data(), size) == VK_SUCCESS;
    }
    DestroyGpuBuffer(allocator, readback);
    if (!ok)
    {
        bytes.clear();
        return false;
    }
    if (levelWidth != nullptr)
    {
        *levelWidth = w;
    }
    if (levelHeight != nullptr)
    {
        *levelHeight = h;
    }
    return true;
}

bool VulkanRenderer::ReadMeshForTest(const IMesh* mesh, MeshData& out)
{
    out = {};
    std::lock_guard lock(_mutex);
    const auto it = _resources->meshes.find(mesh);
    if (it == _resources->meshes.end() || !_initialized || _deviceLost)
    {
        return false;
    }
    const VulkanMesh& m = *it->second;
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m.vertexCount) * sizeof(Vertex);
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(m.indexCount) * sizeof(uint32_t);

    VmaAllocator allocator = _resources->allocator;
    GpuBuffer readback;
    if (!CreateReadbackStaging(allocator, vertexBytes + indexBytes, readback))
    {
        return false;
    }
    const VkBuffer vertexBuffer = m.vertexBuffer;
    const VkBuffer indexBuffer = m.indexBuffer;
    const VkBuffer readbackBuffer = readback.buffer;
    const bool copied = SubmitOneTimeFn(
        [=](VkCommandBuffer commandBuffer)
        {
            VkMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &toTransfer, 0, nullptr, 0, nullptr);

            VkBufferCopy vertexCopy{};
            vertexCopy.srcOffset = 0;
            vertexCopy.dstOffset = 0;
            vertexCopy.size = vertexBytes;
            vkCmdCopyBuffer(commandBuffer, vertexBuffer, readbackBuffer, 1, &vertexCopy);
            if (indexBytes > 0)
            {
                VkBufferCopy indexCopy{};
                indexCopy.srcOffset = 0;
                indexCopy.dstOffset = vertexBytes;
                indexCopy.size = indexBytes;
                vkCmdCopyBuffer(commandBuffer, indexBuffer, readbackBuffer, 1, &indexCopy);
            }

            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                 &toHost, 0, nullptr, 0, nullptr);
        });

    bool ok = copied;
    if (ok)
    {
        out.vertices.resize(m.vertexCount);
        out.indices.resize(m.indexCount);
        ok = vmaCopyAllocationToMemory(allocator, readback.allocation, 0, out.vertices.data(), vertexBytes) ==
            VK_SUCCESS;
        if (ok && indexBytes > 0)
        {
            ok = vmaCopyAllocationToMemory(allocator, readback.allocation, vertexBytes, out.indices.data(),
                                           indexBytes) == VK_SUCCESS;
        }
    }
    DestroyGpuBuffer(allocator, readback);
    if (!ok)
    {
        out = {};
    }
    return ok;
}

uint32_t VulkanRenderer::GetLiveAllocationCountForTest() const
{
    std::lock_guard lock(_mutex);
    if (_resources->allocator == nullptr)
    {
        return 0;
    }
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(_resources->allocator, &statistics);
    // The renderer's own allocations (the uniform rings, the lighting buffer, the 1x1 default textures: stage B) are not
    // the caller's, so they stay out of this count
    const uint32_t total = statistics.total.statistics.allocationCount;
    const uint32_t internal = _draw->internalAllocations;
    return total > internal ? total - internal : 0;
}

uint32_t VulkanRenderer::GetLiveMeshCountForTest() const
{
    std::lock_guard lock(_mutex);
    return static_cast<uint32_t>(_resources->meshes.size());
}

uint32_t VulkanRenderer::GetLiveTextureCountForTest() const
{
    std::lock_guard lock(_mutex);
    return static_cast<uint32_t>(_resources->textures.size());
}

uint32_t VulkanRenderer::GetPendingDeletionCountForTest() const
{
    std::lock_guard lock(_mutex);
    std::size_t count = 0;
    for (const std::vector<RetiredResource>& list : _resources->retired)
    {
        count += list.size();
    }
    return static_cast<uint32_t>(count);
}
