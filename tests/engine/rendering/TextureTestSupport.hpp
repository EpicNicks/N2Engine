#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>

// Shared by the texture asset, GpuCache and Image sprite tests: image files built in memory, and a renderer
// that keeps CPU-side resources (the software renderer's resource classes) and counts every call.
namespace TextureTestSupport
{
    using Rgb = std::array<std::uint8_t, 3>;

    /// A 24-bit BMP file of `width` x `height` pixels, given top row first (as an image editor shows them).
    /// The file stores them bottom-up, as BMPs usually do.
    inline std::vector<std::uint8_t> MakeBmp(const int width, const int height, const std::vector<Rgb> &topRowFirst)
    {
        const std::size_t rowBytes = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t{3};
        const std::size_t pixelBytes = rowBytes * static_cast<std::size_t>(height);
        std::vector<std::uint8_t> bmp;
        const auto u16 = [&bmp](const std::uint32_t v)
        {
            bmp.push_back(static_cast<std::uint8_t>(v));
            bmp.push_back(static_cast<std::uint8_t>(v >> 8));
        };
        const auto u32 = [&bmp](const std::uint32_t v)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                bmp.push_back(static_cast<std::uint8_t>(v >> shift));
            }
        };
        bmp.push_back('B');
        bmp.push_back('M');
        u32(static_cast<std::uint32_t>(54 + pixelBytes)); // file size
        u32(0);                                          // reserved
        u32(54);                                         // pixel data offset
        u32(40);                                         // BITMAPINFOHEADER
        u32(static_cast<std::uint32_t>(width));
        u32(static_cast<std::uint32_t>(height));         // positive: bottom-up rows
        u16(1);                                          // planes
        u16(24);                                         // bits per pixel
        u32(0);                                          // BI_RGB
        u32(static_cast<std::uint32_t>(pixelBytes));
        u32(2835);
        u32(2835);
        u32(0);
        u32(0);
        for (int y = height - 1; y >= 0; --y)
        {
            for (int x = 0; x < width; ++x)
            {
                const Rgb &p = topRowFirst[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                           static_cast<std::size_t>(x)];
                bmp.push_back(p[2]); // BGR
                bmp.push_back(p[1]);
                bmp.push_back(p[0]);
            }
            for (std::size_t pad = static_cast<std::size_t>(width) * 3; pad < rowBytes; ++pad)
            {
                bmp.push_back(0);
            }
        }
        return bmp;
    }

    /// What a CountingRenderer was asked to do. Can live outside the renderer, so a test can still read it (and
    /// see that nothing more happened) after the renderer is destroyed.
    struct RendererCounts
    {
        int createdTextures = 0;
        int destroyedTextures = 0;
        int createdMaterials = 0;
        int destroyedMaterials = 0;
        int createdMeshes = 0;
        int destroyedMeshes = 0;
        std::vector<Renderer::Common::TextureOptions> textureOptions; // per CreateTexture call
        std::vector<std::uint32_t> textureChannels;                   // per CreateTexture call
    };

    class CountingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        CountingRenderer() : _counts(_own) {}
        explicit CountingRenderer(RendererCounts &external) : _counts(external) {}

        /// CreateTexture returns nullptr while this is set
        bool failTextures = false;

        std::vector<std::unique_ptr<Renderer::Software::SWTexture>> textures;
        std::vector<std::unique_ptr<Renderer::Software::SWMaterial>> materials;
        std::vector<std::unique_ptr<Renderer::Software::SWMesh>> meshes;
        std::vector<Renderer::Common::IMaterial *> drawnMaterials; // per DrawMesh

        [[nodiscard]] RendererCounts &Counts() { return _counts; }
        /// As Shutdown does: every lifetime token handed out so far expires
        void Kill() { EndLifetime(); }

        Renderer::Common::ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                                  const uint32_t channels) override
        {
            return CreateTexture(data, width, height, channels, Renderer::Common::TextureOptions::Default());
        }

        Renderer::Common::ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                                  const uint32_t channels,
                                                  const Renderer::Common::TextureOptions &options) override
        {
            ++_counts.createdTextures;
            _counts.textureOptions.push_back(options);
            _counts.textureChannels.push_back(channels);
            if (failTextures || !data)
            {
                return nullptr;
            }
            auto texture = std::make_unique<Renderer::Software::SWTexture>();
            texture->width = width;
            texture->height = height;
            texture->channels = channels;
            texture->options = options;
            texture->data.assign(data, data + static_cast<std::size_t>(width) * height * channels);
            textures.push_back(std::move(texture));
            return textures.back().get();
        }

        void DestroyTexture(Renderer::Common::ITexture *texture) override
        {
            ++_counts.destroyedTextures;
            std::erase_if(textures, [texture](const auto &p) { return p.get() == texture; });
        }

        Renderer::Common::IMesh *CreateMesh(const Renderer::Common::MeshData &meshData) override
        {
            ++_counts.createdMeshes;
            auto mesh = std::make_unique<Renderer::Software::SWMesh>();
            mesh->vertices = meshData.vertices;
            mesh->indices = meshData.indices;
            meshes.push_back(std::move(mesh));
            return meshes.back().get();
        }

        void DestroyMesh(Renderer::Common::IMesh *mesh) override
        {
            ++_counts.destroyedMeshes;
            std::erase_if(meshes, [mesh](const auto &p) { return p.get() == mesh; });
        }

        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *shader) override
        {
            return CreateMaterial(shader, nullptr);
        }

        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *shader,
                                                    Renderer::Common::ITexture *texture) override
        {
            ++_counts.createdMaterials;
            materials.push_back(std::make_unique<Renderer::Software::SWMaterial>(shader, texture));
            return materials.back().get();
        }

        void DestroyMaterial(Renderer::Common::IMaterial *material) override
        {
            ++_counts.destroyedMaterials;
            std::erase_if(materials, [material](const auto &p) { return p.get() == material; });
        }

        [[nodiscard]] Renderer::Common::IShader *GetStandardUnlitShader() const override
        {
            return const_cast<Renderer::Software::SWShader *>(&_unlit);
        }

        using IRenderer::DrawMesh;
        void DrawMesh(Renderer::Common::IMesh *, const float *, Renderer::Common::IMaterial *material,
                      const Renderer::Common::RenderState &) override
        {
            drawnMaterials.push_back(material);
        }

        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override { EndLifetime(); }
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        Renderer::Common::IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(Renderer::Common::IShader *) override {}
        bool DestroyShaderProgram(Renderer::Common::IShader *) override { return false; }
        bool IsValidShader(Renderer::Common::IShader *) const override { return false; }
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const N2Engine::Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] Renderer::Common::IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Counting"; }

        /// The texture a material was last given (CreateMaterial or SetTexture)
        [[nodiscard]] static Renderer::Common::ITexture *TextureOf(Renderer::Common::IMaterial *material)
        {
            const auto *sw = dynamic_cast<Renderer::Software::SWMaterial *>(material);
            return sw ? sw->GetTexture() : nullptr;
        }

    private:
        RendererCounts _own;
        RendererCounts &_counts;
        Renderer::Software::SWShader _unlit{Renderer::Software::SWShaderType::Unlit};
    };
}
