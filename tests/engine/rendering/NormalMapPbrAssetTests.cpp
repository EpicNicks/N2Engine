#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>

#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Texture.hpp"

#include "MeshTestSupport.hpp"
#include "ModelTestSupport.hpp"
#include "TextureTestSupport.hpp"

// Normal maps and the PBR shading as assets (#3 P4b): the tangents the built-in meshes carry, Material's Pbr shading
// and normal scale in .mat JSON and as uniforms, the GPU material made for them (GpuCache), and what the Model asset
// does with a glTF's tangents, normal texture and metallic-roughness texture.

using namespace N2Engine;
using Rendering::GpuCache;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::Model;
using Rendering::ModelSettings;
using Rendering::ModelTangents;
using Rendering::ShadingModel;
using Rendering::Texture;
using MeshTestSupport::RecordingMeshRenderer;
using MeshTestSupport::WarningCapture;
using json = nlohmann::json;
using Renderer::Common::Vertex;

namespace
{
    using Vec3 = std::array<float, 3>;

    Vec3 Sub(const float *a, const float *b)
    {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }

    float Dot(const Vec3 &a, const Vec3 &b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    Vec3 Cross(const Vec3 &a, const Vec3 &b)
    {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }

    Vec3 Normalised(const Vec3 &v)
    {
        const float length = std::sqrt(Dot(v, v));
        return length > 0.0f ? Vec3{v[0] / length, v[1] / length, v[2] / length} : Vec3{0, 0, 0};
    }

    /// For each triangle of a mesh whose uv mapping isn't degenerate, the directions of increasing u and of
    /// increasing v on its surface (the true tangent and bitangent), compared with what its vertices carry:
    /// tangent along u, and cross(normal, tangent) * w along v. `minimum` is the least dot product accepted.
    /// Returns how many triangles were checked.
    std::size_t ExpectTangentsFollowTheUvs(const Mesh &mesh, const float minimum, const std::string &what)
    {
        const auto &vertices = mesh.GetVertices();
        const auto &indices = mesh.GetIndices();
        std::size_t checked = 0;
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const Vertex &a = vertices[indices[t]];
            const Vertex &b = vertices[indices[t + 1]];
            const Vertex &c = vertices[indices[t + 2]];
            const Vec3 e1 = Sub(b.position, a.position);
            const Vec3 e2 = Sub(c.position, a.position);
            const float du1 = b.texCoord[0] - a.texCoord[0], dv1 = b.texCoord[1] - a.texCoord[1];
            const float du2 = c.texCoord[0] - a.texCoord[0], dv2 = c.texCoord[1] - a.texCoord[1];
            const float det = du1 * dv2 - du2 * dv1;
            const Vec3 faceNormal = Cross(e1, e2);
            if (std::fabs(det) < 1e-6f || Dot(faceNormal, faceNormal) < 1e-10f)
            {
                continue; // a degenerate mapping or a zero-area triangle (a pole of the sphere)
            }
            const Vec3 dPdu = {(e1[0] * dv2 - e2[0] * dv1) / det, (e1[1] * dv2 - e2[1] * dv1) / det,
                               (e1[2] * dv2 - e2[2] * dv1) / det};
            const Vec3 dPdv = {(e2[0] * du1 - e1[0] * du2) / det, (e2[1] * du1 - e1[1] * du2) / det,
                               (e2[2] * du1 - e1[2] * du2) / det};
            for (const Vertex *vertex : {&a, &b, &c})
            {
                const Vec3 tangent = {vertex->tangent[0], vertex->tangent[1], vertex->tangent[2]};
                const Vec3 normal = {vertex->normal[0], vertex->normal[1], vertex->normal[2]};
                EXPECT_NEAR(Dot(tangent, tangent), 1.0f, 1e-3f) << what << " triangle " << t / 3 << ": unit tangent";
                EXPECT_NEAR(Dot(tangent, normal), 0.0f, 1e-3f) << what << " triangle " << t / 3 << ": perpendicular";
                EXPECT_GT(Dot(tangent, Normalised(dPdu)), minimum) << what << " triangle " << t / 3 << ": along u";
                const Vec3 bitangent = Cross(normal, tangent);
                const float w = vertex->tangent[3];
                EXPECT_TRUE(w == 1.0f || w == -1.0f) << what;
                EXPECT_GT(Dot({bitangent[0] * w, bitangent[1] * w, bitangent[2] * w}, Normalised(dPdv)), minimum)
                    << what << " triangle " << t / 3 << ": the bitangent along v";
            }
            ++checked;
        }
        return checked;
    }

    std::shared_ptr<Texture> DataTexture()
    {
        Rendering::TextureSettings data;
        data.srgb = false;
        return Texture::CreateFromEncoded(TextureTestSupport::MakeBmp(1, 1, {{9, 9, 9}}), data);
    }

    /// The colour space is process-wide: back to gamma around a test
    struct LinearGuard
    {
        LinearGuard() { Rendering::RenderSettings::SetColorSpace(Renderer::Common::ColorSpace::Linear); }
        ~LinearGuard() { Rendering::RenderSettings::SetColorSpace(Renderer::Common::ColorSpace::Gamma); }
    };
}

// ============================================================================
// The vertex and the built-in meshes
// ============================================================================

TEST(VertexTangentTest, TheVertexIs64BytesWithTheTangentLast)
{
    static_assert(sizeof(Vertex) == 64);
    static_assert(offsetof(Vertex, tangent) == 48);
    // A positional initialiser that stops at the colour leaves the tangent zero: no tangent
    const Vertex plain{{1, 2, 3}, {0, 0, 1}, {0.5f, 0.5f}, {1, 1, 1, 1}};
    for (const float component : plain.tangent)
    {
        EXPECT_EQ(component, 0.0f);
    }
}

TEST(VertexTangentTest, TheBuiltInMeshesCarryTangentsThatFollowTheirUvs)
{
    const auto cube = Mesh::GetBuiltin(Rendering::BuiltinMesh::Cube);
    EXPECT_EQ(ExpectTangentsFollowTheUvs(*cube, 0.999f, "cube"), 12u) << "every one of its 12 triangles";
    const auto quad = Mesh::GetBuiltin(Rendering::BuiltinMesh::Quad);
    EXPECT_EQ(ExpectTangentsFollowTheUvs(*quad, 0.999f, "quad"), 2u);
    const auto sphere = Mesh::GetBuiltin(Rendering::BuiltinMesh::Sphere);
    // The sphere's triangles are curved: the vertex tangent is the analytic one, close to each flat triangle's
    EXPECT_GT(ExpectTangentsFollowTheUvs(*sphere, 0.9f, "sphere"), 100u);
}

TEST(VertexTangentTest, ARuntimeMeshKeepsItsTangentsThroughSetData)
{
    Renderer::Common::MeshData data = Mesh::MakeQuad();
    data.vertices[2].tangent[0] = 0.0f;
    data.vertices[2].tangent[1] = 1.0f;
    data.vertices[2].tangent[2] = 0.0f;
    data.vertices[2].tangent[3] = -1.0f;
    const auto mesh = Mesh::Create(std::move(data));
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->GetVertices()[2].tangent[1], 1.0f);
    EXPECT_EQ(mesh->GetVertices()[2].tangent[3], -1.0f);
    EXPECT_EQ(mesh->GetVertices()[0].tangent[0], 1.0f) << "the quad's own";

    // And through the renderer's copy of it
    const std::size_t baseline = GpuCache::GetEntryCount();
    RecordingMeshRenderer renderer;
    GpuCache::Handle handle = GpuCache::AcquireMesh(renderer, mesh);
    ASSERT_TRUE(handle);
    ASSERT_EQ(renderer.meshes.size(), 1u);
    EXPECT_EQ(renderer.meshes[0]->vertices[2].tangent[1], 1.0f);
    EXPECT_EQ(renderer.meshes[0]->vertices[2].tangent[3], -1.0f);
    handle.Release(true);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

// ============================================================================
// Material: the Pbr shading and the normal scale
// ============================================================================

TEST(MaterialPbrTest, NothingIsPbrOrNormalMappedUnlessAskedFor)
{
    const auto material = Material::Create();
    EXPECT_EQ(material->GetShading(), ShadingModel::Lit);
    EXPECT_FLOAT_EQ(material->GetNormalScale(), 1.0f);
    EXPECT_EQ(material->GetNormalTexture(), nullptr);
    EXPECT_EQ(material->GetMetallicRoughnessTexture(), nullptr);
    EXPECT_EQ(Material::GetDefault()->GetShading(), ShadingModel::Lit);
    EXPECT_EQ(Material::GetDefaultUnlit()->GetShading(), ShadingModel::Unlit);
    EXPECT_EQ(Material::FromJson(json::object())->GetShading(), ShadingModel::Lit) << "an old .mat is as it was";
}

TEST(MaterialPbrTest, PbrAndTheNormalScaleRoundTripThroughMatJson)
{
    WarningCapture capture;
    const auto material = Material::FromJson(
        json{{"shading", "pbr"}, {"metallic", 0.75}, {"smoothness", 0.25}, {"normalScale", 2.5}});
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(capture.messages.empty());
    EXPECT_EQ(material->GetShading(), ShadingModel::Pbr);
    EXPECT_FLOAT_EQ(material->GetMetallic(), 0.75f);
    EXPECT_FLOAT_EQ(material->GetSmoothness(), 0.25f);
    EXPECT_FLOAT_EQ(material->GetNormalScale(), 2.5f);

    const json saved = material->ToJson();
    EXPECT_EQ(saved.at("shading"), "pbr");
    EXPECT_FLOAT_EQ(saved.at("normalScale").get<float>(), 2.5f);
    EXPECT_EQ(Material::FromJson(saved)->ToJson(), saved);

    // The shading name is case-insensitive like the others
    EXPECT_EQ(Material::FromJson(json{{"shading", "PBR"}})->GetShading(), ShadingModel::Pbr);
}

TEST(MaterialPbrTest, AnOldMatFileLoadsUnchangedAndGainsTheNewKeysOnSave)
{
    // What a P4a-era material file wrote: no normalScale
    const json old = {{"shading", "lit"},
                      {"baseColor", {{"r", 0.5}, {"g", 0.5}, {"b", 0.5}, {"a", 1.0}}},
                      {"smoothness", 0.4},
                      {"metallic", 0.2},
                      {"emissive", {{"r", 0.1}, {"g", 0.0}, {"b", 0.0}, {"a", 1.0}}},
                      {"normalTexture", nullptr},
                      {"occlusionStrength", 0.5}};
    WarningCapture capture;
    const auto material = Material::FromJson(old);
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(capture.messages.empty()) << "no warning for a file without the new keys";
    EXPECT_EQ(material->GetShading(), ShadingModel::Lit);
    EXPECT_FLOAT_EQ(material->GetNormalScale(), 1.0f);
    EXPECT_FLOAT_EQ(material->GetSmoothness(), 0.4f);
    EXPECT_FLOAT_EQ(material->GetMetallic(), 0.2f);
    EXPECT_EQ(material->ToJson().at("normalScale"), 1.0f);
    EXPECT_EQ(material->ToJson().at("shading"), "lit");
}

TEST(MaterialPbrTest, BadValuesAreWarnedAboutAndTheNormalScaleIsClamped)
{
    {
        WarningCapture capture;
        const auto material = Material::FromJson(json{{"shading", "phong"}});
        EXPECT_EQ(material->GetShading(), ShadingModel::Lit) << "kept";
        ASSERT_EQ(capture.messages.size(), 1u);
        EXPECT_NE(capture.messages[0].find("pbr"), std::string::npos) << capture.messages[0];
    }
    {
        WarningCapture capture;
        const auto material = Material::FromJson(json{{"normalScale", "strong"}});
        EXPECT_FLOAT_EQ(material->GetNormalScale(), 1.0f);
        EXPECT_EQ(capture.messages.size(), 1u);
    }
    {
        WarningCapture capture;
        EXPECT_FLOAT_EQ(Material::FromJson(json{{"normalScale", 100}})->GetNormalScale(), 4.0f);
        EXPECT_FLOAT_EQ(Material::FromJson(json{{"normalScale", -3}})->GetNormalScale(), 0.0f);
        EXPECT_EQ(capture.messages.size(), 2u) << "one warning each";
    }
    const auto material = Material::Create();
    material->SetNormalScale(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(material->GetNormalScale(), 1.0f);
    material->SetNormalScale(9.0f);
    EXPECT_FLOAT_EQ(material->GetNormalScale(), 4.0f);
    material->SetNormalScale(0.0f);
    EXPECT_FLOAT_EQ(material->GetNormalScale(), 0.0f);
}

TEST(MaterialPbrTest, ApplyUniformsSetsThePbrAndNormalMapInputsForLitAndPbrMaterials)
{
    Renderer::Software::SWShader litShader(Renderer::Software::SWShaderType::Lit);
    Renderer::Software::SWMaterial gpu(&litShader);

    // Lit: Blinn-Phong, no normal map
    const auto lit = Material::Create(ShadingModel::Lit);
    lit->SetNormalScale(2.0f);
    lit->ApplyUniforms(gpu);
    EXPECT_EQ(gpu.GetInt("uPbr", -1), 0);
    EXPECT_EQ(gpu.GetInt("uHasNormalTexture", -1), 0);
    EXPECT_EQ(gpu.GetInt("uHasMetallicRoughnessTexture", -1), 0);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uNormalScale", -1.0f), 2.0f);

    // Pbr, with both textures on the GPU material
    const auto pbr = Material::Create(ShadingModel::Pbr);
    pbr->SetMetallic(0.6f);
    pbr->SetSmoothness(0.3f);
    Renderer::Software::SWTexture normal;
    Renderer::Software::SWTexture mr;
    for (auto *texture : {&normal, &mr})
    {
        texture->data = {1, 2, 3, 4};
        texture->width = texture->height = 1;
        texture->channels = 4;
    }
    gpu.SetAuxTexture(Renderer::Common::AuxTexture::Normal, &normal);
    gpu.SetAuxTexture(Renderer::Common::AuxTexture::MetallicRoughness, &mr);
    pbr->ApplyUniforms(gpu);
    EXPECT_EQ(gpu.GetInt("uPbr", -1), 1);
    EXPECT_EQ(gpu.GetInt("uHasNormalTexture", -1), 1);
    EXPECT_EQ(gpu.GetInt("uHasMetallicRoughnessTexture", -1), 1);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uMetallic", -1.0f), 0.6f);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uSmoothness", -1.0f), 0.3f);
    EXPECT_FLOAT_EQ(gpu.GetFloat("uNormalScale", -1.0f), 1.0f);
    EXPECT_EQ(gpu.GetAuxTexture(Renderer::Common::AuxTexture::Normal), &normal);

    // Back to Lit on the same GPU material: the Pbr flag goes off again
    lit->ApplyUniforms(gpu);
    EXPECT_EQ(gpu.GetInt("uPbr", -1), 0);

    // Unlit reads none of it
    Renderer::Software::SWShader unlitShader(Renderer::Software::SWShaderType::Unlit);
    Renderer::Software::SWMaterial unlitGpu(&unlitShader);
    Material::Create(ShadingModel::Unlit)->ApplyUniforms(unlitGpu);
    EXPECT_EQ(unlitGpu.GetInt("uPbr", -1), -1);
    EXPECT_EQ(unlitGpu.GetInt("uHasNormalTexture", -1), -1);
}

// ============================================================================
// GpuCache: the GPU material of a normal-mapped or Pbr material
// ============================================================================

TEST(GpuCachePbrTest, APbrMaterialIsTheLitShaderWithItsNormalAndMetallicRoughnessTextures)
{
    const std::size_t baseline = GpuCache::GetEntryCount();
    RecordingMeshRenderer renderer;
    const auto material = Material::Create(ShadingModel::Pbr);
    material->SetMetallic(0.5f);
    material->SetNormalTexture(DataTexture());
    material->SetMetallicRoughnessTexture(DataTexture());
    material->SetNormalScale(1.5f);

    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    EXPECT_EQ(renderer.GetCounts().createdTextures, 2);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline + 3) << "the material's entry and its two textures'";
    const auto *sw = RecordingMeshRenderer::AsSW(handle.GetMaterial());
    ASSERT_NE(sw, nullptr);
    EXPECT_EQ(sw->GetShader(), &renderer.litShader) << "Pbr is the standard lit shader, with uPbr set";
    EXPECT_NE(sw->GetAuxTexture(Renderer::Common::AuxTexture::Normal), nullptr);
    EXPECT_NE(sw->GetAuxTexture(Renderer::Common::AuxTexture::MetallicRoughness), nullptr);
    EXPECT_NE(sw->GetAuxTexture(Renderer::Common::AuxTexture::Normal),
              sw->GetAuxTexture(Renderer::Common::AuxTexture::MetallicRoughness));
    EXPECT_EQ(sw->GetInt("uPbr", -1), 1);
    EXPECT_EQ(sw->GetInt("uHasNormalTexture", -1), 1);
    EXPECT_EQ(sw->GetInt("uHasMetallicRoughnessTexture", -1), 1);
    EXPECT_FLOAT_EQ(sw->GetFloat("uNormalScale", -1.0f), 1.5f);

    // A different normal map is a different GPU material
    const std::uint64_t before = handle.GetVersion();
    material->SetNormalTexture(DataTexture());
    EXPECT_NE(material->GetGpuVersion(), before);
    GpuCache::Handle fresh = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh.GetMaterial(), handle.GetMaterial());

    fresh.Release(true);
    handle.Release(true);
    EXPECT_EQ(GpuCache::GetEntryCount(), baseline);
}

TEST(GpuCachePbrTest, NormalAndMetallicRoughnessTexturesAreDataNeverSrgbEvenInLinearLighting)
{
    RecordingMeshRenderer renderer;
    const LinearGuard linear;
    // Made with the default settings, which are sRGB: the material must still not decode them
    const auto colourMarked = Texture::Create(1, 1, std::vector<std::uint8_t>{10, 20, 30, 255});
    ASSERT_TRUE(colourMarked->GetSettings().srgb);
    const auto material = Material::Create(ShadingModel::Pbr);
    material->SetBaseColorTexture(colourMarked);
    material->SetNormalTexture(colourMarked);
    material->SetMetallicRoughnessTexture(colourMarked);

    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    const auto *sw = RecordingMeshRenderer::AsSW(handle.GetMaterial());
    ASSERT_NE(sw, nullptr);
    const auto *base = dynamic_cast<const Renderer::Software::SWTexture *>(sw->GetTexture());
    const auto *normal = dynamic_cast<const Renderer::Software::SWTexture *>(
        sw->GetAuxTexture(Renderer::Common::AuxTexture::Normal));
    const auto *mr = dynamic_cast<const Renderer::Software::SWTexture *>(
        sw->GetAuxTexture(Renderer::Common::AuxTexture::MetallicRoughness));
    ASSERT_NE(base, nullptr);
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(mr, nullptr);
    EXPECT_TRUE(base->options.srgb) << "a Pbr material's colour is sRGB in linear lighting too";
    EXPECT_FALSE(normal->options.srgb);
    EXPECT_FALSE(mr->options.srgb);
    EXPECT_NE(GpuCache::MaterialVersion(*material), material->GetGpuVersion()) << "a lit material in linear lighting";
    handle.Release(true);
}

TEST(GpuCachePbrTest, AnUnlitMaterialNeverGetsTheExtraTextures)
{
    RecordingMeshRenderer renderer;
    const auto material = Material::Create(ShadingModel::Unlit);
    material->SetNormalTexture(DataTexture());
    GpuCache::Handle handle = GpuCache::AcquireMaterial(renderer, material);
    ASSERT_TRUE(handle);
    EXPECT_EQ(renderer.GetCounts().createdTextures, 0);
    EXPECT_EQ(RecordingMeshRenderer::AsSW(handle.GetMaterial())->GetAuxTexture(Renderer::Common::AuxTexture::Normal),
              nullptr);
    handle.Release(true);
}

// ============================================================================
// Model: tangents, the normal texture and PBR materials from a glTF
// ============================================================================

namespace
{
    /// A quad with normals and uvs whose material has a normal texture (scale 2) and a metallic-roughness texture
    ModelTestSupport::Builder BumpyQuad()
    {
        using ModelTestSupport::Builder;
        Builder b;
        const int image = b.AddImage("Bumps", ModelTestSupport::CheckerBmp(), "image/bmp");
        const int mrImage = b.AddImage("MR", ModelTestSupport::CheckerBmp(), "image/bmp");
        const int texture = b.AddTexture(image);
        const int mrTexture = b.AddTexture(mrImage);
        nlohmann::json bumpy;
        bumpy["name"] = "Bumpy";
        bumpy["pbrMetallicRoughness"] = {{"metallicFactor", 0.6}, {"roughnessFactor", 0.3},
                                         {"metallicRoughnessTexture", {{"index", mrTexture}}}};
        bumpy["normalTexture"] = {{"index", texture}, {"scale", 2.0}};
        const int bumpyMaterial = b.AddMaterial(bumpy);
        const int unlitMaterial = b.AddUnlitMaterial("Flat", 1.0f, 0.0f, 0.0f);
        const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
        const int normal = b.AddFloats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, 3);
        const int uv = b.AddFloats({0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}, 2);
        const int indices = b.AddIndicesU16({0, 1, 2, 0, 2, 3});
        const int mesh = b.AddMesh("Quad", {Builder::Primitive(position, indices, bumpyMaterial, normal, uv)});
        const int flatMesh = b.AddMesh("FlatQuad", {Builder::Primitive(position, indices, unlitMaterial, normal, uv)});
        b.SetScene({b.AddNode("Quad", {{"mesh", mesh}}), b.AddNode("FlatQuad", {{"mesh", flatMesh}})});
        return b;
    }
}

TEST(ModelNormalMapTest, ByDefaultMaterialsStayLitAndTheNormalMapAndTangentsComeIn)
{
    const auto model = Model::LoadFromMemory(BumpyQuad().ToGlb());
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetMaterials().size(), 2u);
    const Material &bumpy = *model->GetMaterials()[0];
    EXPECT_EQ(bumpy.GetShading(), ShadingModel::Lit) << "PBR is optional: off by default";
    ASSERT_NE(bumpy.GetNormalTexture(), nullptr);
    EXPECT_FALSE(bumpy.GetNormalTexture()->GetSettings().srgb) << "a normal map is data";
    EXPECT_FLOAT_EQ(bumpy.GetNormalScale(), 2.0f);
    ASSERT_NE(bumpy.GetMetallicRoughnessTexture(), nullptr);
    EXPECT_FALSE(bumpy.GetMetallicRoughnessTexture()->GetSettings().srgb);
    EXPECT_FLOAT_EQ(bumpy.GetMetallic(), 0.6f);
    EXPECT_NEAR(bumpy.GetSmoothness(), 0.7f, 1e-6f);
    EXPECT_EQ(model->GetMaterials()[1]->GetShading(), ShadingModel::Unlit);

    ASSERT_EQ(model->GetMeshes().size(), 2u);
    const auto &vertices = model->GetMeshes()[0]->GetVertices();
    ASSERT_EQ(vertices.size(), 4u);
    for (const Vertex &vertex : vertices)
    {
        EXPECT_NEAR(vertex.tangent[0], 1.0f, 1e-4f);
        EXPECT_NEAR(vertex.tangent[1], 0.0f, 1e-4f);
        EXPECT_EQ(vertex.tangent[3], 1.0f);
    }
    EXPECT_EQ(ExpectTangentsFollowTheUvs(*model->GetMeshes()[0], 0.999f, "imported quad"), 2u);
}

TEST(ModelNormalMapTest, PbrMaterialsAreAnImportSettingAndNeverMakeAnUnlitMaterialPbr)
{
    ModelSettings pbr;
    pbr.pbrMaterials = true;
    const auto model = Model::LoadFromMemory(BumpyQuad().ToGlb(), {}, pbr);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetMaterials().size(), 2u);
    EXPECT_EQ(model->GetMaterials()[0]->GetShading(), ShadingModel::Pbr);
    EXPECT_EQ(model->GetMaterials()[1]->GetShading(), ShadingModel::Unlit) << "KHR_materials_unlit stays unlit";
    EXPECT_TRUE(model->GetSettings().pbrMaterials);
}

TEST(ModelNormalMapTest, GenerateTangentsNeverLeavesTheVerticesWithoutOnes)
{
    ModelSettings never;
    never.generateTangents = ModelTangents::Never;
    const auto model = Model::LoadFromMemory(BumpyQuad().ToGlb(), {}, never);
    ASSERT_NE(model, nullptr);
    for (const Vertex &vertex : model->GetMeshes()[0]->GetVertices())
    {
        for (const float component : vertex.tangent)
        {
            EXPECT_EQ(component, 0.0f);
        }
    }
}

TEST(ModelNormalMapTest, TheTangentAndPbrSettingsAreParsedFromCustomData)
{
    const ModelSettings defaults = Model::ParseSettings(json::object());
    EXPECT_EQ(defaults.generateTangents, ModelTangents::IfMissing);
    EXPECT_FALSE(defaults.pbrMaterials);

    const ModelSettings parsed = Model::ParseSettings(
        json{{"model", {{"generateTangents", "always"}, {"pbrMaterials", true}}}});
    EXPECT_EQ(parsed.generateTangents, ModelTangents::Always);
    EXPECT_TRUE(parsed.pbrMaterials);
    EXPECT_EQ(Model::ParseSettings(json{{"model", {{"generateTangents", "never"}}}}).generateTangents,
              ModelTangents::Never);
    EXPECT_EQ(Model::ParseSettings(json{{"model", {{"generateTangents", "ifMissing"}}}}).generateTangents,
              ModelTangents::IfMissing);

    WarningCapture capture;
    const ModelSettings bad = Model::ParseSettings(
        json{{"model", {{"generateTangents", "sometimes"}, {"pbrMaterials", "yes"}}}});
    EXPECT_EQ(bad, ModelSettings{}) << "both ignored";
    EXPECT_EQ(capture.messages.size(), 2u);
}
