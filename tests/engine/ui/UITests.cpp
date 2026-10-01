#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/IShader.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/Camera.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/common/Color.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

// Screen-space UI without a window or GPU: RectTransform layout, the UI pass's draw order and render state
// (through a renderer that only records), the UI hit test, and the UI hit provider in a PointerDispatcher

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;
using Renderer::Common::CullMode;
using Renderer::Common::RenderState;

namespace
{
    constexpr float Tolerance = 1e-4f;
    const Vector2i Viewport{800, 600};
    const Rect Screen{0.0f, 0.0f, 800.0f, 600.0f};

    void ExpectRect(const Rect &actual, const Rect &expected, const char *what = "")
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.width, expected.width, Tolerance) << what << " width";
        EXPECT_NEAR(actual.height, expected.height, Tolerance) << what << " height";
    }

    // ===== A renderer that records the UI pass =====

    class FakeShader final : public Renderer::Common::IShader
    {
    public:
        bool LoadFromStrings(const std::string &, const std::string &) override { return true; }
        void Bind() const override {}
        void Unbind() const override {}
        bool IsValid() const override { return true; }
        void SetFloat(const std::string &, float) override {}
        void SetInt(const std::string &, int) override {}
        void SetBool(const std::string &, bool) override {}
        void SetVec2(const std::string &, const Math::Vector2 &) override {}
        void SetVec3(const std::string &, const Math::Vector3 &) override {}
        void SetVec4(const std::string &, const Math::Vector4 &) override {}
        void SetMat4(const std::string &, const Math::Matrix<float, 4, 4> &) override {}
        void SetVec2(const std::string &, float, float) override {}
        void SetVec3(const std::string &, float, float, float) override {}
        void SetVec4(const std::string &, float, float, float, float) override {}
    };

    class FakeMesh final : public Renderer::Common::IMesh
    {
    public:
        explicit FakeMesh(const Renderer::Common::MeshData &meshData) : data(meshData) {}
        bool IsValid() const override { return true; }
        uint32_t GetIndexCount() const override { return static_cast<uint32_t>(data.indices.size()); }
        uint32_t GetVertexCount() const override { return static_cast<uint32_t>(data.vertices.size()); }
        Renderer::Common::MeshData data;
    };

    class FakeMaterial final : public Renderer::Common::IMaterial
    {
    public:
        explicit FakeMaterial(Renderer::Common::IShader *materialShader) : shader(materialShader) {}
        void SetInt(const std::string &, int) override {}
        void SetFloat(const std::string &, float) override {}
        void SetVec2(const std::string &, float, float) override {}
        void SetVec2(const std::string &, Math::Vector2 &) override {}
        void SetVec3(const std::string &, float, float, float) override {}
        void SetVec3(const std::string &, Math::Vector3 &) override {}
        void SetVec4(const std::string &, float, float, float, float) override {}
        void SetVec4(const std::string &, Math::Vector4 &) override {}
        void SetColor(const std::string &name, const float r, const float g, const float b, const float a) override
        {
            if (name == "uAlbedo")
            {
                color[0] = r;
                color[1] = g;
                color[2] = b;
                color[3] = a;
            }
        }
        void SetTexture(Renderer::Common::ITexture *t) override { texture = t; }
        [[nodiscard]] Renderer::Common::IShader *GetShader() const override { return shader; }
        [[nodiscard]] Renderer::Common::ITexture *GetTexture() const override { return texture; }
        [[nodiscard]] bool IsValid() const override { return true; }

        Renderer::Common::IShader *shader;
        Renderer::Common::ITexture *texture = nullptr;
        float color[4]{};
    };

    struct UIDraw
    {
        Rect rect;      // read back from the model matrix
        float red;      // the material's colour when drawn, to tell draws apart
        RenderState state;
        const FakeMesh *mesh;
    };

    class RecordingUIRenderer final : public Renderer::Common::IRenderer
    {
    public:
        std::vector<UIDraw> draws;
        int viewProjectionCalls = 0;
        float view[16]{};
        float projection[16]{};
        std::vector<std::unique_ptr<FakeMesh>> meshes;
        std::vector<std::unique_ptr<FakeMaterial>> materials;
        int destroyedMeshes = 0;
        int destroyedMaterials = 0;

        using IRenderer::DrawMesh;
        void DrawMesh(Renderer::Common::IMesh *mesh, const float *model, Renderer::Common::IMaterial *material,
                      const RenderState &state) override
        {
            // Row-major: the scale on the diagonal, the translation in the last column
            const Rect rect{model[3], model[7], model[0], model[5]};
            draws.push_back(UIDraw{rect, static_cast<FakeMaterial *>(material)->color[0], state,
                                   static_cast<const FakeMesh *>(mesh)});
        }
        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        void SetViewProjection(const float *v, const float *p) override
        {
            ++viewProjectionCalls;
            std::copy_n(v, 16, view);
            std::copy_n(p, 16, projection);
        }

        Renderer::Common::IMesh *CreateMesh(const Renderer::Common::MeshData &data) override
        {
            meshes.push_back(std::make_unique<FakeMesh>(data));
            return meshes.back().get();
        }
        void DestroyMesh(Renderer::Common::IMesh *) override { ++destroyedMeshes; }
        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *shader) override
        {
            return CreateMaterial(shader, nullptr);
        }
        Renderer::Common::IMaterial *CreateMaterial(Renderer::Common::IShader *shader,
                                                    Renderer::Common::ITexture *texture) override
        {
            materials.push_back(std::make_unique<FakeMaterial>(shader));
            materials.back()->texture = texture;
            return materials.back().get();
        }
        void DestroyMaterial(Renderer::Common::IMaterial *) override { ++destroyedMaterials; }
        [[nodiscard]] Renderer::Common::IShader *GetStandardUnlitShader() const override
        {
            return const_cast<FakeShader *>(&_shader);
        }

        bool Initialize(GLFWwindow *, uint32_t, uint32_t) override { return true; }
        void Shutdown() override {}
        void Resize(uint32_t, uint32_t) override {}
        void Clear(float, float, float, float) override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        void Present() override {}
        Renderer::Common::IShader *CreateShaderProgram(const char *, const char *) override { return nullptr; }
        void UseShaderProgram(Renderer::Common::IShader *) override {}
        bool DestroyShaderProgram(Renderer::Common::IShader *) override { return false; }
        bool IsValidShader(Renderer::Common::IShader *) const override { return true; }
        Renderer::Common::ITexture *CreateTexture(const uint8_t *, uint32_t, uint32_t, uint32_t) override
        {
            return nullptr;
        }
        void DestroyTexture(Renderer::Common::ITexture *) override {}
        void UpdateSceneLighting(const Renderer::Common::SceneLightingData &, const Math::Vector3 &) override {}
        void OnResize(int, int) override {}
        [[nodiscard]] Renderer::Common::IShader *GetStandardLitShader() const override { return nullptr; }
        void ReadFramebuffer(std::uint8_t *, int, int) const override {}
        void SetWireframe(bool) override {}
        [[nodiscard]] const char *GetRendererName() const override { return "RecordingUI"; }

        std::vector<float> Reds() const
        {
            std::vector<float> reds;
            for (const UIDraw &draw : draws)
            {
                reds.push_back(draw.red);
            }
            return reds;
        }

    private:
        FakeShader _shader;
    };

    // ===== Scene helpers =====

    GameObject::Ptr AddCanvas(Scene &scene, const std::string &name, const int sortOrder = 0)
    {
        auto canvas = UISystem::CreateCanvas(name);
        canvas->GetComponent<Canvas>()->SetSortOrder(sortOrder);
        scene.AddRootGameObject(canvas);
        return canvas;
    }

    /// A child element with a RectTransform at a fixed rect inside its parent (anchors at the parent's
    /// bottom-left, pivot at the element's bottom-left), and an Image whose red channel tags its draws
    GameObject::Ptr AddPanel(const GameObject::Ptr &parent, const std::string &name, const Rect &local,
                             const float red = 1.0f)
    {
        auto element = UISystem::CreateElement(name);
        auto *rectTransform = element->GetComponent<RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Vector2{local.x, local.y});
        rectTransform->SetSizeDelta(Vector2{local.width, local.height});
        auto *image = element->AddComponent<Image>();
        image->SetColor(Common::Color{red, 1.0f, 1.0f, 1.0f});
        parent->AddChild(element, false);
        return element;
    }
}

// ============================================================================
// RectTransform layout
// ============================================================================

TEST(RectTransformLayoutTest, DefaultsAreUnitysCentredHundredPixelBox)
{
    auto element = GameObject::Create("Default");
    const auto *rectTransform = element->AddComponent<RectTransform>();
    ExpectRect(rectTransform->ResolveIn(Screen), Rect{350.0f, 250.0f, 100.0f, 100.0f});
}

TEST(RectTransformLayoutTest, PointAnchorWithPivotAndOffset)
{
    // Pinned to the top-right corner, 10 px in from each edge, growing down and left from its pivot
    const Rect rect = RectTransform::Resolve(Vector2{1.0f, 1.0f}, Vector2{1.0f, 1.0f}, Vector2{1.0f, 1.0f},
                                             Vector2{-10.0f, -10.0f}, Vector2{200.0f, 50.0f}, Screen);
    ExpectRect(rect, Rect{590.0f, 540.0f, 200.0f, 50.0f});

    // The same anchor with the pivot at the element's centre: anchoredPosition places the centre
    const Rect centred = RectTransform::Resolve(Vector2{1.0f, 1.0f}, Vector2{1.0f, 1.0f}, Vector2{0.5f, 0.5f},
                                                Vector2{-100.0f, -25.0f}, Vector2{200.0f, 50.0f}, Screen);
    ExpectRect(centred, Rect{600.0f, 550.0f, 200.0f, 50.0f});
}

TEST(RectTransformLayoutTest, StretchedAnchorsWithOffsets)
{
    auto element = GameObject::Create("Stretched");
    auto *rectTransform = element->AddComponent<RectTransform>();
    rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
    rectTransform->SetAnchorMax(Vector2{1.0f, 1.0f});
    rectTransform->SetOffsetMin(Vector2{10.0f, 20.0f});
    rectTransform->SetOffsetMax(Vector2{-30.0f, -40.0f});

    ExpectRect(rectTransform->ResolveIn(Screen), Rect{10.0f, 20.0f, 760.0f, 540.0f});
    EXPECT_NEAR(rectTransform->GetOffsetMin().x, 10.0f, Tolerance);
    EXPECT_NEAR(rectTransform->GetOffsetMin().y, 20.0f, Tolerance);
    EXPECT_NEAR(rectTransform->GetOffsetMax().x, -30.0f, Tolerance);
    EXPECT_NEAR(rectTransform->GetOffsetMax().y, -40.0f, Tolerance);
    // Unity's representation of the same thing
    EXPECT_NEAR(rectTransform->GetSizeDelta().x, -40.0f, Tolerance);
    EXPECT_NEAR(rectTransform->GetSizeDelta().y, -60.0f, Tolerance);
}

TEST(RectTransformLayoutTest, SettingOneCornerKeepsTheOther)
{
    auto element = GameObject::Create("Corners");
    auto *rectTransform = element->AddComponent<RectTransform>();
    rectTransform->SetPivot(Vector2{0.25f, 0.75f});
    const Rect before = rectTransform->ResolveIn(Screen);

    rectTransform->SetOffsetMin(rectTransform->GetOffsetMin() + Vector2{-20.0f, 5.0f});
    const Rect afterMin = rectTransform->ResolveIn(Screen);
    EXPECT_NEAR(afterMin.XMin(), before.XMin() - 20.0f, Tolerance);
    EXPECT_NEAR(afterMin.YMin(), before.YMin() + 5.0f, Tolerance);
    EXPECT_NEAR(afterMin.XMax(), before.XMax(), Tolerance) << "the top-right corner stays";
    EXPECT_NEAR(afterMin.YMax(), before.YMax(), Tolerance);

    rectTransform->SetOffsetMax(rectTransform->GetOffsetMax() + Vector2{8.0f, -4.0f});
    const Rect afterMax = rectTransform->ResolveIn(Screen);
    EXPECT_NEAR(afterMax.XMin(), afterMin.XMin(), Tolerance) << "the bottom-left corner stays";
    EXPECT_NEAR(afterMax.YMin(), afterMin.YMin(), Tolerance);
    EXPECT_NEAR(afterMax.XMax(), afterMin.XMax() + 8.0f, Tolerance);
    EXPECT_NEAR(afterMax.YMax(), afterMin.YMax() - 4.0f, Tolerance);
}

TEST(RectTransformLayoutTest, ResizeMovesAnchoredElementsAndStretchesStretchedOnes)
{
    const auto scene = Scene::Create("UILayout_Resize");
    const auto canvas = AddCanvas(*scene, "Canvas");

    const auto stretched = UISystem::CreateElement("Stretched");
    stretched->GetComponent<RectTransform>()->StretchToParent();
    canvas->AddChild(stretched, false);

    const auto corner = UISystem::CreateElement("TopRight");
    auto *cornerTransform = corner->GetComponent<RectTransform>();
    cornerTransform->SetAnchorMin(Vector2{1.0f, 1.0f});
    cornerTransform->SetAnchorMax(Vector2{1.0f, 1.0f});
    cornerTransform->SetPivot(Vector2{1.0f, 1.0f});
    cornerTransform->SetSizeDelta(Vector2{64.0f, 32.0f});
    canvas->AddChild(corner, false);

    static_cast<void>(UISystem::CollectGraphics(*scene, Vector2i{800, 600}));
    ExpectRect(stretched->GetComponent<RectTransform>()->GetRect(), Rect{0.0f, 0.0f, 800.0f, 600.0f}, "800x600");
    ExpectRect(cornerTransform->GetRect(), Rect{736.0f, 568.0f, 64.0f, 32.0f}, "800x600");

    static_cast<void>(UISystem::CollectGraphics(*scene, Vector2i{1280, 720}));
    ExpectRect(stretched->GetComponent<RectTransform>()->GetRect(), Rect{0.0f, 0.0f, 1280.0f, 720.0f}, "1280x720");
    ExpectRect(cornerTransform->GetRect(), Rect{1216.0f, 688.0f, 64.0f, 32.0f}, "1280x720: same size, same corner");
}

TEST(RectTransformLayoutTest, ChildrenResolveInTheirParentsRect)
{
    const auto scene = Scene::Create("UILayout_Nested");
    const auto canvas = AddCanvas(*scene, "Canvas");
    const auto panel = AddPanel(canvas, "Panel", Rect{100.0f, 50.0f, 400.0f, 300.0f});

    // Fills the right half of the panel
    const auto half = UISystem::CreateElement("RightHalf");
    auto *halfTransform = half->GetComponent<RectTransform>();
    halfTransform->SetAnchorMin(Vector2{0.5f, 0.0f});
    halfTransform->SetAnchorMax(Vector2{1.0f, 1.0f});
    halfTransform->SetSizeDelta(Vector2{0.0f, 0.0f});
    panel->AddChild(half, false);

    // No RectTransform: fills its parent
    const auto plain = GameObject::Create("Plain");
    half->AddChild(plain, false);
    const auto grandchild = UISystem::CreateElement("Grandchild");
    grandchild->GetComponent<RectTransform>()->StretchToParent();
    plain->AddChild(grandchild, false);

    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    ExpectRect(halfTransform->GetRect(), Rect{300.0f, 50.0f, 200.0f, 300.0f}, "half");
    ExpectRect(grandchild->GetComponent<RectTransform>()->GetRect(), Rect{300.0f, 50.0f, 200.0f, 300.0f},
               "through an object without a RectTransform");
}

TEST(RectTransformLayoutTest, TheCanvasCoversTheViewportWhateverItsRectTransformSays)
{
    const auto scene = Scene::Create("UILayout_CanvasRect");
    const auto canvas = AddCanvas(*scene, "Canvas");
    auto *canvasTransform = canvas->AddComponent<RectTransform>();
    canvasTransform->SetSizeDelta(Vector2{5.0f, 5.0f});

    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    ExpectRect(canvasTransform->GetRect(), Screen);
}

TEST(RectTransformLayoutTest, RectContainsIncludesEdgesAndNeedsArea)
{
    constexpr Rect rect{10.0f, 20.0f, 30.0f, 40.0f};
    EXPECT_TRUE(rect.Contains(Vector2{10.0f, 20.0f}));
    EXPECT_TRUE(rect.Contains(Vector2{40.0f, 60.0f}));
    EXPECT_FALSE(rect.Contains(Vector2{40.1f, 30.0f}));
    EXPECT_FALSE(rect.Contains(Vector2{20.0f, 19.9f}));

    constexpr Rect flat{0.0f, 0.0f, 10.0f, 0.0f};
    EXPECT_FALSE(flat.HasArea());
    EXPECT_FALSE(flat.Contains(Vector2{5.0f, 0.0f}));
    constexpr Rect negative{0.0f, 0.0f, -10.0f, 10.0f};
    EXPECT_FALSE(negative.Contains(Vector2{-5.0f, 5.0f}));
}

TEST(RectTransformLayoutTest, SerializationRoundTrip)
{
    auto source = GameObject::Create("Source");
    auto *rectTransform = source->AddComponent<RectTransform>();
    rectTransform->SetAnchorMin(Vector2{0.1f, 0.2f});
    rectTransform->SetAnchorMax(Vector2{0.9f, 0.8f});
    rectTransform->SetPivot(Vector2{0.0f, 1.0f});
    rectTransform->SetAnchoredPosition(Vector2{12.0f, -7.0f});
    rectTransform->SetSizeDelta(Vector2{-4.0f, 30.0f});
    auto *canvas = source->AddComponent<Canvas>();
    canvas->SetSortOrder(7);
    auto *image = source->AddComponent<Image>();
    image->SetColor(Common::Color{0.25f, 0.5f, 0.75f, 0.5f});
    image->SetRaycastTarget(false);

    auto target = GameObject::Create("Target");
    auto *rectCopy = target->AddComponent<RectTransform>();
    rectCopy->Deserialize(rectTransform->Serialize());
    auto *canvasCopy = target->AddComponent<Canvas>();
    canvasCopy->Deserialize(canvas->Serialize());
    auto *imageCopy = target->AddComponent<Image>();
    imageCopy->Deserialize(image->Serialize());

    ExpectRect(rectCopy->ResolveIn(Screen), rectTransform->ResolveIn(Screen));
    EXPECT_FLOAT_EQ(rectCopy->GetPivot().y, 1.0f);
    EXPECT_EQ(canvasCopy->GetSortOrder(), 7);
    EXPECT_FLOAT_EQ(imageCopy->GetColor().r, 0.25f);
    EXPECT_FLOAT_EQ(imageCopy->GetColor().b, 0.75f);
    EXPECT_FLOAT_EQ(imageCopy->GetColor().a, 0.5f);
    EXPECT_FALSE(imageCopy->GetRaycastTarget());

    for (const char *typeName : {"Canvas", "RectTransform", "Image"})
    {
        EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered(typeName)) << typeName;
    }
}

TEST(RectTransformLayoutTest, HelpersPutObjectsOnTheUILayer)
{
    const auto canvas = UISystem::CreateCanvas();
    const auto element = UISystem::CreateElement();
    EXPECT_EQ(canvas->GetLayer(), Layers::UI);
    EXPECT_EQ(element->GetLayer(), Layers::UI);
    EXPECT_NE(canvas->GetComponent<Canvas>(), nullptr);
    EXPECT_NE(element->GetComponent<RectTransform>(), nullptr);
    EXPECT_EQ(element->AddComponent<RectTransform>(), element->GetComponent<RectTransform>()) << "one per object";
}

// ============================================================================
// The UI pass
// ============================================================================

TEST(UIPassTest, DrawsWithOverlayStateAndPixelProjection)
{
    RecordingUIRenderer renderer;
    const auto scene = Scene::Create("UIPass_State");
    const auto canvas = AddCanvas(*scene, "Canvas");
    AddPanel(canvas, "Panel", Rect{100.0f, 50.0f, 200.0f, 80.0f}, 0.5f);

    UISystem::Render(*scene, &renderer, Viewport);

    ASSERT_EQ(renderer.draws.size(), 1u);
    const UIDraw &draw = renderer.draws[0];
    ExpectRect(draw.rect, Rect{100.0f, 50.0f, 200.0f, 80.0f});
    EXPECT_FALSE(draw.state.depthTest);
    EXPECT_FALSE(draw.state.depthWrite);
    EXPECT_EQ(draw.state.cull, CullMode::None) << "culling is off in the UI pass";
    EXPECT_TRUE(draw.state.blend);
    EXPECT_EQ(draw.state, UISystem::OverlayState());

    // Identity view; the projection maps (0, 0) to (-1, -1) and (800, 600) to (1, 1), y up
    EXPECT_EQ(renderer.viewProjectionCalls, 1);
    const Math::Matrix<float, 4, 4> identity = Math::Matrix<float, 4, 4>::identity();
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(renderer.view[i], identity.Data()[i]) << i;
    }
    const float *p = renderer.projection;
    EXPECT_FLOAT_EQ(p[0] * 0.0f + p[3], -1.0f);
    EXPECT_FLOAT_EQ(p[0] * 800.0f + p[3], 1.0f);
    EXPECT_FLOAT_EQ(p[5] * 0.0f + p[7], -1.0f);
    EXPECT_FLOAT_EQ(p[5] * 600.0f + p[7], 1.0f);
    EXPECT_FLOAT_EQ(p[15], 1.0f);

    // The quad is the unit square, with its two triangles wound counter-clockwise
    ASSERT_NE(draw.mesh, nullptr);
    ASSERT_EQ(draw.mesh->data.vertices.size(), 4u);
    EXPECT_EQ(draw.mesh->data.indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
    const auto &v = draw.mesh->data.vertices;
    for (const auto &[a, b, c] : {std::array{0, 1, 2}, std::array{0, 2, 3}})
    {
        const float cross = (v[b].position[0] - v[a].position[0]) * (v[c].position[1] - v[a].position[1]) -
                            (v[b].position[1] - v[a].position[1]) * (v[c].position[0] - v[a].position[0]);
        EXPECT_GT(cross, 0.0f) << "counter-clockwise";
    }
}

TEST(UIPassTest, HierarchyOrderWithinACanvasAndSortOrderAcrossCanvases)
{
    RecordingUIRenderer renderer;
    const auto scene = Scene::Create("UIPass_Order");

    // Added first but sorted on top
    const auto top = AddCanvas(*scene, "Top", 10);
    AddPanel(top, "TopPanel", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.9f);

    const auto bottom = AddCanvas(*scene, "Bottom", -1);
    const auto parent = AddPanel(bottom, "Parent", Rect{0.0f, 0.0f, 100.0f, 100.0f}, 0.1f);
    AddPanel(parent, "Child", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.2f);
    AddPanel(bottom, "Sibling", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.3f);

    const auto middle = AddCanvas(*scene, "Middle", 0);
    AddPanel(middle, "MiddlePanel", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.5f);
    const auto tie = AddCanvas(*scene, "Tie", 0);
    AddPanel(tie, "TiePanel", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.6f);

    UISystem::Render(*scene, &renderer, Viewport);
    EXPECT_EQ(renderer.Reds(), (std::vector<float>{0.1f, 0.2f, 0.3f, 0.5f, 0.6f, 0.9f}))
        << "parent, child, later sibling; canvases by sortOrder, ties in hierarchy order";
}

TEST(UIPassTest, SkipsInactiveDisabledAndEmptyAndNestedCanvasesJoinTheirParent)
{
    RecordingUIRenderer renderer;
    const auto scene = Scene::Create("UIPass_Skips");
    const auto canvas = AddCanvas(*scene, "Canvas");

    const auto inactive = AddPanel(canvas, "Inactive", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.1f);
    AddPanel(inactive, "UnderInactive", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.15f);
    inactive->SetActive(false);

    const auto disabled = AddPanel(canvas, "DisabledImage", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.2f);
    disabled->GetComponent<Image>()->SetActive(false);
    AddPanel(disabled, "UnderDisabledImage", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.25f);

    AddPanel(canvas, "ZeroSize", Rect{0.0f, 0.0f, 0.0f, 10.0f}, 0.3f);

    // A Canvas under a canvas is an ordinary element of the outer one: its sortOrder doesn't lift it
    const auto nested = AddPanel(canvas, "Nested", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.4f);
    nested->AddComponent<Canvas>()->SetSortOrder(100);
    AddPanel(canvas, "AfterNested", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.5f);

    // Not under any canvas: never drawn
    const auto loose = UISystem::CreateElement("Loose");
    loose->AddComponent<Image>();
    scene->AddRootGameObject(loose);

    const auto disabledCanvas = AddCanvas(*scene, "DisabledCanvas", 50);
    AddPanel(disabledCanvas, "UnderDisabledCanvas", Rect{0.0f, 0.0f, 10.0f, 10.0f}, 0.6f);
    disabledCanvas->GetComponent<Canvas>()->SetActive(false);

    UISystem::Render(*scene, &renderer, Viewport);
    EXPECT_EQ(renderer.Reds(), (std::vector<float>{0.25f, 0.4f, 0.5f}));
}

TEST(UIPassTest, NothingToDrawLeavesTheViewProjectionAlone)
{
    RecordingUIRenderer renderer;
    const auto scene = Scene::Create("UIPass_Empty");
    AddCanvas(*scene, "EmptyCanvas");

    UISystem::Render(*scene, &renderer, Viewport);
    EXPECT_EQ(renderer.viewProjectionCalls, 0);
    EXPECT_TRUE(renderer.draws.empty());

    const auto canvas = AddCanvas(*scene, "Canvas");
    AddPanel(canvas, "Panel", Rect{0.0f, 0.0f, 10.0f, 10.0f});
    UISystem::Render(*scene, &renderer, Vector2i{0, 0});
    EXPECT_EQ(renderer.viewProjectionCalls, 0) << "an empty (minimised) viewport draws nothing";
}

TEST(UIPassTest, TheSceneRenderableQueueNeverDrawsUI)
{
    RecordingUIRenderer renderer;
    const auto scene = Scene::Create("UIPass_NotInScenePass");
    const auto canvas = AddCanvas(*scene, "Canvas");
    AddPanel(canvas, "Panel", Rect{0.0f, 0.0f, 10.0f, 10.0f});

    scene->Render(&renderer, Camera{});
    EXPECT_TRUE(renderer.draws.empty()) << "Image isn't an IRenderable";
}

TEST(UIPassTest, ImageCreatesItsQuadOnceAndReleasesItOnDestroy)
{
    RecordingUIRenderer renderer;
    const auto scene = Scene::Create("UIPass_Resources");
    const auto canvas = AddCanvas(*scene, "Canvas");
    const auto panel = AddPanel(canvas, "Panel", Rect{0.0f, 0.0f, 10.0f, 10.0f});

    UISystem::Render(*scene, &renderer, Viewport);
    UISystem::Render(*scene, &renderer, Viewport);
    EXPECT_EQ(renderer.draws.size(), 2u);
    EXPECT_EQ(renderer.meshes.size(), 1u);
    EXPECT_EQ(renderer.materials.size(), 1u);

    panel->GetComponent<Image>()->OnDestroy();
    EXPECT_EQ(renderer.destroyedMeshes, 1);
    EXPECT_EQ(renderer.destroyedMaterials, 1);
}

// ============================================================================
// Hit testing
// ============================================================================

TEST(UIHitTest, TopmostRaycastTargetWins)
{
    const auto scene = Scene::Create("UIHit_Topmost");
    const auto canvas = AddCanvas(*scene, "Canvas");
    const auto back = AddPanel(canvas, "Back", Rect{0.0f, 0.0f, 400.0f, 400.0f});
    const auto front = AddPanel(canvas, "Front", Rect{100.0f, 100.0f, 100.0f, 100.0f});
    const auto child = AddPanel(back, "ChildOfBack", Rect{300.0f, 300.0f, 50.0f, 50.0f});

    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{150.0f, 150.0f}, Viewport), front.get()) << "later sibling on top";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{50.0f, 50.0f}, Viewport), back.get());
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{320.0f, 320.0f}, Viewport), child.get()) << "child over parent";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{500.0f, 500.0f}, Viewport), nullptr);

    // A higher sortOrder canvas is on top even when it comes first in the hierarchy
    const auto overlay = AddCanvas(*scene, "Overlay", 5);
    const auto popup = AddPanel(overlay, "Popup", Rect{140.0f, 140.0f, 20.0f, 20.0f});
    const auto lower = AddCanvas(*scene, "Lower", -5);
    AddPanel(lower, "Hidden", Rect{0.0f, 0.0f, 800.0f, 600.0f});
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{150.0f, 150.0f}, Viewport), popup.get());
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{50.0f, 50.0f}, Viewport), back.get());
}

TEST(UIHitTest, NonRaycastTargetsAreIgnored)
{
    const auto scene = Scene::Create("UIHit_RaycastTarget");
    const auto canvas = AddCanvas(*scene, "Canvas");
    const auto back = AddPanel(canvas, "Back", Rect{0.0f, 0.0f, 400.0f, 400.0f});
    const auto front = AddPanel(canvas, "Front", Rect{0.0f, 0.0f, 400.0f, 400.0f});

    front->GetComponent<Image>()->SetRaycastTarget(false);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{10.0f, 10.0f}, Viewport), back.get()) << "the click goes through";

    back->GetComponent<Image>()->SetRaycastTarget(false);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{10.0f, 10.0f}, Viewport), nullptr);
}

TEST(UIHitTest, InactiveAndDisabledElementsAreIgnored)
{
    const auto scene = Scene::Create("UIHit_Inactive");
    const auto canvas = AddCanvas(*scene, "Canvas");
    const auto back = AddPanel(canvas, "Back", Rect{0.0f, 0.0f, 400.0f, 400.0f});
    const auto front = AddPanel(canvas, "Front", Rect{0.0f, 0.0f, 400.0f, 400.0f});
    const auto frontChild = AddPanel(front, "FrontChild", Rect{0.0f, 0.0f, 50.0f, 50.0f});

    front->SetActive(false);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{10.0f, 10.0f}, Viewport), back.get())
        << "an inactive object and its children are skipped";

    front->SetActive(true);
    front->GetComponent<Image>()->SetActive(false);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{10.0f, 10.0f}, Viewport), frontChild.get());
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{100.0f, 100.0f}, Viewport), back.get()) << "a disabled Image";

    canvas->SetActive(false);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{10.0f, 10.0f}, Viewport), nullptr) << "an inactive canvas";
    canvas->SetActive(true);
    canvas->GetComponent<Canvas>()->SetActive(false);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{10.0f, 10.0f}, Viewport), nullptr) << "a disabled Canvas";
}

TEST(UIHitTest, WindowPointsAreFlippedAndClippedToTheViewport)
{
    const auto scene = Scene::Create("UIHit_Window");
    const auto canvas = AddCanvas(*scene, "Canvas");
    // The bottom-left 100x100 of the canvas
    const auto corner = AddPanel(canvas, "BottomLeft", Rect{0.0f, 0.0f, 100.0f, 100.0f});

    EXPECT_EQ(UISystem::WindowToCanvas(Vector2{30.0f, 590.0f}, Viewport).y, 10.0f);
    EXPECT_EQ(UISystem::HitTestWindowPoint(*scene, Vector2{30.0f, 590.0f}, Viewport), corner.get())
        << "near the window's bottom edge";
    EXPECT_EQ(UISystem::HitTestWindowPoint(*scene, Vector2{30.0f, 10.0f}, Viewport), nullptr) << "near the top";
    EXPECT_EQ(UISystem::HitTestWindowPoint(*scene, Vector2{-1.0f, 590.0f}, Viewport), nullptr) << "outside";
    EXPECT_EQ(UISystem::HitTestWindowPoint(*scene, Vector2{30.0f, 600.0f}, Viewport), nullptr) << "outside";
}

TEST(UIHitTest, ApplicationProviderWithoutAWindowHitsNothing)
{
    const auto provider = UISystem::MakeApplicationHitProvider();
    EXPECT_EQ(provider(Vector2{10.0f, 10.0f}), nullptr);
}

// ============================================================================
// Pointer events through the dispatcher
// ============================================================================

namespace
{
    class UIPointerRecorder final : public Component
    {
    public:
        explicit UIPointerRecorder(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "UIPointerRecorder"; }

        void OnMouseEnter() override { Record("Enter"); }
        void OnMouseOver() override { Record("Over"); }
        void OnMouseExit() override { Record("Exit"); }
        void OnMouseDown() override { Record("Down"); }
        void OnMouseDrag() override { Record("Drag"); }
        void OnMouseUp() override { Record("Up"); }
        void OnMouseUpAsButton() override { Record("UpAsButton"); }

        std::string name;
        std::vector<std::string> *log = nullptr;

    private:
        void Record(const std::string &event) const { log->push_back(name + "." + event); }
    };
}

// The world is a fake that always has `_world` under the pointer; the UI is the real hit test on a scene.
// Window coordinates: the button panel covers x 0..100, y 500..600 (the bottom-left of the canvas).
class UIPointerTest : public ::testing::Test
{
protected:
    std::unique_ptr<Scene> _scene = Scene::Create("UIPointer");
    Input::PointerDispatcher _dispatcher;
    Input::Mouse _mouse{nullptr};
    GameObject::Ptr _canvas;
    GameObject::Ptr _button;
    GameObject::Ptr _world = GameObject::Create("World");
    int _worldProviderCalls = 0;
    std::vector<std::string> _log;

    void SetUp() override
    {
        _canvas = AddCanvas(*_scene, "Canvas");
        _button = AddPanel(_canvas, "Button", Rect{0.0f, 0.0f, 100.0f, 100.0f});
        Record(*_button, "Button");
        Record(*_world, "World");

        _dispatcher.SetUIHitProvider([this](const Vector2 &point)
        {
            return UISystem::HitTestWindowPoint(*_scene, point, Viewport);
        });
        _dispatcher.SetWorldHitProvider([this](const Vector2 &)
        {
            ++_worldProviderCalls;
            return _world.get();
        });
    }

    void Record(GameObject &gameObject, const std::string &name)
    {
        auto *recorder = gameObject.AddComponent<UIPointerRecorder>();
        recorder->name = name;
        recorder->log = &_log;
    }

    std::vector<std::string> Frame(const Vector2 &windowPoint, const bool held)
    {
        _log.clear();
        _mouse.InjectPointer(windowPoint, held ? Input::Mouse::ButtonBit(0) : 0u);
        _mouse.Update();
        _dispatcher.Process(Input::PointerState::FromMouse(_mouse));
        return _log;
    }

    using Events = std::vector<std::string>;
    const Vector2 _overButton{50.0f, 550.0f};
    const Vector2 _overWorld{400.0f, 100.0f};
};

TEST_F(UIPointerTest, UIHitBlocksTheWorldPick)
{
    EXPECT_EQ(Frame(_overButton, true), (Events{"Button.Down", "Button.Enter", "Button.Over"}));
    EXPECT_TRUE(_dispatcher.IsPointerOverUI());
    EXPECT_EQ(_worldProviderCalls, 0) << "the world isn't picked while UI is under the pointer";
    EXPECT_EQ(_dispatcher.GetCaptured(), _button.get());
}

TEST_F(UIPointerTest, ClickOnAnElement)
{
    Frame(_overButton, false);
    EXPECT_EQ(Frame(_overButton, true), (Events{"Button.Down", "Button.Over"}));
    EXPECT_EQ(Frame(_overButton, true), (Events{"Button.Drag", "Button.Over"}));
    EXPECT_EQ(Frame(_overButton, false), (Events{"Button.UpAsButton", "Button.Up", "Button.Over"}));
}

TEST_F(UIPointerTest, MovingOffTheUIReachesTheWorld)
{
    Frame(_overButton, false);
    EXPECT_EQ(Frame(_overWorld, false), (Events{"Button.Exit", "World.Enter", "World.Over"}));
    EXPECT_FALSE(_dispatcher.IsPointerOverUI());
    EXPECT_EQ(_worldProviderCalls, 1);

    // Dragging from the button onto the world keeps the button's capture
    Frame(_overButton, true);
    EXPECT_EQ(Frame(_overWorld, true), (Events{"Button.Drag", "Button.Exit", "World.Enter", "World.Over"}));
    EXPECT_EQ(Frame(_overWorld, false), (Events{"Button.Up", "World.Over"}));
}

TEST_F(UIPointerTest, NonRaycastTargetAndInactiveElementsLetThePointerThrough)
{
    _button->GetComponent<Image>()->SetRaycastTarget(false);
    EXPECT_EQ(Frame(_overButton, false), (Events{"World.Enter", "World.Over"}));
    EXPECT_FALSE(_dispatcher.IsPointerOverUI());

    _button->GetComponent<Image>()->SetRaycastTarget(true);
    EXPECT_EQ(Frame(_overButton, false), (Events{"World.Exit", "Button.Enter", "Button.Over"}));
    EXPECT_TRUE(_dispatcher.IsPointerOverUI());

    _button->SetActive(false);
    EXPECT_EQ(Frame(_overButton, false), (Events{"World.Enter", "World.Over"}))
        << "an element deactivated while hovered gets no Exit (the picking rule)";
    EXPECT_FALSE(_dispatcher.IsPointerOverUI());
}
