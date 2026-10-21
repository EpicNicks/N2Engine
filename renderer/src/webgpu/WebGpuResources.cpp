#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "renderer/webgpu/WebGpuRenderer.hpp"

#include "WebGpuInternal.hpp"

// Stage G3a of the WebGPU backend (#155): meshes, textures and samplers. Written against webgpu.h (the C API) only.
//
// Uploads are QUEUE WRITES (wgpuQueueWriteBuffer / wgpuQueueWriteTexture): the data is copied at the call and applied on the
// queue's timeline in call order, so an upload works inside or outside an open frame, and what is submitted after it sees it
// while what was submitted before it does not. Nothing here waits.
//
// Lifetimes: there is no deferred deletion (the Vulkan backend's retire lists and frame fences are not needed). Destroying a
// mesh or a texture, or replacing a mesh's buffers, only RELEASES the references the renderer holds: WebGPU keeps an object
// alive for as long as a recorded or submitted command uses it. wgpuBufferDestroy / wgpuTextureDestroy are NOT used for them,
// because they would make a command buffer that is recorded (the open frame) or submitted-and-pending invalid.
//
// The one hazard is the in-frame UpdateMesh rule (see UpdateMesh): a queue write is ordered BEFORE the open frame's command
// buffer, which is submitted at EndFrame, so writing into a buffer an earlier draw of the same frame reads would change what
// that draw shows.
//
// Texture formats: 1 channel is R8Unorm and 2 is RG8Unorm (WebGPU samples them as (r,0,0,1) and (r,g,0,1) natively, so there
// is no swizzle), 3 channels are expanded to RGBA8 with alpha 255, 4 are RGBA8; sRGB (3 and 4 channels) is RGBA8UnormSrgb, which
// the GPU decodes BEFORE filtering (the software renderer decodes after, see docs/rendering.html). Mipmaps are generated on the
// CPU (core WebGPU has no blit): a box filter, in linear light for sRGB textures. Queue writes need no 256-byte row alignment
// (that rule is for buffer <-> texture copies, which the readback hooks do with padded rows).

namespace Renderer::WebGpu
{
    using Common::IMesh;
    using Common::ITexture;
    using Common::MeshData;
    using Common::TextureFilter;
    using Common::TextureOptions;
    using Common::TextureWrap;
    using Common::Vertex;

    namespace
    {
        std::uint32_t MipCount(const std::uint32_t width, const std::uint32_t height)
        {
            std::uint32_t levels = 1;
            for (std::uint32_t size = std::max(width, height); size > 1; size >>= 1)
            {
                ++levels;
            }
            return levels;
        }

        std::uint32_t TexelBytes(const WGPUTextureFormat format)
        {
            switch (format)
            {
            case WGPUTextureFormat_R8Unorm:
                return 1;
            case WGPUTextureFormat_RG8Unorm:
                return 2;
            default:
                return 4; // RGBA8, UNORM or sRGB
            }
        }

        const std::array<float, 256>& SrgbToLinearTable()
        {
            static const std::array<float, 256> table = []
            {
                std::array<float, 256> values{};
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    const float c = static_cast<float>(i) / 255.0f;
                    values[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                }
                return values;
            }();
            return table;
        }

        std::uint8_t LinearToSrgb(const float linear)
        {
            const float l = std::clamp(linear, 0.0f, 1.0f);
            const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            return static_cast<std::uint8_t>(std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        /// The next mip level of `source` (sourceWidth x sourceHeight texels of `texelBytes` bytes, rows tightly packed): each
        /// destination texel is the equal-weight average of the source texels it covers. For the usual halving (even sizes)
        /// that is the 2x2 box; for an odd size the covered ranges overlap by one texel (5 -> 2 averages texels 0-2 and 2-4).
        /// `srgb` averages the first three channels in linear light (4-byte texels only), the alpha channel as it is
        std::vector<std::uint8_t> Downsample(const std::vector<std::uint8_t>& source, const std::uint32_t sourceWidth,
                                             const std::uint32_t sourceHeight, const std::uint32_t destinationWidth,
                                             const std::uint32_t destinationHeight, const std::uint32_t texelBytes,
                                             const bool srgb)
        {
            std::vector<std::uint8_t> destination(static_cast<std::size_t>(destinationWidth) * destinationHeight * texelBytes);
            const std::array<float, 256>& decode = SrgbToLinearTable();
            for (std::uint32_t y = 0; y < destinationHeight; ++y)
            {
                const std::uint64_t y0 = static_cast<std::uint64_t>(y) * sourceHeight / destinationHeight;
                const std::uint64_t y1Ceiling =
                    ((static_cast<std::uint64_t>(y) + 1) * sourceHeight + destinationHeight - 1) / destinationHeight;
                const std::uint64_t y1 = std::min<std::uint64_t>(y1Ceiling, sourceHeight);
                for (std::uint32_t x = 0; x < destinationWidth; ++x)
                {
                    const std::uint64_t x0 = static_cast<std::uint64_t>(x) * sourceWidth / destinationWidth;
                    const std::uint64_t x1Ceiling =
                        ((static_cast<std::uint64_t>(x) + 1) * sourceWidth + destinationWidth - 1) / destinationWidth;
                    const std::uint64_t x1 = std::min<std::uint64_t>(x1Ceiling, sourceWidth);
                    const std::uint64_t count = (x1 - x0) * (y1 - y0);
                    for (std::uint32_t channel = 0; channel < texelBytes; ++channel)
                    {
                        const bool decoded = srgb && channel < 3;
                        double sum = 0.0;
                        std::uint64_t integerSum = 0;
                        for (std::uint64_t sy = y0; sy < y1; ++sy)
                        {
                            for (std::uint64_t sx = x0; sx < x1; ++sx)
                            {
                                const std::uint8_t value = source[(sy * sourceWidth + sx) * texelBytes + channel];
                                if (decoded)
                                {
                                    sum += decode[value];
                                }
                                else
                                {
                                    integerSum += value;
                                }
                            }
                        }
                        std::uint8_t& out = destination[(static_cast<std::size_t>(y) * destinationWidth + x) * texelBytes + channel];
                        out = decoded ? LinearToSrgb(static_cast<float>(sum / static_cast<double>(count)))
                                      : static_cast<std::uint8_t>((integerSum + count / 2) / count);
                    }
                }
            }
            return destination;
        }

        BufferRef CreateBuffer(WebGpuRenderer::State& state, const char* label, const std::uint64_t size,
                               const WGPUBufferUsage usage)
        {
            WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
            descriptor.label = ToView(label);
            descriptor.usage = usage;
            descriptor.size = size;
            return BufferRef(wgpuDeviceCreateBuffer(state.device.Get(), &descriptor));
        }

        /// Writes the vertices and indices of `data` into new buffers (the index buffer stays null without indices). Returns
        /// false, leaving `vertex` and `index` as they were, when a size is over the device's buffer limit or a buffer could
        /// not be made
        bool UploadMesh(WebGpuRenderer::State& state, const MeshData& data, BufferRef& vertex, BufferRef& index)
        {
            const std::uint64_t vertexBytes = static_cast<std::uint64_t>(data.vertices.size()) * sizeof(Vertex);
            const std::uint64_t indexBytes = static_cast<std::uint64_t>(data.indices.size()) * sizeof(std::uint32_t);
            if (vertexBytes > state.maxBufferSize || indexBytes > state.maxBufferSize)
            {
                std::cerr << "WebGPU renderer: a mesh of " << vertexBytes << " vertex bytes and " << indexBytes
                          << " index bytes is over the device's buffer limit of " << state.maxBufferSize << std::endl;
                return false;
            }
            // CopySrc as well, so the test hook can read the buffers back
            BufferRef newVertex = CreateBuffer(state, "N2Engine mesh vertices", vertexBytes,
                                               WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc);
            BufferRef newIndex;
            if (indexBytes > 0)
            {
                newIndex = CreateBuffer(state, "N2Engine mesh indices", indexBytes,
                                        WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc);
            }
            if (!newVertex || (indexBytes > 0 && !newIndex))
            {
                std::cerr << "WebGPU renderer: could not make the buffers of a mesh" << std::endl;
                return false;
            }
            // Both sizes are multiples of 4 (64-byte vertices, 4-byte indices), as a queue write needs
            wgpuQueueWriteBuffer(state.queue.Get(), newVertex.Get(), 0, data.vertices.data(),
                                 static_cast<std::size_t>(vertexBytes));
            if (indexBytes > 0)
            {
                wgpuQueueWriteBuffer(state.queue.Get(), newIndex.Get(), 0, data.indices.data(),
                                     static_cast<std::size_t>(indexBytes));
            }
            vertex = std::move(newVertex);
            index = std::move(newIndex);
            return true;
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Samplers
    // -----------------------------------------------------------------------------------------------------------------

    WGPUSampler FindOrCreateSampler(WebGpuRenderer::State& state, const SamplerKey& key)
    {
        for (const auto& [existingKey, sampler] : state.samplers)
        {
            if (existingKey == key)
            {
                return sampler.Get();
            }
        }

        WGPUSamplerDescriptor descriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine sampler");
        descriptor.addressModeU = key.addressMode;
        descriptor.addressModeV = key.addressMode;
        descriptor.addressModeW = key.addressMode;
        descriptor.magFilter = key.filter;
        descriptor.minFilter = key.filter;
        descriptor.mipmapFilter = key.mipmapFilter;
        descriptor.lodMinClamp = 0.0f;
        descriptor.lodMaxClamp = 32.0f; // a texture with one level clamps to it
        descriptor.maxAnisotropy = 1;   // anisotropic filtering stays off
        SamplerRef sampler(wgpuDeviceCreateSampler(state.device.Get(), &descriptor));
        if (!sampler)
        {
            return nullptr;
        }
        WGPUSampler raw = sampler.Get();
        state.samplers.emplace_back(key, std::move(sampler));
        return raw;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Textures
    // -----------------------------------------------------------------------------------------------------------------

    std::unique_ptr<WebGpuTexture> MakeTexture(WebGpuRenderer::State& state, const std::uint8_t* data,
                                               const std::uint32_t width, const std::uint32_t height,
                                               const std::uint32_t channels, const TextureOptions& options)
    {
        if (width > state.maxTextureDimension || height > state.maxTextureDimension)
        {
            std::cerr << "WebGPU renderer: a " << width << "x" << height << " texture is over the device's limit of "
                      << state.maxTextureDimension << std::endl;
            return nullptr;
        }

        // The format follows the channel count; sRGB applies to 3 and 4 channels only, as in the OpenGL and Vulkan renderers
        // (a 1 or 2 channel texture is data)
        WGPUTextureFormat format = channels == 1 ? WGPUTextureFormat_R8Unorm
            : channels == 2                      ? WGPUTextureFormat_RG8Unorm
                                                 : WGPUTextureFormat_RGBA8Unorm;
        const bool srgb = channels >= 3 && options.srgb;
        if (srgb)
        {
            format = WGPUTextureFormat_RGBA8UnormSrgb;
        }
        const std::uint32_t texelBytes = TexelBytes(format);
        const std::uint32_t mipLevels = options.mipmaps ? MipCount(width, height) : 1;

        // Level 0 in the texture's format, rows tightly packed (3 channels expanded with alpha 255)
        std::vector<std::uint8_t> level0;
        if (channels == 3)
        {
            const std::size_t count = static_cast<std::size_t>(width) * height;
            level0.resize(count * 4);
            for (std::size_t i = 0; i < count; ++i)
            {
                level0[i * 4 + 0] = data[i * 3 + 0];
                level0[i * 4 + 1] = data[i * 3 + 1];
                level0[i * 4 + 2] = data[i * 3 + 2];
                level0[i * 4 + 3] = 255;
            }
        }
        else
        {
            level0.assign(data, data + static_cast<std::size_t>(width) * height * texelBytes);
        }

        WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine texture");
        // CopySrc as well, so the test hook can read the levels back
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size = {width, height, 1};
        descriptor.format = format;
        descriptor.mipLevelCount = mipLevels;
        TextureRef texture(wgpuDeviceCreateTexture(state.device.Get(), &descriptor));
        if (!texture)
        {
            std::cerr << "WebGPU renderer: could not make a " << width << "x" << height << " texture" << std::endl;
            return nullptr;
        }
        // The default view: every mip level, one layer, the texture's format, 2D
        TextureViewRef view(wgpuTextureCreateView(texture.Get(), nullptr));
        if (!view)
        {
            std::cerr << "WebGPU renderer: could not make the view of a texture" << std::endl;
            return nullptr;
        }

        // Nearest or linear min/mag, the mipmap filter to match, repeat or clamp-to-edge
        SamplerKey samplerKey;
        samplerKey.filter = options.filter == TextureFilter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
        samplerKey.mipmapFilter =
            samplerKey.filter == WGPUFilterMode_Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
        samplerKey.addressMode = options.wrap == TextureWrap::Repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
        WGPUSampler sampler = FindOrCreateSampler(state, samplerKey);
        if (sampler == nullptr)
        {
            std::cerr << "WebGPU renderer: could not make a sampler" << std::endl;
            return nullptr;
        }

        // Upload level 0, then each further level made from the one before it
        std::vector<std::uint8_t> pixels = std::move(level0);
        std::uint32_t levelWidth = width;
        std::uint32_t levelHeight = height;
        for (std::uint32_t level = 0; level < mipLevels; ++level)
        {
            if (level > 0)
            {
                const std::uint32_t nextWidth = std::max(1u, levelWidth / 2);
                const std::uint32_t nextHeight = std::max(1u, levelHeight / 2);
                pixels = Downsample(pixels, levelWidth, levelHeight, nextWidth, nextHeight, texelBytes, srgb);
                levelWidth = nextWidth;
                levelHeight = nextHeight;
            }
            WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            destination.texture = texture.Get();
            destination.mipLevel = level;
            destination.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.offset = 0;
            layout.bytesPerRow = levelWidth * texelBytes;
            layout.rowsPerImage = levelHeight;
            const WGPUExtent3D extent = {levelWidth, levelHeight, 1};
            wgpuQueueWriteTexture(state.queue.Get(), &destination, pixels.data(), pixels.size(), &layout, &extent);
        }

        auto result = std::make_unique<WebGpuTexture>();
        result->texture = std::move(texture);
        result->view = std::move(view);
        result->sampler = sampler;
        result->samplerKey = samplerKey;
        result->format = format;
        result->width = width;
        result->height = height;
        result->channels = channels;
        result->mipLevels = mipLevels;
        result->options = options;
        return result;
    }

    Common::ITexture* WebGpuRenderer::CreateTexture(const std::uint8_t* data, std::uint32_t width, std::uint32_t height,
                                                    std::uint32_t channels)
    {
        return CreateTexture(data, width, height, channels, TextureOptions::Default());
    }

    Common::ITexture* WebGpuRenderer::CreateTexture(const std::uint8_t* data, std::uint32_t width, std::uint32_t height,
                                                    std::uint32_t channels, const Common::TextureOptions& options)
    {
        if (data == nullptr || width == 0 || height == 0 || channels < 1 || channels > 4)
        {
            return nullptr;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.device)
        {
            return nullptr;
        }
        std::unique_ptr<WebGpuTexture> texture = MakeTexture(state, data, width, height, channels, options);
        if (!texture)
        {
            return nullptr;
        }
        ITexture* raw = texture.get();
        {
            // A new id from a counter that never repeats: the address may be one a destroyed texture had
            std::lock_guard idLock(state.textureIdMutex);
            texture->id = state.nextTextureId++;
            state.textureIds[raw] = texture->id;
        }
        state.textures.emplace(raw, std::move(texture));
        return raw;
    }

    void WebGpuRenderer::DestroyTexture(Common::ITexture* texture)
    {
        if (texture == nullptr)
        {
            return;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        const auto it = state.textures.find(texture);
        if (it == state.textures.end())
        {
            return; // not this renderer's (or already destroyed, or the renderer was shut down)
        }
        const std::uint64_t id = it->second->id;
        // The bind groups that name the texture go first (a draw recorded earlier keeps its own references). The sampler stays
        // in the cache. Releasing is enough: recorded and submitted commands hold the texture alive
        for (auto group = state.materialGroups.begin(); group != state.materialGroups.end();)
        {
            bool uses = false;
            for (std::size_t slot = 1; slot < group->first.size(); ++slot)
            {
                uses = uses || group->first[slot] == id;
            }
            group = uses ? state.materialGroups.erase(group) : std::next(group);
        }
        state.textures.erase(it);
        {
            std::lock_guard idLock(state.textureIdMutex);
            state.textureIds.erase(texture);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Meshes
    // -----------------------------------------------------------------------------------------------------------------

    Common::IMesh* WebGpuRenderer::CreateMesh(const Common::MeshData& meshData)
    {
        if (meshData.vertices.empty())
        {
            return nullptr; // as the other renderers: a mesh needs vertices
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.device)
        {
            return nullptr;
        }
        auto mesh = std::make_unique<WebGpuMesh>();
        if (!UploadMesh(state, meshData, mesh->vertexBuffer, mesh->indexBuffer))
        {
            return nullptr;
        }
        mesh->vertexCount = static_cast<std::uint32_t>(meshData.vertices.size());
        mesh->indexCount = static_cast<std::uint32_t>(meshData.indices.size());
        IMesh* raw = mesh.get();
        state.meshes.emplace(raw, std::move(mesh));
        return raw;
    }

    void WebGpuRenderer::DestroyMesh(Common::IMesh* mesh)
    {
        if (mesh == nullptr)
        {
            return;
        }
        std::lock_guard lock(_mutex);
        // The buffers are released with the mesh; a recorded or submitted draw holds them alive until it has run
        _state->meshes.erase(mesh);
    }

    bool WebGpuRenderer::UpdateMesh(Common::IMesh* mesh, const Common::MeshData& meshData)
    {
        if (mesh == nullptr || meshData.vertices.empty())
        {
            return false;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.device)
        {
            return false;
        }
        const auto it = state.meshes.find(mesh);
        if (it == state.meshes.end())
        {
            return false;
        }
        WebGpuMesh& target = *it->second;
        const auto vertexCount = static_cast<std::uint32_t>(meshData.vertices.size());
        const auto indexCount = static_cast<std::uint32_t>(meshData.indices.size());

        // The in-frame rule. A queue write executes on the queue's timeline when it is made, before the command buffer of the
        // frame being recorded (submitted at EndFrame). So when the mesh was already DRAWN in this frame, those draws would
        // read the new data: the update must go to NEW buffers, which the earlier draws do not reference. A mesh not drawn in
        // this frame (or any mesh outside a frame: every earlier frame is already submitted, and the queue runs its work in
        // order) can be written in place, as long as the buffers are the right size
        const bool drawnInOpenFrame = _frameOpen && target.lastDrawFrame == state.frameSerial;
        const bool sameSize = vertexCount == target.vertexCount && indexCount == target.indexCount;
        if (!drawnInOpenFrame && sameSize)
        {
            wgpuQueueWriteBuffer(state.queue.Get(), target.vertexBuffer.Get(), 0, meshData.vertices.data(),
                                 meshData.vertices.size() * sizeof(Vertex));
            if (indexCount > 0)
            {
                wgpuQueueWriteBuffer(state.queue.Get(), target.indexBuffer.Get(), 0, meshData.indices.data(),
                                     meshData.indices.size() * sizeof(std::uint32_t));
            }
            return true;
        }

        BufferRef vertex;
        BufferRef index;
        if (!UploadMesh(state, meshData, vertex, index))
        {
            return false; // the mesh is unchanged
        }
        target.vertexBuffer = std::move(vertex); // the old buffers are released (earlier draws keep them alive)
        target.indexBuffer = std::move(index);
        target.vertexCount = vertexCount;
        target.indexCount = indexCount;
        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Test and diagnostic hooks
    // -----------------------------------------------------------------------------------------------------------------

    MeshInfoForTest WebGpuRenderer::GetMeshInfoForTest(const Common::IMesh* mesh) const
    {
        MeshInfoForTest info;
        std::lock_guard lock(_mutex);
        const auto it = _state->meshes.find(mesh);
        if (it == _state->meshes.end())
        {
            return info;
        }
        info.valid = true;
        info.vertexBuffer = it->second->vertexBuffer.Get();
        info.indexBuffer = it->second->indexBuffer.Get();
        info.vertexCount = it->second->vertexCount;
        info.indexCount = it->second->indexCount;
        return info;
    }

    bool WebGpuRenderer::ReadMeshForTest(const Common::IMesh* mesh, Common::MeshData& out) const
    {
        std::lock_guard lock(_mutex);
        State& state = *_state;
        const auto it = state.meshes.find(mesh);
        if (it == state.meshes.end() || !_initialized || state.lost.load())
        {
            return false;
        }
        const WebGpuMesh& source = *it->second;
        std::vector<std::uint8_t> bytes;
        if (!CopyBufferToHostLocked(source.vertexBuffer.Get(), static_cast<std::uint64_t>(source.vertexCount) * sizeof(Vertex),
                                    bytes))
        {
            return false;
        }
        out.vertices.resize(source.vertexCount);
        std::memcpy(out.vertices.data(), bytes.data(), bytes.size());
        out.indices.clear();
        if (source.indexCount > 0)
        {
            if (!CopyBufferToHostLocked(source.indexBuffer.Get(),
                                        static_cast<std::uint64_t>(source.indexCount) * sizeof(std::uint32_t), bytes))
            {
                return false;
            }
            out.indices.resize(source.indexCount);
            std::memcpy(out.indices.data(), bytes.data(), bytes.size());
        }
        return true;
    }

    TextureInfoForTest WebGpuRenderer::GetTextureInfoForTest(const Common::ITexture* texture) const
    {
        TextureInfoForTest info;
        std::lock_guard lock(_mutex);
        const auto it = _state->textures.find(texture);
        if (it == _state->textures.end())
        {
            return info;
        }
        const WebGpuTexture& t = *it->second;
        info.valid = true;
        info.format = t.format;
        info.width = t.width;
        info.height = t.height;
        info.channels = t.channels;
        info.mipLevels = t.mipLevels;
        info.filter = t.samplerKey.filter;
        info.mipmapFilter = t.samplerKey.mipmapFilter;
        info.addressMode = t.samplerKey.addressMode;
        info.sampler = t.sampler;
        return info;
    }

    bool WebGpuRenderer::ReadTextureForTest(const Common::ITexture* texture, std::uint32_t mipLevel,
                                            std::vector<std::uint8_t>& bytes, std::uint32_t* levelWidth,
                                            std::uint32_t* levelHeight) const
    {
        std::lock_guard lock(_mutex);
        State& state = *_state;
        const auto it = state.textures.find(texture);
        if (it == state.textures.end() || !_initialized || state.lost.load())
        {
            return false;
        }
        const WebGpuTexture& source = *it->second;
        if (mipLevel >= source.mipLevels)
        {
            return false;
        }
        const std::uint32_t width = std::max(1u, source.width >> mipLevel);
        const std::uint32_t height = std::max(1u, source.height >> mipLevel);
        if (!CopyTextureToHostLocked(source.texture.Get(), WGPUTextureAspect_All, TexelBytes(source.format), width, height,
                                     bytes, mipLevel))
        {
            return false;
        }
        if (levelWidth != nullptr)
        {
            *levelWidth = width;
        }
        if (levelHeight != nullptr)
        {
            *levelHeight = height;
        }
        return true;
    }

    std::uint64_t WebGpuRenderer::GetTextureIdForTest(const Common::ITexture* texture) const
    {
        std::lock_guard lock(_mutex);
        const auto it = _state->textures.find(texture);
        return it == _state->textures.end() ? 0 : it->second->id;
    }

    std::uint32_t WebGpuRenderer::GetLiveMeshCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->meshes.size());
    }

    std::uint32_t WebGpuRenderer::GetLiveTextureCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->textures.size());
    }
}
