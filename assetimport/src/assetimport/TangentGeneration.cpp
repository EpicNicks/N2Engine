#include "assetimport/TangentGeneration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

extern "C"
{
#include <mikktspace/mikktspace.h>
}

namespace N2Engine::AssetImport
{
    namespace
    {
        using Vec4 = std::array<float, 4>;

        /// What the MikkTSpace callbacks read and write: face f is the triangle at indices 3f..3f+2, and a corner is
        /// 3f + vertexInFace
        struct MikkMesh
        {
            const std::vector<ImportedVertex> *vertices = nullptr;
            const std::vector<std::uint32_t> *indices = nullptr;
            std::vector<Vec4> *corners = nullptr; // the tangent MikkTSpace gave each corner, w = 0 until it does
        };

        const ImportedVertex &VertexOf(const SMikkTSpaceContext *context, const int face, const int vertex)
        {
            const auto *mesh = static_cast<const MikkMesh *>(context->m_pUserData);
            return (*mesh->vertices)[(*mesh->indices)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vertex)]];
        }

        int GetNumFaces(const SMikkTSpaceContext *context)
        {
            const auto *mesh = static_cast<const MikkMesh *>(context->m_pUserData);
            return static_cast<int>(mesh->indices->size() / 3);
        }

        int GetNumVerticesOfFace(const SMikkTSpaceContext *, const int)
        {
            return 3;
        }

        /// MikkTSpace is given only finite numbers and unit normals, whatever the mesh holds: a NaN or a zero normal
        /// would make its sorting and normalising unpredictable
        float Finite(const float value)
        {
            return std::isfinite(value) ? value : 0.0f;
        }

        void GetPosition(const SMikkTSpaceContext *context, float out[], const int face, const int vertex)
        {
            const ImportedVertex &v = VertexOf(context, face, vertex);
            out[0] = Finite(v.position[0]);
            out[1] = Finite(v.position[1]);
            out[2] = Finite(v.position[2]);
        }

        void GetNormal(const SMikkTSpaceContext *context, float out[], const int face, const int vertex)
        {
            const ImportedVertex &v = VertexOf(context, face, vertex);
            const float x = Finite(v.normal[0]), y = Finite(v.normal[1]), z = Finite(v.normal[2]);
            const float length = std::sqrt(x * x + y * y + z * z);
            if (!(length > 1e-12f) || !std::isfinite(length))
            {
                out[0] = 0.0f;
                out[1] = 0.0f;
                out[2] = 1.0f;
                return;
            }
            out[0] = x / length;
            out[1] = y / length;
            out[2] = z / length;
        }

        void GetTexCoord(const SMikkTSpaceContext *context, float out[], const int face, const int vertex)
        {
            const ImportedVertex &v = VertexOf(context, face, vertex);
            out[0] = Finite(v.texCoord[0]);
            out[1] = Finite(v.texCoord[1]);
        }

        void SetTSpaceBasic(const SMikkTSpaceContext *context, const float tangent[], const float sign, const int face,
                            const int vertex)
        {
            auto *mesh = static_cast<MikkMesh *>(context->m_pUserData);
            // The bitangent is cross(normal, tangent) * sign (the header's "basic" form)
            (*mesh->corners)[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(vertex)] =
                Vec4{tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f};
        }

        float Dot3(const float a[3], const float b[3])
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        bool IsFinite3(const float v[3])
        {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        /// Whether a vertex has a tangent the shaders can use: finite, not zero
        bool HasTangent(const ImportedVertex &vertex)
        {
            const float *t = vertex.tangent;
            return IsFinite3(t) && Dot3(t, t) > 1e-12f;
        }

        /// A unit vector perpendicular to the normal (w = +1): the axis the normal is least aligned with, with the
        /// normal's part removed. Any normal, even zero or not finite, gives a finite unit tangent.
        Vec4 FallbackTangent(const ImportedVertex &vertex)
        {
            float n[3] = {vertex.normal[0], vertex.normal[1], vertex.normal[2]};
            if (!IsFinite3(n) || Dot3(n, n) < 1e-12f)
            {
                return Vec4{1.0f, 0.0f, 0.0f, 1.0f};
            }
            const float inv = 1.0f / std::sqrt(Dot3(n, n));
            n[0] *= inv;
            n[1] *= inv;
            n[2] *= inv;
            // The smallest component picks the axis furthest from the normal
            float axis[3] = {0.0f, 0.0f, 0.0f};
            const float ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
            axis[ax <= ay && ax <= az ? 0 : (ay <= az ? 1 : 2)] = 1.0f;
            const float d = Dot3(axis, n);
            float t[3] = {axis[0] - n[0] * d, axis[1] - n[1] * d, axis[2] - n[2] * d};
            const float length = std::sqrt(Dot3(t, t));
            return Vec4{t[0] / length, t[1] / length, t[2] / length, 1.0f};
        }

        /// MikkTSpace's corner tangent made safe: finite, unit length and perpendicular to the vertex normal (it is
        /// already, up to rounding). When it isn't usable (degenerate UVs) the fallback, and usable = false.
        Vec4 CleanTangent(const Vec4 &raw, const ImportedVertex &vertex, bool &usable)
        {
            usable = true;
            float t[3] = {raw[0], raw[1], raw[2]};
            if (!IsFinite3(t))
            {
                usable = false;
                return FallbackTangent(vertex);
            }
            // Remove the normal's part (a no-op for a unit normal and a perpendicular tangent)
            const float n2 = Dot3(vertex.normal, vertex.normal);
            if (IsFinite3(vertex.normal) && n2 > 1e-12f)
            {
                const float d = Dot3(t, vertex.normal) / n2;
                t[0] -= vertex.normal[0] * d;
                t[1] -= vertex.normal[1] * d;
                t[2] -= vertex.normal[2] * d;
            }
            const float length2 = Dot3(t, t);
            if (!(length2 > 1e-12f) || !std::isfinite(length2))
            {
                usable = false;
                return FallbackTangent(vertex);
            }
            const float inv = 1.0f / std::sqrt(length2);
            return Vec4{t[0] * inv, t[1] * inv, t[2] * inv, raw[3] < 0.0f ? -1.0f : 1.0f};
        }

        /// Whether the triangle whose first corner is `corner` maps a surface area of the texture (its uvs aren't all on
        /// one line): MikkTSpace gives a degenerate triangle some tangent, but it is no one's tangent space
        bool HasUvArea(const std::vector<ImportedVertex> &vertices, const std::vector<std::uint32_t> &indices,
                       const std::size_t corner)
        {
            const float *a = vertices[indices[corner]].texCoord;
            const float *b = vertices[indices[corner + 1]].texCoord;
            const float *c = vertices[indices[corner + 2]].texCoord;
            const float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
            return std::isfinite(area) && std::fabs(area) > 1e-12f;
        }

        /// Two tangents are the same space: the same handedness and (to 0.8 degrees) the same direction
        bool SameTangent(const float a[4], const Vec4 &b)
        {
            return (a[3] < 0.0f) == (b[3] < 0.0f) && Dot3(a, b.data()) > 0.9999f;
        }
    }

    TangentResult GenerateTangents(std::vector<ImportedVertex> &vertices, std::vector<std::uint32_t> &indices,
                                   const TangentSelect select, const std::size_t maxVertices)
    {
        TangentResult result;
        const std::size_t triangleCount = indices.size() / 3;
        const std::size_t cornerCount = triangleCount * 3;
        for (std::size_t i = 0; i < cornerCount; ++i)
        {
            if (indices[i] >= vertices.size())
            {
                result.ok = false;
                return result;
            }
        }

        // The vertices to write
        std::vector<std::uint8_t> wanted(vertices.size(), 0);
        bool any = false;
        for (std::size_t v = 0; v < vertices.size(); ++v)
        {
            if (select == TangentSelect::All || !HasTangent(vertices[v]))
            {
                wanted[v] = 1;
                any = true;
            }
        }
        if (!any)
        {
            return result;
        }

        // MikkTSpace over every triangle: it welds vertices by position, normal and uv itself, and gives each corner
        // its tangent
        std::vector<Vec4> corners;
        if (cornerCount > 0)
        {
            corners.assign(cornerCount, Vec4{0.0f, 0.0f, 0.0f, 0.0f});
            MikkMesh mesh{&vertices, &indices, &corners};
            SMikkTSpaceInterface callbacks{};
            callbacks.m_getNumFaces = GetNumFaces;
            callbacks.m_getNumVerticesOfFace = GetNumVerticesOfFace;
            callbacks.m_getPosition = GetPosition;
            callbacks.m_getNormal = GetNormal;
            callbacks.m_getTexCoord = GetTexCoord;
            callbacks.m_setTSpaceBasic = SetTSpaceBasic;
            callbacks.m_setTSpace = nullptr;
            SMikkTSpaceContext context{};
            context.m_pInterface = &callbacks;
            context.m_pUserData = &mesh;
            if (!genTangSpaceDefault(&context))
            {
                result.ok = false;
                return result;
            }
        }

        // What each wanted vertex was given: the tangent of the first triangle corner that reached it (its own slot),
        // and, for a vertex whose other corners asked for a different tangent, the copies made for them
        struct Copy
        {
            std::uint32_t vertex;
            Vec4 tangent;
        };
        std::vector<Vec4> own(vertices.size());
        std::vector<std::uint8_t> assigned(vertices.size(), 0);
        std::unordered_map<std::uint32_t, std::vector<Copy>> copies;
        std::vector<ImportedVertex> added;
        std::vector<Vec4> addedTangents;

        std::vector<std::uint32_t> newIndices = indices; // applied only when everything fits

        // First pass: a vertex's own slot is the tangent of its first corner that MikkTSpace gave a usable one. A corner
        // without one (a degenerate triangle) never takes the slot, and never asks for a copy: it uses the slot.
        std::vector<Vec4> cornerTangents(cornerCount);
        std::vector<std::uint8_t> cornerUsable(cornerCount, 0);
        for (std::size_t c = 0; c < cornerCount; ++c)
        {
            const std::uint32_t original = indices[c];
            if (!wanted[original])
            {
                continue;
            }
            bool usable = true;
            cornerTangents[c] = CleanTangent(corners[c], vertices[original], usable);
            usable = usable && HasUvArea(vertices, indices, c - c % 3);
            cornerUsable[c] = usable ? 1 : 0;
            if (usable && !assigned[original])
            {
                assigned[original] = 1;
                own[original] = cornerTangents[c];
            }
        }
        for (std::size_t c = 0; c < cornerCount; ++c)
        {
            const std::uint32_t original = indices[c];
            if (!wanted[original] || !cornerUsable[c])
            {
                continue;
            }
            const Vec4 &tangent = cornerTangents[c];
            if (SameTangent(own[original].data(), tangent))
            {
                continue; // the vertex's own slot already holds it
            }
            std::vector<Copy> &known = copies[original];
            const auto same = std::ranges::find_if(known, [&](const Copy &copy)
            {
                return SameTangent(copy.tangent.data(), tangent);
            });
            if (same != known.end())
            {
                newIndices[c] = same->vertex;
                continue;
            }
            if (vertices.size() + added.size() >= maxVertices)
            {
                result.ok = false;
                return result;
            }
            const auto index = static_cast<std::uint32_t>(vertices.size() + added.size());
            known.push_back(Copy{index, tangent});
            added.push_back(vertices[original]);
            addedTangents.push_back(tangent);
            newIndices[c] = index;
        }

        // Write the tangents. A wanted vertex no triangle reached gets the fallback.
        for (std::size_t v = 0; v < vertices.size(); ++v)
        {
            if (!wanted[v])
            {
                continue;
            }
            if (!assigned[v])
            {
                own[v] = FallbackTangent(vertices[v]);
                ++result.fallbackVertices;
            }
            std::copy(own[v].begin(), own[v].end(), vertices[v].tangent);
        }
        for (std::size_t i = 0; i < added.size(); ++i)
        {
            std::copy(addedTangents[i].begin(), addedTangents[i].end(), added[i].tangent);
        }
        result.verticesAdded = added.size();
        result.verticesWritten = std::count(wanted.begin(), wanted.end(), std::uint8_t{1}) + added.size();
        vertices.insert(vertices.end(), added.begin(), added.end());
        indices = std::move(newIndices);
        return result;
    }
}
