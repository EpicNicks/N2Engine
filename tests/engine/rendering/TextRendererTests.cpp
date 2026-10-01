#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SWMesh.hpp>
#include <renderer/software/SWShader.hpp>
#include <renderer/software/SWTexture.hpp>
#include <text/TextLayout.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/text/Font.hpp"

// TextRenderer through a renderer that keeps CPU-side resources (the software renderer's resource
// classes) and records every call. No window or GPU is involved.

using namespace N2Engine;
using Rendering::TextRenderer;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;
using Renderer::Common::TextureOptions;

namespace
{
    struct RecordedDraw
    {
        IMesh *mesh = nullptr;
        std::array<float, 16> model{};
        IMaterial *material = nullptr;
        RenderState state;
    };

    struct RecordedTexture
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 0;
        TextureOptions options;
    };

    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        // What the test can switch off
        bool supportsUpdateMesh = true;
        bool hasTextShader = true;

        // What was asked for
        std::vector<RecordedDraw> draws;
        std::vector<RecordedTexture> createdTextures;
        int createMeshCalls = 0;
        int updateMeshCalls = 0;
        int destroyMeshCalls = 0;
        int destroyTextureCalls = 0;
        int createMaterialCalls = 0;
        int destroyMaterialCalls = 0;

        // What is alive
        std::vector<std::unique_ptr<Renderer::Software::SWMesh>> meshes;
        std::vector<std::unique_ptr<Renderer::Software::SWTexture>> textures;
        std::vector<std::unique_ptr<Renderer::Software::SWMaterial>> materials;
        Renderer::Software::SWShader textShader{Renderer::Software::SWShaderType::Unlit};

        IMesh *CreateMesh(const MeshData &meshData) override
        {
            ++createMeshCalls;
            auto mesh = std::make_unique<Renderer::Software::SWMesh>();
            mesh->vertices = meshData.vertices;
            mesh->indices = meshData.indices;
            meshes.push_back(std::move(mesh));
            return meshes.back().get();
        }

        bool UpdateMesh(IMesh *mesh, const MeshData &meshData) override
        {
            if (!supportsUpdateMesh)
            {
                return false;
            }
            ++updateMeshCalls;
            auto *sw = Find(meshes, mesh);
            if (!sw || meshData.vertices.empty())
            {
                return false;
            }
            sw->vertices = meshData.vertices;
            sw->indices = meshData.indices;
            return true;
        }

        void DestroyMesh(IMesh *mesh) override
        {
            ++destroyMeshCalls;
            Erase(meshes, mesh);
        }

        ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                const uint32_t channels) override
        {
            return CreateTexture(data, width, height, channels, TextureOptions::Default());
        }

        ITexture *CreateTexture(const uint8_t *data, const uint32_t width, const uint32_t height,
                                const uint32_t channels, const TextureOptions &options) override
        {
            createdTextures.push_back(RecordedTexture{width, height, channels, options});
            auto texture = std::make_unique<Renderer::Software::SWTexture>();
            texture->width = width;
            texture->height = height;
            texture->channels = channels;
            texture->options = options;
            texture->data.assign(data, data + static_cast<size_t>(width) * height * channels);
            textures.push_back(std::move(texture));
            return textures.back().get();
        }

        void DestroyTexture(ITexture *texture) override
        {
            ++destroyTextureCalls;
            Erase(textures, texture);
        }

        IMaterial *CreateMaterial(IShader *shader) override { return CreateMaterial(shader, nullptr); }

        IMaterial *CreateMaterial(IShader *shader, ITexture *texture) override
        {
            ++createMaterialCalls;
            materials.push_back(std::make_unique<Renderer::Software::SWMaterial>(shader, texture));
            return materials.back().get();
        }

        void DestroyMaterial(IMaterial *material) override
        {
            ++destroyMaterialCalls;
            Erase(materials, material);
        }

        [[nodiscard]] IShader *GetStandardTextShader() const override
        {
            return hasTextShader ? const_cast<Renderer::Software::SWShader *>(&textShader) : nullptr;
        }

        using IRenderer::DrawMesh;
        void DrawMesh(IMesh *mesh, const float *modelMatrix, IMaterial *material, const RenderState &state) override
        {
            RecordedDraw draw;
            draw.mesh = mesh;
            std::copy_n(modelMatrix, 16, draw.model.begin());
            draw.material = material;
            draw.state = state;
            draws.push_back(draw);
        }

        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(IShader *) override {}
        bool DestroyShaderProgram(IShader *) override { return false; }
        bool IsValidShader(IShader *) const override { return false; }
        void SetViewProjection(const float *, const float *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] IShader *GetStandardUnlitShader() const override { return nullptr; }
        [[nodiscard]] IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "Recording"; }

        [[nodiscard]] Renderer::Software::SWMesh *MeshOf(const RecordedDraw &draw) const
        {
            return Find(meshes, draw.mesh);
        }

    private:
        template <typename Owned, typename Base>
        static Owned *Find(const std::vector<std::unique_ptr<Owned>> &owned, Base *object)
        {
            const auto it = std::ranges::find_if(owned, [object](const auto &p) { return p.get() == object; });
            return it == owned.end() ? nullptr : it->get();
        }

        template <typename Owned, typename Base>
        static void Erase(std::vector<std::unique_ptr<Owned>> &owned, Base *object)
        {
            std::erase_if(owned, [object](const auto &p) { return p.get() == object; });
        }
    };

    // A TextRenderer on its own object; the object is returned so the component lives as long as it
    struct TextObject
    {
        GameObject::Ptr gameObject;
        TextRenderer *text = nullptr;
    };

    TextObject MakeText(const std::string &name, const std::string &text)
    {
        TextObject result;
        result.gameObject = GameObject::Create(name);
        result.text = result.gameObject->AddComponent<TextRenderer>();
        result.text->SetText(text);
        return result;
    }

    std::shared_ptr<Text::Font> DefaultFont()
    {
        auto font = Text::Font::GetDefault();
        EXPECT_NE(font, nullptr) << "the embedded default font didn't build";
        return font;
    }

    // The z component of (b - a) x (c - a): positive when a, b, c wind counter-clockwise seen from +Z
    float Winding(const Renderer::Common::Vertex &a, const Renderer::Common::Vertex &b,
                  const Renderer::Common::Vertex &c)
    {
        const float abx = b.position[0] - a.position[0];
        const float aby = b.position[1] - a.position[1];
        const float acx = c.position[0] - a.position[0];
        const float acy = c.position[1] - a.position[1];
        return abx * acy - aby * acx;
    }
}

// ============================================================================
// Mesh generation
// ============================================================================

TEST(TextRendererMeshTest, OneQuadPerGlyphWithTheLayoutsPositionsAndUvs)
{
    const auto font = DefaultFont();
    ASSERT_NE(font, nullptr);

    Text::LayoutOptions options;
    options.fontSize = 0.5f;
    const Text::TextLayout layout = font->Layout("Hello, world", options);
    ASSERT_EQ(layout.quads.size(), 11u) << "every character but the space has a glyph";

    const MeshData mesh = TextRenderer::BuildMesh(layout);
    ASSERT_EQ(mesh.vertices.size(), layout.quads.size() * 4);
    ASSERT_EQ(mesh.indices.size(), layout.quads.size() * 6);

    for (size_t i = 0; i < layout.quads.size(); ++i)
    {
        const Text::GlyphQuad &quad = layout.quads[i];
        const Renderer::Common::Vertex *v = &mesh.vertices[i * 4];

        // Bottom-left, bottom-right, top-right, top-left; uvs are y-down, so the top corners get minY
        const std::array<std::array<float, 4>, 4> expected{{
            {quad.position.minX, quad.position.minY, quad.uv.minX, quad.uv.maxY},
            {quad.position.maxX, quad.position.minY, quad.uv.maxX, quad.uv.maxY},
            {quad.position.maxX, quad.position.maxY, quad.uv.maxX, quad.uv.minY},
            {quad.position.minX, quad.position.maxY, quad.uv.minX, quad.uv.minY},
        }};
        for (size_t corner = 0; corner < 4; ++corner)
        {
            EXPECT_FLOAT_EQ(v[corner].position[0], expected[corner][0]) << "quad " << i << " corner " << corner;
            EXPECT_FLOAT_EQ(v[corner].position[1], expected[corner][1]) << "quad " << i << " corner " << corner;
            EXPECT_FLOAT_EQ(v[corner].position[2], 0.0f);
            EXPECT_FLOAT_EQ(v[corner].texCoord[0], expected[corner][2]) << "quad " << i << " corner " << corner;
            EXPECT_FLOAT_EQ(v[corner].texCoord[1], expected[corner][3]) << "quad " << i << " corner " << corner;
            EXPECT_FLOAT_EQ(v[corner].normal[2], 1.0f);
            EXPECT_FLOAT_EQ(v[corner].color[3], 1.0f);
        }

        const auto base = static_cast<uint32_t>(i * 4);
        const std::vector<uint32_t> indices(mesh.indices.begin() + static_cast<std::ptrdiff_t>(i * 6),
                                            mesh.indices.begin() + static_cast<std::ptrdiff_t>(i * 6 + 6));
        EXPECT_EQ(indices, (std::vector<uint32_t>{base, base + 1, base + 2, base, base + 2, base + 3}));
    }
}

TEST(TextRendererMeshTest, UvsStayInsideTheAtlas)
{
    const auto font = DefaultFont();
    ASSERT_NE(font, nullptr);

    // Every printable ASCII character
    std::string ascii;
    for (char c = 0x21; c < 0x7F; ++c)
    {
        ascii += c;
    }
    const MeshData mesh = TextRenderer::BuildMesh(font->Layout(ascii));
    ASSERT_FALSE(mesh.vertices.empty());

    for (const Renderer::Common::Vertex &vertex : mesh.vertices)
    {
        EXPECT_GE(vertex.texCoord[0], 0.0f);
        EXPECT_LE(vertex.texCoord[0], 1.0f);
        EXPECT_GE(vertex.texCoord[1], 0.0f);
        EXPECT_LE(vertex.texCoord[1], 1.0f);
    }
}

TEST(TextRendererMeshTest, QuadsFaceTheCamera)
{
    const auto font = DefaultFont();
    ASSERT_NE(font, nullptr);

    // Both triangles of every quad wind counter-clockwise seen from +Z, so back-face culling keeps them
    // for the default camera (at +Z looking down -Z)
    const MeshData mesh = TextRenderer::BuildMesh(font->Layout("Ag"));
    ASSERT_EQ(mesh.indices.size() % 3, 0u);
    for (size_t i = 0; i < mesh.indices.size(); i += 3)
    {
        EXPECT_GT(Winding(mesh.vertices[mesh.indices[i]], mesh.vertices[mesh.indices[i + 1]],
                          mesh.vertices[mesh.indices[i + 2]]), 0.0f) << "triangle " << i / 3;
    }
}

TEST(TextRendererMeshTest, EmptyLayoutGivesAnEmptyMesh)
{
    const MeshData mesh = TextRenderer::BuildMesh(Text::TextLayout{});
    EXPECT_TRUE(mesh.vertices.empty());
    EXPECT_TRUE(mesh.indices.empty());
}

// ============================================================================
// Settings and layout
// ============================================================================

TEST(TextRendererTest, DefaultsAndTheDefaultFont)
{
    const auto object = MakeText("Defaults", "");
    const TextRenderer &text = *object.text;

    EXPECT_EQ(text.GetText(), "");
    EXPECT_EQ(text.GetFont(), nullptr);
    EXPECT_EQ(text.GetEffectiveFont(), Text::Font::GetDefault());
    EXPECT_NE(text.GetEffectiveFont(), nullptr);
    EXPECT_FLOAT_EQ(text.GetFontSize(), 1.0f);
    EXPECT_EQ(text.GetHorizontalAlign(), Text::HorizontalAlign::Left);
    EXPECT_EQ(text.GetVerticalAlign(), Text::VerticalAlign::Top);
    EXPECT_FLOAT_EQ(text.GetMaxWidth(), 0.0f);
    EXPECT_FLOAT_EQ(text.GetLineSpacing(), 1.0f);
    EXPECT_FLOAT_EQ(text.GetLetterSpacing(), 0.0f);
    EXPECT_NE(object.gameObject->GetPositionable(), nullptr) << "a TextRenderer needs a position to draw at";
}

TEST(TextRendererTest, LayoutIsTheFontsLayoutWithTheComponentsSettings)
{
    auto object = MakeText("Layout", "Two words\nand a line");
    TextRenderer &text = *object.text;
    text.SetFontSize(2.0f);
    text.SetMaxWidth(6.0f);
    text.SetHorizontalAlign(Text::HorizontalAlign::Center);
    text.SetVerticalAlign(Text::VerticalAlign::Middle);
    text.SetLineSpacing(1.5f);
    text.SetLetterSpacing(0.1f);

    const Text::LayoutOptions options = text.GetLayoutOptions();
    EXPECT_FLOAT_EQ(options.fontSize, 2.0f);
    EXPECT_FLOAT_EQ(options.maxWidth, 6.0f);
    EXPECT_EQ(options.horizontalAlign, Text::HorizontalAlign::Center);
    EXPECT_EQ(options.verticalAlign, Text::VerticalAlign::Middle);
    EXPECT_FLOAT_EQ(options.lineSpacing, 1.5f);
    EXPECT_FLOAT_EQ(options.letterSpacing, 0.1f);

    const Text::TextLayout expected = DefaultFont()->Layout("Two words\nand a line", options);
    const Text::TextLayout &actual = text.GetLayout();
    ASSERT_EQ(actual.quads.size(), expected.quads.size());
    for (size_t i = 0; i < expected.quads.size(); ++i)
    {
        EXPECT_EQ(actual.quads[i].position, expected.quads[i].position) << "quad " << i;
        EXPECT_EQ(actual.quads[i].uv, expected.quads[i].uv) << "quad " << i;
    }
    EXPECT_EQ(actual.bounds, expected.bounds);
    EXPECT_EQ(actual.lines.size(), expected.lines.size());
}

TEST(TextRendererTest, ASettingChangeLaysTheTextOutAgain)
{
    auto object = MakeText("Relayout", "Wide");
    TextRenderer &text = *object.text;

    const Text::Rect small = text.GetLayout().bounds;
    text.SetFontSize(3.0f);
    const Text::Rect large = text.GetLayout().bounds;

    EXPECT_NEAR(large.Width(), small.Width() * 3.0f, 1e-3f);
    EXPECT_NEAR(large.Height(), small.Height() * 3.0f, 1e-3f);
}

// ============================================================================
// Drawing
// ============================================================================

TEST(TextRendererTest, DrawsInTheTransparentQueueWithTheTransparentState)
{
    RecordingRenderer renderer;
    {
        auto object = MakeText("Queue", "Glass");
        EXPECT_EQ(object.text->GetRenderQueue(), (RenderQueueKey{RenderQueue::Transparent, 0}));

        const auto scene = Scene::Create("TextRenderer_Queue");
        scene->AddRootGameObject(object.gameObject);
        scene->Render(&renderer, Camera{});

        ASSERT_EQ(renderer.draws.size(), 1u);
        EXPECT_EQ(renderer.draws[0].state, RenderState::Transparent());
        EXPECT_FALSE(renderer.draws[0].state.depthWrite);
        EXPECT_TRUE(renderer.draws[0].state.blend);

        // Render (outside a scene) uses the same state
        object.text->Render(&renderer);
        ASSERT_EQ(renderer.draws.size(), 2u);
        EXPECT_EQ(renderer.draws[1].state, RenderState::Transparent());

        object.text->CleanupRenderResources(&renderer);
    }
}

TEST(TextRendererTest, DrawsTheLayoutMeshWithTheAtlasAndColourAtTheObjectsTransform)
{
    RecordingRenderer renderer;
    auto object = MakeText("Draw", "Hi!");
    TextRenderer &text = *object.text;
    text.SetColor(Common::Color::Cyan);
    object.gameObject->GetPositionable()->SetPosition(Math::Vector3(1.0f, 2.0f, 3.0f));

    text.Render(&renderer);
    ASSERT_EQ(renderer.draws.size(), 1u);
    const RecordedDraw &draw = renderer.draws[0];

    // The mesh is the layout's
    const Renderer::Software::SWMesh *mesh = renderer.MeshOf(draw);
    ASSERT_NE(mesh, nullptr);
    const MeshData expected = TextRenderer::BuildMesh(text.GetLayout());
    EXPECT_EQ(mesh->vertices.size(), expected.vertices.size());
    EXPECT_EQ(mesh->indices, expected.indices);

    // The material uses the text shader and the font's atlas
    const auto *material = dynamic_cast<const Renderer::Software::SWMaterial *>(draw.material);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->GetShader(), renderer.GetStandardTextShader());
    const auto *atlas = dynamic_cast<const Renderer::Software::SWTexture *>(material->GetTexture());
    ASSERT_NE(atlas, nullptr);
    const Text::FontAtlas &fontAtlas = DefaultFont()->GetSdfFont().GetAtlas();
    EXPECT_EQ(atlas->width, static_cast<uint32_t>(fontAtlas.GetWidth()));
    EXPECT_EQ(atlas->height, static_cast<uint32_t>(fontAtlas.GetHeight()));
    EXPECT_EQ(atlas->channels, 1u);
    EXPECT_EQ(atlas->data, fontAtlas.GetPixels());

    // The colour is the material's uAlbedo
    const Common::Color &color = text.GetColor();
    const std::array<float, 4> albedo = material->GetVec4("uAlbedo", {0, 0, 0, 0});
    EXPECT_FLOAT_EQ(albedo[0], color.r);
    EXPECT_FLOAT_EQ(albedo[1], color.g);
    EXPECT_FLOAT_EQ(albedo[2], color.b);
    EXPECT_FLOAT_EQ(albedo[3], color.a);

    // The model matrix is the object's world matrix (row-major: translation in the last column)
    const auto world = object.gameObject->GetPositionable()->GetLocalToWorldMatrix();
    for (size_t i = 0; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(draw.model[i], world.Data()[i]) << "element " << i;
    }
    EXPECT_FLOAT_EQ(draw.model[3], 1.0f);
    EXPECT_FLOAT_EQ(draw.model[7], 2.0f);
    EXPECT_FLOAT_EQ(draw.model[11], 3.0f);

    text.CleanupRenderResources(&renderer);
}

TEST(TextRendererTest, TheAtlasIsAnSdfTextureUploadedOncePerFont)
{
    RecordingRenderer renderer;
    auto first = MakeText("AtlasA", "One");
    auto second = MakeText("AtlasB", "Two");

    first.text->Render(&renderer);
    second.text->Render(&renderer);
    first.text->Render(&renderer);

    ASSERT_EQ(renderer.createdTextures.size(), 1u) << "both texts use the default font: one texture";
    const RecordedTexture &texture = renderer.createdTextures[0];
    EXPECT_EQ(texture.channels, 1u);
    EXPECT_EQ(texture.options, TextureOptions::SdfAtlas());
    EXPECT_EQ(texture.options.wrap, Renderer::Common::TextureWrap::ClampToEdge);
    EXPECT_EQ(texture.options.filter, Renderer::Common::TextureFilter::Linear);
    EXPECT_FALSE(texture.options.mipmaps);

    // Each text has its own material (its own colour) on the shared texture
    EXPECT_EQ(renderer.createMaterialCalls, 2);
    EXPECT_EQ(renderer.draws[0].material->GetTexture(), renderer.draws[1].material->GetTexture());
    EXPECT_NE(renderer.draws[0].material, renderer.draws[1].material);

    // The texture goes when its last user releases it
    first.text->CleanupRenderResources(&renderer);
    EXPECT_EQ(renderer.destroyTextureCalls, 0);
    second.text->CleanupRenderResources(&renderer);
    EXPECT_EQ(renderer.destroyTextureCalls, 1);
    EXPECT_TRUE(renderer.textures.empty());
    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());

    // And comes back when text draws again
    first.text->Render(&renderer);
    EXPECT_EQ(renderer.createdTextures.size(), 2u);
    first.text->CleanupRenderResources(&renderer);
}

TEST(TextRendererTest, NothingIsRebuiltWhenNothingChanged)
{
    RecordingRenderer renderer;
    auto object = MakeText("Dirty", "Score: 0");
    TextRenderer &text = *object.text;

    for (int frame = 0; frame < 5; ++frame)
    {
        text.Render(&renderer);
    }
    EXPECT_EQ(renderer.draws.size(), 5u);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 0);
    EXPECT_EQ(renderer.createdTextures.size(), 1u);
    EXPECT_EQ(renderer.createMaterialCalls, 1);

    // Setting the same values again isn't a change
    text.SetText("Score: 0");
    text.SetFontSize(text.GetFontSize());
    text.SetHorizontalAlign(text.GetHorizontalAlign());
    text.Render(&renderer);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 0);

    // The colour is a uniform: no rebuild
    text.SetColor(Common::Color::Red);
    text.Render(&renderer);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 0);

    text.CleanupRenderResources(&renderer);
}

TEST(TextRendererTest, AChangedSettingUpdatesTheMeshInPlace)
{
    RecordingRenderer renderer;
    auto object = MakeText("Update", "Score: 0");
    TextRenderer &text = *object.text;

    text.Render(&renderer);
    IMesh *const mesh = renderer.draws.back().mesh;
    const size_t vertexCount = renderer.MeshOf(renderer.draws.back())->vertices.size();

    text.SetText("Score: 100");
    text.Render(&renderer);
    EXPECT_EQ(renderer.createMeshCalls, 1);
    EXPECT_EQ(renderer.updateMeshCalls, 1);
    EXPECT_EQ(renderer.destroyMeshCalls, 0);
    EXPECT_EQ(renderer.draws.back().mesh, mesh) << "the same mesh, updated";
    EXPECT_EQ(renderer.MeshOf(renderer.draws.back())->vertices.size(), vertexCount + 2 * 4);

    // Every layout input counts as a change
    int expectedUpdates = 1;
    const auto expectUpdate = [&](const char *what)
    {
        text.Render(&renderer);
        ++expectedUpdates;
        EXPECT_EQ(renderer.updateMeshCalls, expectedUpdates) << what;
    };
    text.SetFontSize(2.0f);
    expectUpdate("font size");
    text.SetMaxWidth(1.0f);
    expectUpdate("max width");
    text.SetHorizontalAlign(Text::HorizontalAlign::Right);
    expectUpdate("horizontal alignment");
    text.SetVerticalAlign(Text::VerticalAlign::Bottom);
    expectUpdate("vertical alignment");
    text.SetLineSpacing(2.0f);
    expectUpdate("line spacing");
    text.SetLetterSpacing(0.2f);
    expectUpdate("letter spacing");

    EXPECT_EQ(renderer.createMeshCalls, 1);
    text.CleanupRenderResources(&renderer);
}

TEST(TextRendererTest, ABackendWithoutUpdateMeshGetsANewMesh)
{
    RecordingRenderer renderer;
    renderer.supportsUpdateMesh = false;
    auto object = MakeText("NoUpdate", "A");
    TextRenderer &text = *object.text;

    text.Render(&renderer);
    text.SetText("AB");
    text.Render(&renderer);

    EXPECT_EQ(renderer.createMeshCalls, 2);
    EXPECT_EQ(renderer.destroyMeshCalls, 1);
    EXPECT_EQ(renderer.meshes.size(), 1u) << "the old mesh was destroyed, not leaked";
    EXPECT_EQ(renderer.MeshOf(renderer.draws.back())->vertices.size(), 2u * 4u);
    text.CleanupRenderResources(&renderer);
}

TEST(TextRendererTest, EmptyTextDrawsNothing)
{
    RecordingRenderer renderer;
    auto object = MakeText("Empty", "");
    object.text->Render(&renderer);
    object.text->SetText("   ");
    object.text->Render(&renderer);

    EXPECT_TRUE(renderer.draws.empty());
    EXPECT_EQ(renderer.createMeshCalls, 0);

    // Text appearing later draws
    object.text->SetText("Now");
    object.text->Render(&renderer);
    EXPECT_EQ(renderer.draws.size(), 1u);

    // Text emptied again stops drawing and keeps its mesh for later
    object.text->SetText("");
    object.text->Render(&renderer);
    EXPECT_EQ(renderer.draws.size(), 1u);
    EXPECT_EQ(renderer.destroyMeshCalls, 0);
    object.text->CleanupRenderResources(&renderer);
}

TEST(TextRendererTest, NoTextShaderMeansNoDrawsAndNoResources)
{
    RecordingRenderer renderer;
    renderer.hasTextShader = false;
    auto object = MakeText("NoShader", "Hidden");

    object.text->Render(&renderer);

    EXPECT_TRUE(renderer.draws.empty());
    EXPECT_TRUE(renderer.createdTextures.empty());
    EXPECT_EQ(renderer.createMeshCalls, 0);
    EXPECT_EQ(renderer.createMaterialCalls, 0);
}

TEST(TextRendererTest, NullRendererIsANoOp)
{
    auto object = MakeText("Null", "Text");
    EXPECT_NO_THROW(object.text->Render(nullptr));
    EXPECT_NO_THROW(object.text->InitializeRenderResources(nullptr));
    EXPECT_NO_THROW(object.text->CleanupRenderResources(nullptr));
}

TEST(TextRendererTest, ANewRendererReleasesWhatTheOldOneHeld)
{
    RecordingRenderer first;
    RecordingRenderer second;
    auto object = MakeText("Switch", "Text");

    object.text->Render(&first);
    object.text->Render(&second);

    EXPECT_TRUE(first.meshes.empty());
    EXPECT_TRUE(first.materials.empty());
    EXPECT_TRUE(first.textures.empty());
    EXPECT_EQ(second.draws.size(), 1u);
    EXPECT_EQ(second.createdTextures.size(), 1u);

    // Cleanup on a renderer it holds nothing on does nothing
    object.text->CleanupRenderResources(&first);
    EXPECT_EQ(second.destroyMeshCalls, 0);

    object.text->CleanupRenderResources(&second);
    EXPECT_TRUE(second.meshes.empty());
    EXPECT_TRUE(second.textures.empty());
}

TEST(TextRendererTest, DestroyingTheObjectInASceneReleasesItsResources)
{
    RecordingRenderer renderer;
    const auto scene = Scene::Create("TextRenderer_Destroy");
    auto object = MakeText("Destroyed", "Bye");
    scene->AddRootGameObject(object.gameObject);
    scene->ProcessAttachQueue(); // as a loaded scene does; only attached components get OnDestroy
    scene->Render(&renderer, Camera{});
    ASSERT_EQ(renderer.meshes.size(), 1u);

    scene->Clear();

    EXPECT_TRUE(renderer.meshes.empty());
    EXPECT_TRUE(renderer.materials.empty());
    EXPECT_TRUE(renderer.textures.empty());
}

// ============================================================================
// Serialization and scripting
// ============================================================================

TEST(TextRendererTest, SettingsSurviveASceneLoad)
{
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("TextRenderer"));

    auto object = MakeText("Saved", "Saved text\nline two");
    TextRenderer &text = *object.text;
    text.SetFontSize(0.75f);
    text.SetColor(Common::Color::Magenta);
    text.SetHorizontalAlign(Text::HorizontalAlign::Center);
    text.SetVerticalAlign(Text::VerticalAlign::Baseline);
    text.SetMaxWidth(4.0f);
    text.SetLineSpacing(1.25f);
    text.SetLetterSpacing(-0.05f);

    const auto scene = Scene::Create("TextRenderer_RoundTrip");
    scene->AddRootGameObject(object.gameObject);
    const nlohmann::json saved = scene->Serialize();
    const auto loadedScene = Scene::FromJSON(saved);
    ASSERT_NE(loadedScene, nullptr);
    const auto loadedObject = loadedScene->FindGameObject("Saved");
    ASSERT_NE(loadedObject, nullptr);
    const auto *loaded = loadedObject->GetComponent<TextRenderer>();
    ASSERT_NE(loaded, nullptr) << "TextRenderer wasn't registered, so loading dropped it";

    EXPECT_EQ(loaded->GetText(), "Saved text\nline two");
    EXPECT_EQ(loaded->GetFont(), nullptr);
    EXPECT_FLOAT_EQ(loaded->GetFontSize(), 0.75f);
    EXPECT_FLOAT_EQ(loaded->GetColor().r, Common::Color::Magenta.r);
    EXPECT_FLOAT_EQ(loaded->GetColor().g, Common::Color::Magenta.g);
    EXPECT_FLOAT_EQ(loaded->GetColor().b, Common::Color::Magenta.b);
    EXPECT_FLOAT_EQ(loaded->GetColor().a, Common::Color::Magenta.a);
    EXPECT_EQ(loaded->GetHorizontalAlign(), Text::HorizontalAlign::Center);
    EXPECT_EQ(loaded->GetVerticalAlign(), Text::VerticalAlign::Baseline);
    EXPECT_FLOAT_EQ(loaded->GetMaxWidth(), 4.0f);
    EXPECT_FLOAT_EQ(loaded->GetLineSpacing(), 1.25f);
    EXPECT_FLOAT_EQ(loaded->GetLetterSpacing(), -0.05f);

    // Alignments are saved by name
    EXPECT_NE(saved.dump().find("\"Baseline\""), std::string::npos) << saved.dump();
}

TEST(TextRendererTest, LuaCanAddAndConfigureIt)
{
    using Scripting::LuaRuntime;
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    sol::state &lua = LuaRuntime::Instance().GetState();

    const auto run = [&lua](const std::string &code)
    {
        const sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error error = result;
            ADD_FAILURE() << "Lua error: " << error.what() << "\nin: " << code;
        }
    };

    run(R"(
        text_test_go = GameObject.Create("LuaText")
        text_test_renderer = text_test_go:AddComponent("TextRenderer")
        text_test_renderer:SetText("Hello")
        text_test_renderer:SetFontSize(2)
        text_test_renderer:SetMaxWidth(10)
        text_test_renderer:SetLineSpacing(1.5)
        text_test_renderer:SetLetterSpacing(0.25)
        text_test_renderer:SetAlignment("Center", "Middle")
        text_test_h, text_test_v = text_test_renderer:GetAlignment()
        text_test_minX, text_test_minY, text_test_maxX, text_test_maxY = text_test_renderer:GetBounds()
        text_test_renderer:SetFont(nil)
        text_test_same = text_test_go:GetComponent("TextRenderer") == text_test_renderer
    )");

    run(R"(assert(text_test_renderer:GetText() == "Hello"))");
    EXPECT_EQ(lua["text_test_h"].get<std::string>(), "Center");
    EXPECT_EQ(lua["text_test_v"].get<std::string>(), "Middle");
    EXPECT_TRUE(lua["text_test_same"].get<bool>());

    // Centred in both directions: the bounds straddle the origin
    const auto minX = lua["text_test_minX"].get<float>();
    const auto maxX = lua["text_test_maxX"].get<float>();
    const auto minY = lua["text_test_minY"].get<float>();
    const auto maxY = lua["text_test_maxY"].get<float>();
    EXPECT_LT(minX, 0.0f);
    EXPECT_GT(maxX, 0.0f);
    EXPECT_NEAR(minX, -maxX, 1e-3f);
    EXPECT_NEAR(minY, -maxY, 1e-3f);

    // An unknown alignment is an error and changes nothing
    const sol::protected_function_result bad =
        lua.safe_script(R"(text_test_renderer:SetAlignment("Right", "Sideways"))", sol::script_pass_on_error);
    EXPECT_FALSE(bad.valid());
    run(R"(text_test_h2, text_test_v2 = text_test_renderer:GetAlignment())");
    EXPECT_EQ(lua["text_test_h2"].get<std::string>(), "Center");
    EXPECT_EQ(lua["text_test_v2"].get<std::string>(), "Middle");

    // A font that doesn't load is an error
    const sol::protected_function_result missing =
        lua.safe_script(R"(text_test_renderer:SetFont("res://fonts/NoSuchFont.ttf"))", sol::script_pass_on_error);
    EXPECT_FALSE(missing.valid());

    run(R"(
        assert(text_test_renderer:GetFontSize() == 2)
        assert(text_test_renderer:GetMaxWidth() == 10)
        assert(text_test_renderer:GetLineSpacing() == 1.5)
        assert(text_test_renderer:GetLetterSpacing() == 0.25)
        text_test_go = nil
        text_test_renderer = nil
    )");
}
