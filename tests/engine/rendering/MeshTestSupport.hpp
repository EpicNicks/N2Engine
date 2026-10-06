#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>

#include "engine/Logger.hpp"

// Shared by the mesh, material, MeshRenderer and built-in shape tests: a renderer that keeps CPU-side resources
// (the software renderer's resource classes), counts every call and records every draw. No window or GPU.
namespace MeshTestSupport
{
    /// One DrawMesh call
    struct RecordedDraw
    {
        Renderer::Common::IMesh *mesh = nullptr;
        Renderer::Common::IMaterial *material = nullptr;
        Renderer::Common::RenderState state;
        Renderer::Common::IndexRange range; // the whole mesh for a draw without a range
        bool hasRange = false;
        std::array<float, 16> model{};
        std::array<float, 4> albedo{};      // the material's uAlbedo when drawn
        float alphaCutoff = 0.0f;            // the material's uAlphaCutoff when drawn
    };

    /// What a RecordingMeshRenderer was asked to do. Can live outside it, so a test can still read it (and see that
    /// nothing more happened) after the renderer is destroyed.
    struct Counts
    {
        int createdMeshes = 0;
        int destroyedMeshes = 0;
        int updatedMeshes = 0;
        int createdMaterials = 0;
        int destroyedMaterials = 0;
        int createdTextures = 0;
        int destroyedTextures = 0;

        [[nodiscard]] int Destroys() const { return destroyedMeshes + destroyedMaterials + destroyedTextures; }
    };

    class RecordingMeshRenderer final : public Renderer::Common::IRenderer
    {
    public:
        RecordingMeshRenderer() : _counts(_own) {}
        explicit RecordingMeshRenderer(Counts &external) : _counts(external) {}

        /// UpdateMesh fails (as the default body does) while this is false
        bool canUpdateMeshes = true;
        /// GetStandardLitShader returns null while this is false (as a backend without a lit shader)
        bool hasLitShader = true;
        /// CreateMesh returns null (still counting the call) while this is false
        bool canCreateMeshes = true;

        std::vector<std::unique_ptr<Renderer::Software::SWMesh>> meshes;
        std::vector<std::unique_ptr<Renderer::Software::SWMaterial>> materials;
        std::vector<std::unique_ptr<Renderer::Software::SWTexture>> textures;
        std::vector<RecordedDraw> draws;

        Renderer::Software::SWShader unlitShader{Renderer::Software::SWShaderType::Unlit};
        Renderer::Software::SWShader litShader{Renderer::Software::SWShaderType::Lit};

        [[nodiscard]] Counts &GetCounts() { return _counts; }
        /// As Shutdown does: every lifetime token handed out so far expires
        void Kill() { EndLifetime(); }

        [[nodiscard]] bool OwnsMesh(const Renderer::Common::IMesh *mesh) const
        {
            return std::ranges::any_of(meshes, [mesh](const auto &p) { return p.get() == mesh; });
        }
        [[nodiscard]] bool OwnsMaterial(const Renderer::Common::IMaterial *material) const
        {
            return std::ranges::any_of(materials, [material](const auto &p) { return p.get() == material; });
        }
        [[nodiscard]] static const Renderer::Software::SWMaterial *AsSW(const Renderer::Common::IMaterial *material)
        {
            return dynamic_cast<const Renderer::Software::SWMaterial *>(material);
        }

        Renderer::Common::IMesh *CreateMesh(const Renderer::Common::MeshData &meshData) override
        {
            ++_counts.createdMeshes;
            if (!canCreateMeshes)
            {
                return nullptr;
            }
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

        bool UpdateMesh(Renderer::Common::IMesh *mesh, const Renderer::Common::MeshData &meshData) override
        {
            if (!canUpdateMeshes)
            {
                return false;
            }
            for (const auto &owned : meshes)
            {
                if (owned.get() == mesh)
                {
                    ++_counts.updatedMeshes;
                    owned->vertices = meshData.vertices;
                    owned->indices = meshData.indices;
                    return true;
                }
            }
            return false;
        }

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
            if (!data)
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
            return const_cast<Renderer::Software::SWShader *>(&unlitShader);
        }
        [[nodiscard]] Renderer::Common::IShader *GetStandardLitShader() const override
        {
            return hasLitShader ? const_cast<Renderer::Software::SWShader *>(&litShader) : nullptr;
        }

        using IRenderer::DrawMesh;
        void DrawMesh(Renderer::Common::IMesh *mesh, const float *model, Renderer::Common::IMaterial *material,
                      const Renderer::Common::RenderState &state) override
        {
            Record(mesh, model, material, state, Renderer::Common::IndexRange{0, mesh ? mesh->GetIndexCount() : 0u},
                   false);
        }
        void DrawMesh(Renderer::Common::IMesh *mesh, const float *model, Renderer::Common::IMaterial *material,
                      const Renderer::Common::RenderState &state, const Renderer::Common::IndexRange &range) override
        {
            Record(mesh, model, material, state, range, true);
        }

        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override { EndLifetime(); } // as the real backends do
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        Renderer::Common::IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(Renderer::Common::IShader *) override {}
        bool DestroyShaderProgram(Renderer::Common::IShader *) override { return true; }
        bool IsValidShader(Renderer::Common::IShader *) const override { return false; }
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const N2Engine::Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "RecordingMesh"; }

    private:
        void Record(Renderer::Common::IMesh *mesh, const float *model, Renderer::Common::IMaterial *material,
                    const Renderer::Common::RenderState &state, const Renderer::Common::IndexRange &range,
                    const bool hasRange)
        {
            RecordedDraw draw;
            draw.mesh = mesh;
            draw.material = material;
            draw.state = state;
            draw.range = range;
            draw.hasRange = hasRange;
            if (model)
            {
                std::memcpy(draw.model.data(), model, sizeof(float) * 16);
            }
            if (const auto *sw = AsSW(material))
            {
                draw.albedo = sw->GetVec4("uAlbedo", {1, 1, 1, 1});
                draw.alphaCutoff = sw->GetFloat("uAlphaCutoff", 0.0f);
            }
            draws.push_back(draw);
        }

        Counts _own;
        Counts &_counts;
    };

    /// Collects the warnings logged while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = N2Engine::Logger::logEvent += [this](const std::string_view message, const N2Engine::Logger::LogLevel level)
            {
                if (level == N2Engine::Logger::LogLevel::Warn || level == N2Engine::Logger::LogLevel::Error)
                {
                    messages.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { N2Engine::Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        /// Warnings and errors, in order
        std::vector<std::string> messages;

        [[nodiscard]] bool Mentions(const std::string_view text) const
        {
            return std::ranges::any_of(messages, [text](const std::string &m) { return m.find(text) != std::string::npos; });
        }

    private:
        size_t _id = 0;
    };
}
