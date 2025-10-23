#pragma once

#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace Renderer
{
    namespace Common
    {
        struct Vertex
        {
            float position[3];
            float normal[3];
            float texCoord[2];
            float color[4];
        };

        struct MeshData
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
        };

        struct Transform
        {
            float model[16];
            float view[16];
            float projection[16];
        };

        struct RenderObject
        {
            uint32_t meshId;
            Transform transform;
            uint32_t materialId;
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
            virtual uint32_t CreateShaderProgram(const char *vertexSource, const char *fragmentSource) = 0;
            virtual uint32_t LoadShaderProgram(const std::string &vertexPath, const std::string &fragmentPath) = 0;
            virtual void UseShaderProgram(uint32_t shaderId) = 0;
            virtual void DestroyShaderProgram(uint32_t shaderId) = 0;
            virtual bool IsValidShader(uint32_t shaderId) const = 0;

            // Resource management
            virtual uint32_t CreateMesh(const MeshData &meshData) = 0;
            virtual void DestroyMesh(uint32_t meshId) = 0;
            virtual uint32_t CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels) = 0;
            virtual void DestroyTexture(uint32_t textureId) = 0;
            virtual uint32_t CreateMaterial(uint32_t shaderId, uint32_t textureId = 0) = 0;
            virtual void DestroyMaterial(uint32_t materialId) = 0;

            // Rendering
            virtual void SetViewProjection(const float *view, const float *projection) = 0;
            virtual void DrawMesh(uint32_t meshId, const float *modelMatrix, uint32_t materialId) = 0;
            virtual void DrawObjects(const std::vector<RenderObject> &objects) = 0;
            virtual void OnResize(int width, int height) = 0;

            // Debug
            virtual void SetWireframe(bool enabled) = 0;
            virtual const char *GetRendererName() const = 0;
        };
    }
}