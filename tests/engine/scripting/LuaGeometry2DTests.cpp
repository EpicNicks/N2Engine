// LineRenderer, TrailRenderer, MeshRenderer2D and the Mesh table from Lua (docs/meshes.html#lines, issue #445 item 5): every setter
// and getter, and the checks on a script's mesh, which are Lua errors, never a crash.

#include <gtest/gtest.h>

#include <string>

#include "engine/GameObject.hpp"
#include "engine/rendering/LineRenderer.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/MeshRenderer2D.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

namespace
{
    sol::state &Lua()
    {
        return LuaRuntime::Instance().GetState();
    }

    void RunLua(const std::string &code)
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error error = result;
            FAIL() << error.what() << "\nin: " << code;
        }
    }

    /// The error `code` raises (empty when it raises none)
    std::string ErrorOf(const std::string &code)
    {
        EXPECT_TRUE(LuaRuntime::Instance().Initialize());
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            return {};
        }
        const sol::error error = result;
        return error.what();
    }
}

TEST(LuaGeometry2DTest, AScriptCanSetEveryLineField)
{
    RunLua(R"(
        local go = GameObject.Create('LuaLine')
        local line = go:AddComponent('LineRenderer')
        line:SetPositions({Vector2(0, 0), Vector3(1, 0, 2), Vector2(1, 1)})
        assert(line:GetPositionCount() == 3)
        assert(line:GetPosition(2).z == 2)
        line:SetPosition(3, Vector2(2, 2))
        assert(line:GetPositions()[3].x == 2)
        line:SetPositionCount(5)
        assert(line:GetPositionCount() == 5 and line:GetPosition(5).x == 0)
        line:SetWidth(0.5)
        assert(line:GetStartWidth() == 0.5 and line:GetEndWidth() == 0.5)
        line:SetEndWidth(0.25)
        assert(line:GetEndWidth() == 0.25)
        line:SetColor(Color.Red)
        assert(line:GetEndColor().r == 1)
        line:SetEndColor(Color.Blue)
        assert(line:GetEndColor().b == 1 and line:GetStartColor().r == 1)
        line:SetLoop(true)
        assert(line:GetLoop())
        line:SetUseWorldSpace(true)
        assert(line:GetUseWorldSpace())
        line:SetJoin('Bevel')
        assert(line:GetJoin() == 'Bevel')
        line:SetMiterLimit(2)
        assert(line:GetMiterLimit() == 2)
        line:SetCap('Square')
        assert(line:GetCap() == 'Square')
        line:SetBlendMode('Additive')
        assert(line:GetBlendMode() == 'Additive')
        line:SetSortingOrder(4)
        assert(line:GetSortingOrder() == 4)
        assert(go:GetComponent('LineRenderer') == line)

        assert(not pcall(function() line:SetPosition(6, Vector2(0, 0)) end))
        assert(not pcall(function() line:SetPosition(0, Vector2(0, 0)) end))
        assert(not pcall(function() line:GetPosition(99) end))
        assert(not pcall(function() line:SetPositions({Vector2(0, 0), 'corner'}) end))
        assert(not pcall(function() line:SetJoin('Round') end))
        assert(not pcall(function() line:SetBlendMode('Screen') end))
        assert(not pcall(function() line:SetPositionCount(-1) end))
    )");
}

TEST(LuaGeometry2DTest, AScriptCanSetEveryTrailField)
{
    RunLua(R"(
        local go = GameObject.Create('LuaTrail')
        local trail = go:AddComponent('TrailRenderer')
        trail:SetTime(0.5)
        assert(trail:GetTime() == 0.5)
        trail:SetMinVertexDistance(0.2)
        assert(math.abs(trail:GetMinVertexDistance() - 0.2) < 1e-6)
        trail:SetEmitting(false)
        assert(not trail:IsEmitting())
        trail:SetWidth(0.3)
        assert(math.abs(trail:GetStartWidth() - 0.3) < 1e-6)
        trail:SetStartWidth(1)
        trail:SetEndWidth(0)
        assert(trail:GetStartWidth() == 1 and trail:GetEndWidth() == 0)
        trail:SetColor(Color.Green)
        trail:SetEndColor(Color(0, 1, 0, 0))
        assert(trail:GetStartColor().g == 1 and trail:GetEndColor().a == 0)
        trail:SetJoin('Bevel')
        assert(trail:GetJoin() == 'Bevel')
        trail:SetMiterLimit(3)
        assert(trail:GetMiterLimit() == 3)
        trail:SetBlendMode('Additive')
        assert(trail:GetBlendMode() == 'Additive')
        trail:SetSortingOrder(-1)
        assert(trail:GetSortingOrder() == -1)
        trail:Clear()
        assert(trail:GetPointCount() == 0)
    )");
}

TEST(LuaGeometry2DTest, AScriptBuildsAConcaveMeshAndShowsIt)
{
    RunLua(R"(
        local l = {Vector2(0, 0), Vector2(2, 0), Vector2(2, 1), Vector2(1, 1), Vector2(1, 2), Vector2(0, 2)}
        local tris = Mesh.Triangulate(l)
        assert(#tris == 12)
        for _, i in ipairs(tris) do assert(i >= 1 and i <= 6) end

        local mesh = Mesh.Create({vertices = l, indices = tris})
        assert(mesh:GetVertexCount() == 6 and mesh:GetIndexCount() == 12)
        -- Without indices the outline is triangulated
        local auto = Mesh.Create({vertices = l})
        assert(auto:GetIndexCount() == 12)
        assert(auto:GetBounds().max.x == 2)

        local go = GameObject.Create('LuaMesh2D')
        local r = go:AddComponent('MeshRenderer2D')
        r:SetMesh(mesh)
        assert(r:GetMesh() == mesh)
        r:SetColor(Color.Red)
        assert(r:GetColor().r == 1)
        r:SetBlendMode('Additive')
        assert(r:GetBlendMode() == 'Additive')
        r:SetSortingOrder(2)
        assert(r:GetSortingOrder() == 2)
        r:SetTexture(nil)
        assert(r:GetTexture() == nil)
        r:SetMesh('Quad')
        assert(r:GetMesh():GetVertexCount() == 4)
        r:SetMesh(nil)
        assert(r:GetMesh() == nil)

        -- Colours and uvs, one per vertex; SetData replaces the geometry
        mesh:SetData({
            vertices = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)},
            colors = {Color.Red, Color.Green, Color.Blue},
            uvs = {Vector2(0, 0), Vector2(1, 0), Vector2(0, 1)},
            indices = {1, 2, 3},
        })
        assert(mesh:GetVertexCount() == 3)

        -- A MeshRenderer takes one too
        local m = GameObject.Create('LuaMesh3D'):AddComponent('MeshRenderer')
        m:SetMesh(mesh)
        assert(m:GetMesh() == '')
    )");
}

TEST(LuaGeometry2DTest, EveryBadMeshIsALuaErrorThatSaysWhy)
{
    const std::string tri = "vertices = {Vector2(0, 0), Vector2(1, 0), Vector2(0, 1)}";
    const auto expectError = [](const std::string &code, const std::string &reason)
    {
        const std::string error = ErrorOf(code);
        EXPECT_NE(error.find(reason), std::string::npos) << "code: " << code << "\nerror: " << error;
    };
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 4}})", "outside the vertices 1 to 3");
    expectError("Mesh.Create({" + tri + ", indices = {0, 1, 2}})", "outside the vertices");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, -7}})", "outside the vertices");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2}})", "whole triangles");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 2.5}})", "not a whole number");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 'three'}})", "not a number");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 1e300}})", "not a whole number");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 3}, colors = {Color.Red}})", "colours for 3 vertices");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 3}, uvs = {Vector2(0, 0)}})", "uvs for 3 vertices");
    expectError("Mesh.Create({" + tri + ", indices = {1, 2, 3}, colors = {1, 2, 3}})", "not a Color");
    expectError("Mesh.Create({" + tri + ", indices = 5})", "'indices' must be a table");
    expectError("Mesh.Create({indices = {1, 2, 3}})", "needs 'vertices'");
    expectError("Mesh.Create({vertices = {Vector2(0, 0), 'x', Vector2(0, 1)}, indices = {1, 2, 3}})", "not a Vector2 or a Vector3");
    expectError("Mesh.Create({vertices = {Vector2(0, 0), Vector2(1, 0), Vector2(2, 0)}})", "simple polygon");
    expectError("Mesh.Triangulate({Vector2(0, 0), Vector2(1, 0)})", "3 to");
    // Over the limit: refused before anything is read
    expectError("local v = {} for i = 1, Mesh.MaxVertices + 1 do v[i] = Vector2(i, 0) end Mesh.Create({vertices = v, indices = {1, 2, 3}})",
                "over the limit");
    expectError("local i = {} for k = 1, Mesh.MaxIndices + 3 do i[k] = 1 end Mesh.Create({" + tri + ", indices = i})", "over the limit");
    // A built-in mesh can't be changed through its handle
    expectError("local r = GameObject.Create('B'):AddComponent('MeshRenderer2D') r:SetMesh('Quad') r:GetMesh():SetData({" + tri +
                    ", indices = {1, 2, 3}})",
                "built-in");
    expectError("GameObject.Create('M'):AddComponent('MeshRenderer'):SetMesh(42)", "expected a ProceduralMesh");

    // A failed SetData keeps the mesh as it was
    RunLua("bad_mesh = Mesh.Create({" + tri + ", indices = {1, 2, 3}}) "
        "assert(not pcall(function() bad_mesh:SetData({" + tri + ", indices = {1, 2, 9}}) end)) "
        "assert(bad_mesh:GetIndexCount() == 3) bad_mesh = nil");
}
