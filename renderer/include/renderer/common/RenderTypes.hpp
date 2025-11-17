#pragma once

#include <vector>
#include <cstdint>

namespace Renderer::Common
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
}