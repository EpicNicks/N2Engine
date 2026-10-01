#pragma once

#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <math/Vector3.hpp>

#include "renderer/common/RenderTypes.hpp"
#include "renderer/common/RenderState.hpp"
#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/IMesh.hpp"

#include "renderer/common/SceneLighting.hpp"

namespace Renderer::Common
{
    struct RenderObject
    {
        IMesh *mesh;
        Transform transform;
        IMaterial *material;
        RenderState state{};
    };

    /**
     * A rendering backend.
     *
     * Draw order: the draws of a frame (DrawMesh and DrawObjects between BeginFrame and EndFrame) reach
     * the framebuffer in submission order, unless a backend documents otherwise. Each draw carries its own
     * RenderState; there are no global depth/cull/blend setters, so a backend that reorders draws still
     * applies the right state to each one.
     *
     * Exceptions:
     * - SoftwareRenderer decides by each draw's state, not its queue. It reorders depth-writing draws
     *   front to back (each run of consecutive draws that both depth-test and write depth; early-Z),
     *   which cannot change the image apart from exact depth ties. Every draw that doesn't both
     *   depth-test and write depth (normally the Transparent queue) keeps its submission position. A
     *   Transparent renderable that doesn't override IRenderable::RenderInQueue draws with the default,
     *   depth-writing state, so it can be reordered. It has no blending: RenderState::blend is ignored.
     * - VulkanRenderer does not draw yet (#43).
     */
    class IRenderer
    {
    public:
        virtual ~IRenderer() = default;

        // Lifecycle
        virtual bool Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height) = 0;
        virtual void Shutdown() = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
        virtual void Clear(float r, float g, float b, float a) = 0;

        // Frame management
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;
        virtual void Present() = 0;

        // Shader management
        virtual IShader* CreateShaderProgram(const char *vertexSource, const char *fragmentSource) = 0;
        virtual void UseShaderProgram(IShader *shader) = 0;
        virtual bool DestroyShaderProgram(IShader *shader) = 0;
        virtual bool IsValidShader(IShader *shader) const = 0;

        // Resource management
        virtual IMesh* CreateMesh(const MeshData &meshData) = 0;
        virtual void DestroyMesh(IMesh *mesh) = 0;
        virtual ITexture* CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels) = 0;
        virtual void DestroyTexture(ITexture *texture) = 0;
        virtual IMaterial* CreateMaterial(IShader *shader) = 0;
        virtual IMaterial* CreateMaterial(IShader *shader, ITexture *texture) = 0;
        virtual void DestroyMaterial(IMaterial *material) = 0;

        // Rendering
        virtual void SetViewProjection(const float *view, const float *projection) = 0;
        virtual void UpdateSceneLighting(const SceneLightingData &lighting,
                                         const N2Engine::Math::Vector3 &cameraPosition) = 0;

        /// Draws with the default RenderState (RenderState::Opaque()).
        void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material)
        {
            DrawMesh(mesh, modelMatrix, material, RenderState{});
        }
        /// Draws with the given fixed-function state. The state applies to this draw only.
        virtual void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material,
                              const RenderState &state) = 0;
        /// Draws each object with its own RenderObject::state, in order.
        virtual void DrawObjects(const std::vector<RenderObject> &objects) = 0;
        virtual void OnResize(int width, int height) = 0;

        [[nodiscard]] virtual IShader* GetStandardUnlitShader() const = 0;
        [[nodiscard]] virtual IShader* GetStandardLitShader() const = 0;

        virtual void ReadFramebuffer(std::uint8_t *buffer, int width, int height) const = 0;

        // Debug
        virtual void SetWireframe(bool enabled) = 0;
        [[nodiscard]] virtual const char* GetRendererName() const = 0;
    };
}
