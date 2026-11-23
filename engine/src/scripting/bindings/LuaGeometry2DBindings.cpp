// The Lua side of lines, trails and script-built meshes (docs/meshes.html#lines, #trails, #script-meshes): the LineRenderer,
// TrailRenderer and MeshRenderer2D components, the global Mesh table (Create, Triangulate) and the ProceduralMesh handle.

#include "engine/scripting/bindings/LuaBindings.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/Geometry2D.hpp"
#include "engine/rendering/LineRenderer.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer2D.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/rendering/TrailRenderer.hpp"
#include "engine/scripting/LuaRuntime.hpp"

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        /// What Mesh.Create returns: a mesh made at runtime, shared with whatever draws it
        struct LuaMeshHandle
        {
            std::shared_ptr<Rendering::Mesh> mesh;
        };

        [[noreturn]] void Raise(const std::string_view caller, const std::string &message)
        {
            throw std::runtime_error(std::format("{}: {}", caller, message));
        }

        /// An enum value from its scene-file name; anything else is an error naming the options
        template <typename E>
        E ParseName(const std::string &name, const std::string_view caller)
        {
            const std::vector<std::string> options = FieldTraits<E>::EnumOptions();
            const auto it = std::ranges::find(options, name);
            if (it == options.end())
            {
                std::string expected;
                for (const std::string &option : options)
                {
                    expected += expected.empty() ? option : ", " + option;
                }
                Raise(caller, std::format("unknown value '{}'. Expected one of: {}", name, expected));
            }
            return static_cast<E>(std::distance(options.begin(), it));
        }

        template <typename E>
        std::string NameOf(const E value)
        {
            return nlohmann::json(value).template get<std::string>();
        }

        /// A Vector2 (z = 0) or Vector3, or nullopt
        std::optional<Math::Vector3> AsPoint(const sol::object &value)
        {
            if (value.is<Math::Vector3>())
            {
                return value.as<Math::Vector3>();
            }
            if (value.is<Math::Vector2>())
            {
                const auto v = value.as<Math::Vector2>();
                return Math::Vector3(v.x, v.y, 0.0f);
            }
            return std::nullopt;
        }

        Math::Vector3 PointOrThrow(const sol::object &value, const std::string_view caller)
        {
            const auto point = AsPoint(value);
            if (!point)
            {
                Raise(caller, "a point must be a Vector2 or a Vector3");
            }
            return *point;
        }

        /// A table of Vector2/Vector3, of at most `limit` entries (checked before reading any)
        std::vector<Math::Vector3> ReadPoints(const sol::table &table, const std::size_t limit, const std::string_view caller)
        {
            const std::size_t count = table.size();
            if (count > limit)
            {
                Raise(caller, std::format("{} points, over the limit of {}", count, limit));
            }
            std::vector<Math::Vector3> points;
            points.reserve(count);
            for (std::size_t i = 1; i <= count; ++i)
            {
                const auto point = AsPoint(table[i]);
                if (!point)
                {
                    Raise(caller, std::format("point {} is not a Vector2 or a Vector3", i));
                }
                points.push_back(*point);
            }
            return points;
        }

        /// The table field `key` as a table, or nullopt when it is nil; any other type is an error
        std::optional<sol::table> OptionalTable(const sol::table &desc, const char *key, const std::string_view caller)
        {
            const sol::object value = desc[key];
            if (value.get_type() == sol::type::lua_nil)
            {
                return std::nullopt;
            }
            if (value.get_type() != sol::type::table)
            {
                Raise(caller, std::format("'{}' must be a table", key));
            }
            return value.as<sol::table>();
        }

        /// A script's mesh description: { vertices = {...}, indices = {...}, colors = {...}, uvs = {...} }. Without indices the vertices
        /// are a polygon's outline (x, y), triangulated
        Renderer::Common::MeshData ReadMeshDesc(const sol::table &desc, const std::string_view caller)
        {
            Rendering::ScriptMeshDesc mesh;
            const auto vertices = OptionalTable(desc, "vertices", caller);
            if (!vertices)
            {
                Raise(caller, "the description needs 'vertices', a table of Vector2 or Vector3");
            }
            mesh.positions = ReadPoints(*vertices, Rendering::kMaxScriptMeshVertices, caller);

            if (const auto colors = OptionalTable(desc, "colors", caller))
            {
                const std::size_t count = colors->size();
                if (count > Rendering::kMaxScriptMeshVertices)
                {
                    Raise(caller, std::format("{} colours, over the limit of {}", count, Rendering::kMaxScriptMeshVertices));
                }
                for (std::size_t i = 1; i <= count; ++i)
                {
                    const sol::object color = (*colors)[i];
                    if (!color.is<Common::Color>())
                    {
                        Raise(caller, std::format("colour {} is not a Color", i));
                    }
                    mesh.colors.push_back(color.as<Common::Color>());
                }
            }
            if (const auto uvs = OptionalTable(desc, "uvs", caller))
            {
                const std::size_t count = uvs->size();
                if (count > Rendering::kMaxScriptMeshVertices)
                {
                    Raise(caller, std::format("{} uvs, over the limit of {}", count, Rendering::kMaxScriptMeshVertices));
                }
                for (std::size_t i = 1; i <= count; ++i)
                {
                    const sol::object uv = (*uvs)[i];
                    if (!uv.is<Math::Vector2>())
                    {
                        Raise(caller, std::format("uv {} is not a Vector2", i));
                    }
                    mesh.uvs.push_back(uv.as<Math::Vector2>());
                }
            }

            if (const auto indices = OptionalTable(desc, "indices", caller))
            {
                const std::size_t count = indices->size();
                if (count > Rendering::kMaxScriptMeshIndices)
                {
                    Raise(caller, std::format("{} indices, over the limit of {}", count, Rendering::kMaxScriptMeshIndices));
                }
                mesh.indexBase = 1;
                mesh.indices.reserve(count);
                for (std::size_t i = 1; i <= count; ++i)
                {
                    const sol::object index = (*indices)[i];
                    if (index.get_type() != sol::type::number)
                    {
                        Raise(caller, std::format("index {} is not a number", i));
                    }
                    const double value = index.as<double>();
                    // Integers only, and small enough to hold exactly (any real index is far smaller)
                    if (!std::isfinite(value) || std::floor(value) != value || std::abs(value) > 9007199254740992.0)
                    {
                        Raise(caller, std::format("index {} is not a whole number", i));
                    }
                    mesh.indices.push_back(static_cast<std::int64_t>(value));
                }
            }
            else
            {
                std::vector<Math::Vector2> outline;
                outline.reserve(mesh.positions.size());
                for (const Math::Vector3 &p : mesh.positions)
                {
                    outline.emplace_back(p.x, p.y);
                }
                std::string error;
                const std::vector<std::uint32_t> triangles = Rendering::TriangulatePolygon2D(outline, &error);
                if (triangles.empty())
                {
                    Raise(caller, "without indices the vertices must be a simple polygon, but " + error);
                }
                mesh.indexBase = 0;
                mesh.indices.assign(triangles.begin(), triangles.end());
            }

            std::string error;
            auto data = Rendering::MakeScriptMeshData(mesh, &error);
            if (!data)
            {
                Raise(caller, error);
            }
            return std::move(*data);
        }

        /// 1-based point index from Lua as 0-based, checked against `count`
        std::size_t PointIndex(const std::int64_t index, const std::size_t count, const std::string_view caller)
        {
            if (index < 1 || static_cast<std::uint64_t>(index) > count)
            {
                Raise(caller, std::format("index {} is outside the points 1 to {}", index, count));
            }
            return static_cast<std::size_t>(index - 1);
        }

        sol::table PointsToLua(sol::this_state state, const std::vector<Math::Vector3> &points)
        {
            sol::table list = sol::state_view(state).create_table(static_cast<int>(points.size()), 0);
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                list[i + 1] = points[i];
            }
            return list;
        }
    }

    std::shared_ptr<Rendering::Mesh> MeshArgumentOrThrow(const sol::object &mesh, const std::string_view caller)
    {
        if (mesh.get_type() == sol::type::lua_nil)
        {
            return nullptr;
        }
        if (mesh.is<LuaMeshHandle>())
        {
            return mesh.as<LuaMeshHandle>().mesh;
        }
        if (mesh.get_type() == sol::type::string)
        {
            return LoadMeshOrThrow(mesh.as<std::string>(), caller);
        }
        Raise(caller, "expected a ProceduralMesh (Mesh.Create), a built-in mesh's name, a mesh asset's path or nil");
    }

    void BindGeometry2D(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        // ===== ProceduralMesh and the Mesh table =====
        lua.new_usertype<LuaMeshHandle>("ProceduralMesh", sol::no_constructor,
            // Replaces the geometry with a new description (the same rules as Mesh.Create); every renderer drawing it updates
            "SetData", [](LuaMeshHandle &handle, const sol::table &desc)
            {
                constexpr std::string_view caller = "ProceduralMesh:SetData";
                Renderer::Common::MeshData data = ReadMeshDesc(desc, caller);
                if (handle.mesh->IsBuiltin())
                {
                    Raise(caller, "a built-in mesh is shared and can't be changed");
                }
                if (!handle.mesh->SetData(std::move(data)))
                {
                    Raise(caller, "the mesh was refused");
                }
            },
            "GetVertexCount", [](const LuaMeshHandle &handle) { return static_cast<std::int64_t>(handle.mesh->GetVertices().size()); },
            "GetIndexCount", [](const LuaMeshHandle &handle) { return static_cast<std::int64_t>(handle.mesh->GetIndices().size()); },
            "GetBounds", [](const LuaMeshHandle &handle) { return handle.mesh->GetBounds(); },
            sol::meta_function::equal_to, [](const LuaMeshHandle &a, const LuaMeshHandle &b) { return a.mesh == b.mesh; });

        lua["Mesh"] = lua.create_table_with(
            "Create", [](const sol::table &desc)
            {
                constexpr std::string_view caller = "Mesh.Create";
                auto mesh = Rendering::Mesh::Create(ReadMeshDesc(desc, caller));
                if (!mesh)
                {
                    Raise(caller, "the mesh was refused");
                }
                return LuaMeshHandle{std::move(mesh)};
            },
            // Triangles (1-based indices, three per triangle) covering a simple polygon given as Vector2 points
            "Triangulate", [](const sol::table &points, sol::this_state state)
            {
                constexpr std::string_view caller = "Mesh.Triangulate";
                const std::vector<Math::Vector3> read = ReadPoints(points, Rendering::kMaxTriangulatePoints, caller);
                std::vector<Math::Vector2> outline;
                outline.reserve(read.size());
                for (const Math::Vector3 &p : read)
                {
                    outline.emplace_back(p.x, p.y);
                }
                std::string error;
                const std::vector<std::uint32_t> triangles = Rendering::TriangulatePolygon2D(outline, &error);
                if (triangles.empty())
                {
                    Raise(caller, error);
                }
                sol::table list = sol::state_view(state).create_table(static_cast<int>(triangles.size()), 0);
                for (std::size_t i = 0; i < triangles.size(); ++i)
                {
                    list[i + 1] = static_cast<std::int64_t>(triangles[i]) + 1;
                }
                return list;
            },
            "MaxVertices", static_cast<std::int64_t>(Rendering::kMaxScriptMeshVertices),
            "MaxIndices", static_cast<std::int64_t>(Rendering::kMaxScriptMeshIndices)
        );

        // ===== LineRenderer =====
        using LineRef = ComponentRef<Rendering::LineRenderer>;
        BindComponentType<Rendering::LineRenderer>(
            lua, "LineRenderer",
            "SetPositions", [](const LineRef &c, const sol::table &points)
            {
                const auto positions = ReadPoints(points, Rendering::kMaxLinePoints, "LineRenderer:SetPositions");
                c.Pin()->SetPositions(positions);
            },
            "GetPositions", [](const LineRef &c, sol::this_state state) { return PointsToLua(state, c.Pin()->GetPositions()); },
            "SetPosition", [](const LineRef &c, const std::int64_t index, const sol::object &point)
            {
                constexpr std::string_view caller = "LineRenderer:SetPosition";
                const auto line = c.Pin();
                line->SetPosition(PointIndex(index, line->GetPositionCount(), caller), PointOrThrow(point, caller));
            },
            "GetPosition", [](const LineRef &c, const std::int64_t index)
            {
                const auto line = c.Pin();
                return line->GetPositions()[PointIndex(index, line->GetPositionCount(), "LineRenderer:GetPosition")];
            },
            "SetPositionCount", [](const LineRef &c, const std::int64_t count)
            {
                if (count < 0 || static_cast<std::uint64_t>(count) > Rendering::kMaxLinePoints)
                {
                    Raise("LineRenderer:SetPositionCount", std::format("the count must be 0 to {}, not {}", Rendering::kMaxLinePoints, count));
                }
                c.Pin()->SetPositionCount(static_cast<std::size_t>(count));
            },
            "GetPositionCount", [](const LineRef &c) { return static_cast<std::int64_t>(c.Pin()->GetPositionCount()); },
            "SetWidth", Forward<LineRef, &Rendering::LineRenderer::SetWidth>(),
            "SetStartWidth", Forward<LineRef, &Rendering::LineRenderer::SetStartWidth>(),
            "GetStartWidth", Forward<LineRef, &Rendering::LineRenderer::GetStartWidth>(),
            "SetEndWidth", Forward<LineRef, &Rendering::LineRenderer::SetEndWidth>(),
            "GetEndWidth", Forward<LineRef, &Rendering::LineRenderer::GetEndWidth>(),
            "SetColor", Forward<LineRef, &Rendering::LineRenderer::SetColor>(),
            "SetStartColor", Forward<LineRef, &Rendering::LineRenderer::SetStartColor>(),
            "GetStartColor", Forward<LineRef, &Rendering::LineRenderer::GetStartColor>(),
            "SetEndColor", Forward<LineRef, &Rendering::LineRenderer::SetEndColor>(),
            "GetEndColor", Forward<LineRef, &Rendering::LineRenderer::GetEndColor>(),
            "SetLoop", Forward<LineRef, &Rendering::LineRenderer::SetLoop>(),
            "GetLoop", Forward<LineRef, &Rendering::LineRenderer::GetLoop>(),
            "SetUseWorldSpace", Forward<LineRef, &Rendering::LineRenderer::SetUseWorldSpace>(),
            "GetUseWorldSpace", Forward<LineRef, &Rendering::LineRenderer::GetUseWorldSpace>(),
            // "Miter" or "Bevel"
            "SetJoin", [](const LineRef &c, const std::string &name)
            { c.Pin()->SetJoin(ParseName<Rendering::LineJoin>(name, "LineRenderer:SetJoin")); },
            "GetJoin", [](const LineRef &c) { return NameOf(c.Pin()->GetJoin()); },
            "SetMiterLimit", Forward<LineRef, &Rendering::LineRenderer::SetMiterLimit>(),
            "GetMiterLimit", Forward<LineRef, &Rendering::LineRenderer::GetMiterLimit>(),
            // "Butt" or "Square"
            "SetCap", [](const LineRef &c, const std::string &name)
            { c.Pin()->SetCap(ParseName<Rendering::LineCap>(name, "LineRenderer:SetCap")); },
            "GetCap", [](const LineRef &c) { return NameOf(c.Pin()->GetCap()); },
            // "Alpha" or "Additive"
            "SetBlendMode", [](const LineRef &c, const std::string &name)
            { c.Pin()->SetBlendMode(ParseName<Rendering::Blend2D>(name, "LineRenderer:SetBlendMode")); },
            "GetBlendMode", [](const LineRef &c) { return NameOf(c.Pin()->GetBlendMode()); },
            "SetSortingOrder", Forward<LineRef, &Rendering::LineRenderer::SetSortingOrder>(),
            "GetSortingOrder", Forward<LineRef, &Rendering::LineRenderer::GetSortingOrder>()
        );

        // ===== TrailRenderer =====
        using TrailRef = ComponentRef<Rendering::TrailRenderer>;
        BindComponentType<Rendering::TrailRenderer>(
            lua, "TrailRenderer",
            "Clear", Forward<TrailRef, &Rendering::TrailRenderer::Clear>(),
            "GetPointCount", [](const TrailRef &c) { return static_cast<std::int64_t>(c.Pin()->GetPointCount()); },
            "SetTime", Forward<TrailRef, &Rendering::TrailRenderer::SetTime>(),
            "GetTime", Forward<TrailRef, &Rendering::TrailRenderer::GetTime>(),
            "SetMinVertexDistance", Forward<TrailRef, &Rendering::TrailRenderer::SetMinVertexDistance>(),
            "GetMinVertexDistance", Forward<TrailRef, &Rendering::TrailRenderer::GetMinVertexDistance>(),
            "SetEmitting", Forward<TrailRef, &Rendering::TrailRenderer::SetEmitting>(),
            "IsEmitting", Forward<TrailRef, &Rendering::TrailRenderer::IsEmitting>(),
            "SetWidth", Forward<TrailRef, &Rendering::TrailRenderer::SetWidth>(),
            "SetStartWidth", Forward<TrailRef, &Rendering::TrailRenderer::SetStartWidth>(),
            "GetStartWidth", Forward<TrailRef, &Rendering::TrailRenderer::GetStartWidth>(),
            "SetEndWidth", Forward<TrailRef, &Rendering::TrailRenderer::SetEndWidth>(),
            "GetEndWidth", Forward<TrailRef, &Rendering::TrailRenderer::GetEndWidth>(),
            "SetColor", Forward<TrailRef, &Rendering::TrailRenderer::SetColor>(),
            "SetStartColor", Forward<TrailRef, &Rendering::TrailRenderer::SetStartColor>(),
            "GetStartColor", Forward<TrailRef, &Rendering::TrailRenderer::GetStartColor>(),
            "SetEndColor", Forward<TrailRef, &Rendering::TrailRenderer::SetEndColor>(),
            "GetEndColor", Forward<TrailRef, &Rendering::TrailRenderer::GetEndColor>(),
            "SetJoin", [](const TrailRef &c, const std::string &name)
            { c.Pin()->SetJoin(ParseName<Rendering::LineJoin>(name, "TrailRenderer:SetJoin")); },
            "GetJoin", [](const TrailRef &c) { return NameOf(c.Pin()->GetJoin()); },
            "SetMiterLimit", Forward<TrailRef, &Rendering::TrailRenderer::SetMiterLimit>(),
            "GetMiterLimit", Forward<TrailRef, &Rendering::TrailRenderer::GetMiterLimit>(),
            "SetBlendMode", [](const TrailRef &c, const std::string &name)
            { c.Pin()->SetBlendMode(ParseName<Rendering::Blend2D>(name, "TrailRenderer:SetBlendMode")); },
            "GetBlendMode", [](const TrailRef &c) { return NameOf(c.Pin()->GetBlendMode()); },
            "SetSortingOrder", Forward<TrailRef, &Rendering::TrailRenderer::SetSortingOrder>(),
            "GetSortingOrder", Forward<TrailRef, &Rendering::TrailRenderer::GetSortingOrder>()
        );

        // ===== MeshRenderer2D =====
        using Mesh2DRef = ComponentRef<Rendering::MeshRenderer2D>;
        BindComponentType<Rendering::MeshRenderer2D>(
            lua, "MeshRenderer2D",
            // A ProceduralMesh, a built-in mesh's name, a mesh asset's path, or nil
            "SetMesh", [](const Mesh2DRef &c, const sol::object &mesh)
            { c.Pin()->SetMesh(MeshArgumentOrThrow(mesh, "MeshRenderer2D:SetMesh")); },
            // The mesh as a ProceduralMesh handle (also for an asset or built-in one), or nil
            "GetMesh", [](const Mesh2DRef &c) -> sol::optional<LuaMeshHandle>
            {
                const auto renderer = c.Pin();
                if (!renderer->GetMesh())
                {
                    return sol::nullopt;
                }
                return LuaMeshHandle{renderer->GetMesh()};
            },
            "SetColor", Forward<Mesh2DRef, &Rendering::MeshRenderer2D::SetColor>(),
            "GetColor", Forward<Mesh2DRef, &Rendering::MeshRenderer2D::GetColor>(),
            // An image file, e.g. "res://sprites/rock.png"; nil for the vertex colours alone. One that doesn't load is an error
            "SetTexture", [](const Mesh2DRef &c, const sol::optional<std::string> path)
            { c.Pin()->SetTexture(path ? LoadTextureOrThrow(*path, "MeshRenderer2D:SetTexture") : nullptr); },
            "GetTexture", [](const Mesh2DRef &c) -> sol::optional<std::string>
            {
                const auto renderer = c.Pin();
                if (!renderer->GetTexture())
                {
                    return sol::nullopt;
                }
                return TexturePathForLua(*renderer->GetTexture());
            },
            "SetBlendMode", [](const Mesh2DRef &c, const std::string &name)
            { c.Pin()->SetBlendMode(ParseName<Rendering::Blend2D>(name, "MeshRenderer2D:SetBlendMode")); },
            "GetBlendMode", [](const Mesh2DRef &c) { return NameOf(c.Pin()->GetBlendMode()); },
            "SetSortingOrder", Forward<Mesh2DRef, &Rendering::MeshRenderer2D::SetSortingOrder>(),
            "GetSortingOrder", Forward<Mesh2DRef, &Rendering::MeshRenderer2D::GetSortingOrder>()
        );
    }
}
