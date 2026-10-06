#include "engine/rendering/Mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <utility>

#include <math/Constants.hpp>

#include "engine/Logger.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        using Renderer::Common::MeshData;
        using Renderer::Common::Vertex;

        constexpr std::size_t BuiltinCount = 3;

        constexpr std::size_t IndexOf(const BuiltinMesh which)
        {
            return static_cast<std::size_t>(which);
        }

        // Fixed, so scene files written today name the same meshes tomorrow. Never change them.
        constexpr std::array<const char *, BuiltinCount> BuiltinUuids{
            "6e32656e-6d65-5348-0001-000000000001", // Cube
            "6e32656e-6d65-5348-0001-000000000002", // Sphere
            "6e32656e-6d65-5348-0001-000000000003", // Quad
        };

        constexpr std::array<std::string_view, BuiltinCount> BuiltinNames{"Cube", "Sphere", "Quad"};

        void Include(BoundingBox &box, bool &any, const Vertex &vertex)
        {
            const Math::Vector3 p(vertex.position[0], vertex.position[1], vertex.position[2]);
            if (!any)
            {
                box.min = p;
                box.max = p;
                any = true;
                return;
            }
            box.min = Math::Vector3(std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z));
            box.max = Math::Vector3(std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z));
        }

        std::shared_ptr<Mesh> MakeBuiltin(const BuiltinMesh which)
        {
            MeshData data;
            switch (which)
            {
            case BuiltinMesh::Cube:
                data = Mesh::MakeCube();
                break;
            case BuiltinMesh::Sphere:
                data = Mesh::MakeSphere();
                break;
            case BuiltinMesh::Quad:
                data = Mesh::MakeQuad();
                break;
            }
            return Mesh::Create(std::move(data));
        }
    }

    // ===== Built-in geometry =====

    MeshData Mesh::MakeCube()
    {
        // Unit cube, 24 vertices (4 per face, so each face has its own normal), exactly as CubeRenderer made it
        MeshData cubeData;

        constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        constexpr float h = 0.5f; // Half size for unit cube

        cubeData.vertices = {
            // Front face (Z+)
            {{-h, -h,  h}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h,  h}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h,  h}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h,  h}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            // Back face (Z-)
            {{ h, -h, -h}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h, -h, -h}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h, -h}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h, -h}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            // Right face (X+)
            {{ h, -h,  h}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h, -h}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h, -h}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h,  h}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            // Left face (X-)
            {{-h, -h, -h}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h, -h,  h}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h,  h}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h, -h}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            // Top face (Y+)
            {{-h,  h,  h}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h,  h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h, -h}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h, -h}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            // Bottom face (Y-)
            {{-h, -h, -h}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h, -h}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h,  h}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h, -h,  h}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}
        };

        // 36 indices (6 faces x 2 triangles x 3 vertices)
        cubeData.indices = {
            // Front
            0, 1, 2, 0, 2, 3,
            // Back
            4, 5, 6, 4, 6, 7,
            // Right
            8, 9, 10, 8, 10, 11,
            // Left
            12, 13, 14, 12, 14, 15,
            // Top
            16, 17, 18, 16, 18, 19,
            // Bottom
            20, 21, 22, 20, 22, 23
        };
        return cubeData;
    }

    MeshData Mesh::MakeSphere(const std::uint32_t latitudeSegments, const std::uint32_t longitudeSegments)
    {
        // A UV sphere from parametric equations, exactly as SphereRenderer made it
        MeshData sphereData;

        for (std::uint32_t lat = 0; lat <= latitudeSegments; ++lat)
        {
            constexpr float PI = Math::Constants::PI_F;
            const float theta = static_cast<float>(lat) * PI / static_cast<float>(latitudeSegments);
            // 0 to PI (top to bottom)
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            for (std::uint32_t lon = 0; lon <= longitudeSegments; ++lon)
            {
                constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                constexpr float radius = 0.5f;
                const float phi = static_cast<float>(lon) * 2.0f * PI / static_cast<float>(longitudeSegments);
                // 0 to 2*PI (around)
                const float sinPhi = std::sin(phi);
                const float cosPhi = std::cos(phi);

                // Vertex position on unit sphere
                const float x = cosPhi * sinTheta;
                const float y = cosTheta;
                const float z = sinPhi * sinTheta;

                // UV texture coordinates
                const float u = static_cast<float>(lon) / static_cast<float>(longitudeSegments);
                const float v = static_cast<float>(lat) / static_cast<float>(latitudeSegments);

                Vertex vertex;
                vertex.position[0] = x * radius;
                vertex.position[1] = y * radius;
                vertex.position[2] = z * radius;

                // Normal (same as normalized position for unit sphere)
                vertex.normal[0] = x;
                vertex.normal[1] = y;
                vertex.normal[2] = z;

                vertex.texCoord[0] = u;
                vertex.texCoord[1] = v;

                vertex.color[0] = white[0];
                vertex.color[1] = white[1];
                vertex.color[2] = white[2];
                vertex.color[3] = white[3];

                sphereData.vertices.push_back(vertex);
            }
        }

        // Two triangles per quad
        for (std::uint32_t lat = 0; lat < latitudeSegments; ++lat)
        {
            for (std::uint32_t lon = 0; lon < longitudeSegments; ++lon)
            {
                const std::uint32_t first = lat * (longitudeSegments + 1) + lon;
                const std::uint32_t second = first + longitudeSegments + 1;

                sphereData.indices.push_back(first);
                sphereData.indices.push_back(second);
                sphereData.indices.push_back(first + 1);

                sphereData.indices.push_back(second);
                sphereData.indices.push_back(second + 1);
                sphereData.indices.push_back(first + 1);
            }
        }
        return sphereData;
    }

    MeshData Mesh::MakeQuad()
    {
        // 1 x 1, centred at the origin, facing +Z, exactly as QuadRenderer made it
        MeshData quadData;

        constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        quadData.vertices = {
            // Bottom-left
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            // Bottom-right
            {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            // Top-right
            {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            // Top-left
            {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}
        };

        // Two triangles: (0,1,2) and (0,2,3)
        quadData.indices = {0, 1, 2, 0, 2, 3};
        return quadData;
    }

    // ===== Bounds and validation =====

    BoundingBox Mesh::ComputeBounds(const std::span<const Vertex> vertices, const std::span<const std::uint32_t> indices)
    {
        BoundingBox box(Math::Vector3(0.0f, 0.0f, 0.0f), Math::Vector3(0.0f, 0.0f, 0.0f));
        bool any = false;
        if (indices.empty())
        {
            for (const Vertex &vertex : vertices)
            {
                Include(box, any, vertex);
            }
            return box;
        }
        for (const std::uint32_t index : indices)
        {
            if (index < vertices.size())
            {
                Include(box, any, vertices[index]);
            }
        }
        return box;
    }

    std::optional<std::string> Mesh::Validate(const MeshData &data, std::vector<Submesh> &submeshes)
    {
        if (data.vertices.empty())
        {
            return std::string("it has no vertices");
        }
        if (data.indices.empty())
        {
            return std::string("it has no indices");
        }
        if (data.indices.size() % 3 != 0)
        {
            return std::format("{} indices aren't whole triangles", data.indices.size());
        }
        if (data.indices.size() > 0xFFFFFFFFull)
        {
            return std::string("it has too many indices");
        }
        const std::size_t vertexCount = data.vertices.size();
        for (std::size_t i = 0; i < data.indices.size(); ++i)
        {
            if (data.indices[i] >= vertexCount)
            {
                return std::format("index {} is {}, past the last of its {} vertices", i, data.indices[i], vertexCount);
            }
        }

        if (submeshes.empty())
        {
            submeshes.push_back(Submesh{0, static_cast<std::uint32_t>(data.indices.size()), {}});
        }
        for (std::size_t i = 0; i < submeshes.size(); ++i)
        {
            Submesh &submesh = submeshes[i];
            const std::uint64_t end = static_cast<std::uint64_t>(submesh.firstIndex) + submesh.indexCount;
            if (submesh.indexCount == 0 || submesh.firstIndex % 3 != 0 || submesh.indexCount % 3 != 0 ||
                end > data.indices.size())
            {
                return std::format("submesh {} (indices {} to {}) isn't whole triangles inside its {} indices", i,
                                   submesh.firstIndex, end, data.indices.size());
            }
            submesh.bounds = ComputeBounds(
                data.vertices, std::span<const std::uint32_t>(data.indices).subspan(submesh.firstIndex, submesh.indexCount));
        }
        return std::nullopt;
    }

    // ===== Mesh =====

    std::shared_ptr<Mesh> Mesh::Create(MeshData data, std::vector<Submesh> submeshes)
    {
        auto mesh = std::make_shared<Mesh>();
        if (!mesh->SetData(std::move(data), std::move(submeshes)))
        {
            return nullptr;
        }
        return mesh;
    }

    bool Mesh::SetData(MeshData data, std::vector<Submesh> submeshes)
    {
        if (_builtin)
        {
            Logger::Error(std::format("Mesh::SetData: the built-in {} mesh is shared and can't be changed", _builtinName));
            return false;
        }
        if (const auto error = Validate(data, submeshes))
        {
            Logger::Error(std::format("Mesh {}: not a valid triangle mesh: {}", GetDebugName(), *error));
            return false;
        }
        _data = std::move(data);
        _submeshes = std::move(submeshes);
        _bounds = ComputeBounds(_data.vertices);
        ++_version;
        return true;
    }

    std::string Mesh::GetDebugName() const
    {
        if (_builtin)
        {
            return _builtinName;
        }
        if (_resourcePath.IsValid())
        {
            return _resourcePath.ToString();
        }
        return "mesh";
    }

    std::shared_ptr<Mesh> Mesh::GetBuiltin(const BuiltinMesh which)
    {
        // Made once, thread-safely, and leaked on purpose: a component kept alive by Lua can be destroyed after
        // function-local statics
        static const auto *builtins = []
        {
            auto *meshes = new std::array<std::shared_ptr<Mesh>, BuiltinCount>();
            for (std::size_t i = 0; i < BuiltinCount; ++i)
            {
                const auto which = static_cast<BuiltinMesh>(i);
                std::shared_ptr<Mesh> mesh = MakeBuiltin(which);
                mesh->SetUUID(GetBuiltinUUID(which));
                mesh->_builtin = true;
                mesh->_builtinName = std::string(BuiltinNames[i]);
                (*meshes)[i] = std::move(mesh);
            }
            return meshes;
        }();
        return (*builtins)[IndexOf(which) < BuiltinCount ? IndexOf(which) : 0];
    }

    Math::UUID Mesh::GetBuiltinUUID(const BuiltinMesh which)
    {
        const std::size_t index = IndexOf(which) < BuiltinCount ? IndexOf(which) : 0;
        return Math::UUID::FromString(BuiltinUuids[index]).value_or(Math::UUID::ZERO);
    }

    std::shared_ptr<Mesh> Mesh::FindBuiltin(const Math::UUID &uuid)
    {
        for (std::size_t i = 0; i < BuiltinCount; ++i)
        {
            const auto which = static_cast<BuiltinMesh>(i);
            if (GetBuiltinUUID(which) == uuid)
            {
                return GetBuiltin(which);
            }
        }
        return nullptr;
    }

    std::string_view Mesh::GetBuiltinName(const BuiltinMesh which)
    {
        return BuiltinNames[IndexOf(which) < BuiltinCount ? IndexOf(which) : 0];
    }

    std::optional<BuiltinMesh> Mesh::ParseBuiltinName(const std::string_view name)
    {
        for (std::size_t i = 0; i < BuiltinCount; ++i)
        {
            if (BuiltinNames[i] == name)
            {
                return static_cast<BuiltinMesh>(i);
            }
        }
        return std::nullopt;
    }
}
