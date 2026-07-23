#pragma once

#include <vector>
#include <memory>

#include "renderer/common/Renderer.hpp"
#include "renderer/software/RenderThread.hpp"
#include "renderer/software/SWMesh.hpp"
#include "renderer/software/SWTexture.hpp"
#include "renderer/software/SWShader.hpp"
#include "renderer/software/SWMaterial.hpp"

namespace Renderer::Software
{
    class SoftwareRenderer : public Common::IRenderer
    {
    public:
        // Lifecycle
        bool Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height) override;
        void Shutdown() override;
        void Resize(uint32_t width, uint32_t height) override;
        void Clear(float r, float g, float b, float a) override;

        // Frame
        void BeginFrame() override;
        void EndFrame() override;
        void Present() override;

        // Shaders
        Common::IShader* CreateShaderProgram(const char *vs, const char *fs) override;
        void UseShaderProgram(Common::IShader *shader) override;
        bool DestroyShaderProgram(Common::IShader *shader) override;
        bool IsValidShader(Common::IShader *shader) const override;

        // Resources
        Common::IMesh* CreateMesh(const Common::MeshData &meshData) override;
        void DestroyMesh(Common::IMesh *mesh) override;
        Common::ITexture* CreateTexture(const uint8_t *data, uint32_t w, uint32_t h, uint32_t ch) override;
        void DestroyTexture(Common::ITexture *texture) override;
        Common::IMaterial* CreateMaterial(Common::IShader *shader) override;
        Common::IMaterial* CreateMaterial(Common::IShader *shader, Common::ITexture *texture) override;
        void DestroyMaterial(Common::IMaterial *material) override;

        // Rendering
        void SetViewProjection(const float *view, const float *projection) override;
        void UpdateSceneLighting(const Common::SceneLightingData &lighting,
                                 const N2Engine::Math::Vector3 &cameraPosition) override;
        void DrawMesh(Common::IMesh *mesh, const float *modelMatrix, Common::IMaterial *material) override;
        void DrawObjects(const std::vector<Common::RenderObject> &objects) override;
        void OnResize(int width, int height) override;

        Common::IShader* GetStandardUnlitShader() const override { return m_unlitShader.get(); }
        Common::IShader* GetStandardLitShader() const override { return m_litShader.get(); }

        void ReadFramebuffer(uint8_t *buffer, int width, int height) const override;

        void SetWireframe(bool enabled) override;
        const char* GetRendererName() const override { return "Software Rasterizer"; }

    private:
        struct DrawCommand {
            SWMesh* mesh;
            float modelMatrix[16];
            SWMaterial* material;
        };

        std::vector<DrawCommand> m_drawQueue;
        RenderThread m_renderThread;

        // Framebuffer
        uint32_t m_width = 0, m_height = 0;
        std::vector<uint32_t> m_colorBuffer; // RGBA8 packed
        std::vector<float> m_depthBuffer;
        float m_clearR = 0, m_clearG = 0, m_clearB = 0, m_clearA = 1;

        // GL blit
        GLFWwindow *m_window = nullptr;
        unsigned int m_blitTex = 0, m_blitVAO = 0, m_blitVBO = 0, m_blitProg = 0;
        bool SetupBlitResources();

        // Matrices (row-major, matching OpenGLRenderer convention)
        float m_view[16]{}, m_proj[16]{};

        // Lighting state
        Common::SceneLightingData m_lighting;
        N2Engine::Math::Vector3 m_cameraPos{};

        // Built-in shaders
        std::unique_ptr<SWShader> m_unlitShader;
        std::unique_ptr<SWShader> m_litShader;

        // Owned resource sets (for lifetime tracking)
        std::vector<std::unique_ptr<SWMesh>> m_meshes;
        std::vector<std::unique_ptr<SWTexture>> m_textures;
        std::vector<std::unique_ptr<SWMaterial>> m_materials;
        std::vector<std::unique_ptr<SWShader>> m_shaders;

        bool m_wireframe = false;

        // Rasterizer internals
        void ClearBuffers();
        void SetPixel(int x, int y, float depth, uint32_t color);

        struct SWFragment
        {
            float x, y, z;        // NDC after persp divide
            float wx, wy, wz;     // world-space position (for lighting)
            float nx, ny, nz;     // interpolated normal (world)
            float u, v;           // interpolated texcoord
            float r, g, b, a;     // interpolated vertex color
        };

        void RasterizeTriangle(const SWFragment &f0, const SWFragment &f1, const SWFragment &f2, const SWMaterial *mat,
                               const float *modelMatrix);
        void RasterizeMesh(SWMesh* mesh, const float* modelMatrix, SWMaterial* material);

        uint32_t ShadeLit(const SWFragment &frag, const SWMaterial *mat, const float *modelMatrix) const;
        uint32_t ShadeUnlit(const SWFragment &frag, const SWMaterial *mat) const;

        // Math helpers (row-major)
        void Mul4x4(const float *a, const float *b, float *out) const;
        void TransformPoint(const float *mat, const float *in3, float *out4) const;
        void TransformNormal(const float *modelMatrix, const float *n, float *out) const;
    };
}