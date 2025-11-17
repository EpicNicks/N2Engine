#pragma once

#include <memory>
#include <vector>
#include <cstdint>
#include <string>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "renderer/common/RenderTypes.hpp"
#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/IMesh.hpp"

namespace Renderer::Common
{
    struct RenderObject
    {
        IMesh *mesh;
        Transform transform;
        IMaterial *material;
    };

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
        virtual IShader *CreateShaderProgram(const char *vertexSource, const char *fragmentSource) = 0;
        virtual void UseShaderProgram(IShader *shader) = 0;
        virtual bool DestroyShaderProgram(IShader *shader) = 0;
        virtual bool IsValidShader(IShader *shader) const = 0;

        // Resource management
        virtual IMesh *CreateMesh(const MeshData &meshData) = 0;
        virtual void DestroyMesh(IMesh *mesh) = 0;
        virtual ITexture *CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels) = 0;
        virtual void DestroyTexture(ITexture *texture) = 0;
        virtual IMaterial *CreateMaterial(IShader *shader, ITexture *texture = nullptr) = 0;
        virtual void DestroyMaterial(IMaterial *material) = 0;

        // Rendering
        virtual void SetViewProjection(const float *view, const float *projection) = 0;
        virtual void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material) = 0;
        virtual void DrawObjects(const std::vector<RenderObject> &objects) = 0;
        virtual void OnResize(int width, int height) = 0;

        virtual IShader *GetStandardUnlitShader() const = 0;
        virtual IShader *GetStandardLitShader() const = 0;

        // Debug
        virtual void SetWireframe(bool enabled) = 0;
        virtual const char *GetRendererName() const = 0;
    };
}