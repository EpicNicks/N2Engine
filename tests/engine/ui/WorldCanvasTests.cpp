#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Ray.hpp>
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
#include "engine/IRenderable.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

// World-space canvases without a window or GPU: the render mode and its serialization, the canvas-to-world
// matrix, the single Transparent-queue draw (state, order, hierarchy order inside, overlay separation, through a
// renderer that only records), the ray hit test's maths, and a Button clicked through a PointerDispatcher

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;
using Math::Vector3;
using Renderer::Common::CullMode;
using Renderer::Common::RenderState;

namespace
{
    constexpr float Tolerance = 1e-4f;
    const Vector2i Viewport{800, 600};
    const Vector2 Centre{400.0f, 300.0f};

    void ExpectVector(const Vector3 &actual, const Vector3 &expected, const char *what = "")
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.z, expected.z, Tolerance) << what << " z";
    }

    // ===== A renderer that records full model matrices =====

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
        void SetColor(const std::string &name, const float r, const float, const float, const float) override
        {
            if (name == "uAlbedo")
            {
                red = r;
            }
        }
        void SetTexture(Renderer::Common::ITexture *t) override { texture = t; }
        [[nodiscard]] Renderer::Common::IShader *GetShader() const override { return shader; }
        [[nodiscard]] Renderer::Common::ITexture *GetTexture() const override { return texture; }
        [[nodiscard]] bool IsValid() const override { return true; }

        Renderer::Common::IShader *shader;
        Renderer::Common::ITexture *texture = nullptr;
        float red = 0.0f;
    };

    struct RecordedDraw
    {
        std::array<float, 16> model{};
        RenderState state;
        float red = -1.0f; // an Image's colour's red channel; -1 for a Marker
        int tag = -1;      // a Marker's tag; -1 for an Image

        /// The model matrix applied to a point of the unit square (z = 0)
        [[nodiscard]] Vector3 Apply(const float u, const float v) const
        {
            return Vector3{model[0] * u + model[1] * v + model[3], model[4] * u + model[5] * v + model[7],
                           model[8] * u + model[9] * v + model[11]};
        }
    };

    class RecordingRenderer final : public Renderer::Common::IRenderer
    {
    public:
        std::vector<RecordedDraw> draws;
        std::vector<std::unique_ptr<FakeMesh>> meshes;
        std::vector<std::unique_ptr<FakeMaterial>> materials;

        using IRenderer::DrawMesh;
        void DrawMesh(Renderer::Common::IMesh *, const float *model, Renderer::Common::IMaterial *material,
                      const RenderState &state) override
        {
            RecordedDraw draw;
            std::copy_n(model, 16, draw.model.begin());
            draw.state = state;
            if (material)
            {
                draw.red = static_cast<FakeMaterial *>(material)->red;
            }
            else
            {
                draw.tag = static_cast<int>(model[0]);
            }
            draws.push_back(draw);
        }
        void DrawObjects(const std::vector<Renderer::Common::RenderObject> &) override {}
        void SetViewProjection(const float *, const float *) override {}

        Renderer::Common::IMesh *CreateMesh(const Renderer::Common::MeshData &data) override
        {
            meshes.push_back(std::make_unique<FakeMesh>(data));
            return meshes.back().get();
        }
        void DestroyMesh(Renderer::Common::IMesh *) override {}
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
        void DestroyMaterial(Renderer::Common::IMaterial *) override {}
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
        [[nodiscard]] const char *GetRendererName() const override { return "RecordingWorldCanvas"; }

        /// What was drawn, in order: an Image's red channel, or 100 + a Marker's tag
        [[nodiscard]] std::vector<float> Sequence() const
        {
            std::vector<float> sequence;
            for (const RecordedDraw &draw : draws)
            {
                sequence.push_back(draw.tag >= 0 ? 100.0f + static_cast<float>(draw.tag) : draw.red);
            }
            return sequence;
        }

    private:
        FakeShader _shader;
    };

    /// A transparent renderable at its object's position that draws one tagged mesh
    class Marker final : public IRenderable
    {
    public:
        explicit Marker(GameObject &gameObject) : IRenderable(gameObject) { gameObject.CreatePositionable(); }

        [[nodiscard]] std::string GetTypeName() const override { return "WorldCanvasTest_Marker"; }
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override { return {RenderQueue::Transparent, 0}; }
        void Render(Renderer::Common::IRenderer *renderer) override
        {
            RenderInQueue(renderer, RenderState::Transparent(), RenderQueue::Transparent);
        }
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const RenderState &state, RenderQueue) override
        {
            float model[16]{};
            model[0] = static_cast<float>(tag);
            renderer->DrawMesh(nullptr, model, nullptr, state);
        }
        void InitializeRenderResources(Renderer::Common::IRenderer *) override {}
        void CleanupRenderResources(Renderer::Common::IRenderer *) override {}

        int tag = 0;
    };

    // ===== Scene helpers =====

    /// A world canvas of `size` canvas units, its centre (the default pivot) at `position`, 0.01 world units
    /// per canvas unit
    GameObject::Ptr AddWorldCanvas(Scene &scene, const std::string &name, const Vector3 &position,
                                   const Vector2 &size = Vector2{200.0f, 100.0f})
    {
        auto canvas = UISystem::CreateCanvas(name, CanvasRenderMode::WorldSpace);
        canvas->GetComponent<Canvas>()->SetSize(size);
        canvas->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(canvas);
        return canvas;
    }

    GameObject::Ptr AddOverlayCanvas(Scene &scene, const std::string &name)
    {
        auto canvas = UISystem::CreateCanvas(name);
        scene.AddRootGameObject(canvas);
        return canvas;
    }

    /// A child element at a fixed rect inside its parent (anchors and pivot at the bottom-left), with an Image
    /// whose red channel tags its draws
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

    void AddMarker(Scene &scene, const int tag, const Vector3 &position)
    {
        auto gameObject = GameObject::Create("Marker" + std::to_string(tag));
        gameObject->AddComponent<Marker>()->tag = tag;
        gameObject->GetPositionable()->SetPosition(position);
        scene.AddRootGameObject(gameObject);
    }

    /// The application's default view: from z = 10 down -z, 60 degrees, 800 x 600
    Camera DefaultCamera()
    {
        Camera camera;
        camera.SetPerspective(60.0f, 800.0f / 600.0f, 0.1f, 100.0f);
        camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
        return camera;
    }
}

// ============================================================================
// Render mode, size and placement
// ============================================================================

TEST(WorldCanvasTest, RenderModeDefaultsToOverlayAndWorldSpaceAddsATransformAndARect)
{
    const auto overlay = UISystem::CreateCanvas("Overlay");
    auto *canvas = overlay->GetComponent<Canvas>();
    EXPECT_EQ(canvas->GetRenderMode(), CanvasRenderMode::ScreenSpaceOverlay);
    EXPECT_FALSE(canvas->IsWorldSpace());
    EXPECT_FALSE(overlay->HasPositionable()) << "an overlay canvas needs no transform";
    EXPECT_FALSE(overlay->HasComponent<RectTransform>());

    canvas->SetRenderMode(CanvasRenderMode::WorldSpace);
    EXPECT_TRUE(canvas->IsWorldSpace());
    EXPECT_TRUE(overlay->HasPositionable());
    ASSERT_TRUE(overlay->HasComponent<RectTransform>());
    EXPECT_FLOAT_EQ(canvas->GetSize().x, 100.0f) << "Unity's default RectTransform size";
    EXPECT_FLOAT_EQ(overlay->GetPositionable()->GetScale().x, 1.0f) << "SetRenderMode leaves the scale alone";

    canvas->SetSize(Vector2{400.0f, 300.0f});
    EXPECT_FLOAT_EQ(overlay->GetComponent<RectTransform>()->GetSizeDelta().x, 400.0f);
    EXPECT_FLOAT_EQ(canvas->GetSize().y, 300.0f);

    canvas->SetRenderMode(CanvasRenderMode::ScreenSpaceOverlay);
    EXPECT_FALSE(canvas->IsWorldSpace());
    EXPECT_TRUE(overlay->HasPositionable()) << "nothing is removed on switching back";

    const auto world = UISystem::CreateCanvas("World", CanvasRenderMode::WorldSpace);
    EXPECT_TRUE(world->GetComponent<Canvas>()->IsWorldSpace());
    EXPECT_EQ(world->GetLayer(), Layers::UI);
    ASSERT_TRUE(world->HasPositionable());
    EXPECT_FLOAT_EQ(world->GetPositionable()->GetScale().x, 0.01f) << "a canvas unit is a hundredth of a unit";
    EXPECT_FLOAT_EQ(world->GetPositionable()->GetScale().y, 0.01f);
    EXPECT_FLOAT_EQ(world->GetComponent<Canvas>()->GetSize().x, 100.0f);
}

TEST(WorldCanvasTest, WithoutARectTransformTheSizeIsAHundredSquareAndThePivotTheCentre)
{
    const auto object = GameObject::Create("Bare");
    auto *canvas = object->AddComponent<Canvas>();
    EXPECT_FLOAT_EQ(canvas->GetSize().x, Canvas::DefaultWorldSize);
    EXPECT_FLOAT_EQ(canvas->GetSize().y, Canvas::DefaultWorldSize);
    EXPECT_FLOAT_EQ(canvas->GetPivot().x, 0.5f);

    // No Positionable either: centred on the origin, one world unit per canvas unit
    const Canvas::Matrix4 m = canvas->GetCanvasToWorldMatrix();
    EXPECT_FLOAT_EQ(m(0, 3), -50.0f);
    EXPECT_FLOAT_EQ(m(1, 3), -50.0f);
    EXPECT_FLOAT_EQ(m(0, 0), 1.0f);
}

TEST(WorldCanvasTest, CanvasToWorldPutsThePivotAtThePositionAndScalesCanvasUnits)
{
    const auto scene = Scene::Create("WorldCanvas_Matrix");
    const auto canvas = AddWorldCanvas(*scene, "World", Vector3(1.0f, 2.0f, 3.0f));
    const Canvas::Matrix4 m = canvas->GetComponent<Canvas>()->GetCanvasToWorldMatrix();

    ExpectVector(m.TransformPoint(Vector3(0.0f, 0.0f, 0.0f)), Vector3(0.0f, 1.5f, 3.0f), "bottom-left");
    ExpectVector(m.TransformPoint(Vector3(100.0f, 50.0f, 0.0f)), Vector3(1.0f, 2.0f, 3.0f), "the centre (pivot)");
    ExpectVector(m.TransformPoint(Vector3(200.0f, 100.0f, 0.0f)), Vector3(2.0f, 2.5f, 3.0f), "top-right");

    // A pivot at the bottom-left corner puts that corner at the position
    canvas->GetComponent<RectTransform>()->SetPivot(Vector2{0.0f, 0.0f});
    const Canvas::Matrix4 corner = canvas->GetComponent<Canvas>()->GetCanvasToWorldMatrix();
    ExpectVector(corner.TransformPoint(Vector3(0.0f, 0.0f, 0.0f)), Vector3(1.0f, 2.0f, 3.0f), "pivot (0, 0)");
}

// ============================================================================
// Serialization
// ============================================================================

TEST(WorldCanvasTest, RenderModeIsSavedByName)
{
    const auto source = GameObject::Create("Source");
    auto *canvas = source->AddComponent<Canvas>();
    canvas->SetSortOrder(4);
    canvas->SetRenderMode(CanvasRenderMode::WorldSpace);
    const nlohmann::json saved = canvas->Serialize();
    ASSERT_TRUE(saved.contains("renderMode"));
    EXPECT_EQ(saved["renderMode"].get<std::string>(), "WorldSpace");

    const auto target = GameObject::Create("Target");
    auto *copy = target->AddComponent<Canvas>();
    copy->Deserialize(saved);
    EXPECT_EQ(copy->GetRenderMode(), CanvasRenderMode::WorldSpace);
    EXPECT_EQ(copy->GetSortOrder(), 4);

    const auto overlay = GameObject::Create("Overlay");
    EXPECT_EQ(overlay->AddComponent<Canvas>()->Serialize()["renderMode"].get<std::string>(), "ScreenSpaceOverlay");
}

TEST(WorldCanvasTest, OldScenesAndUnknownNamesLoadAsOverlay)
{
    const auto source = GameObject::Create("Old");
    auto *canvas = source->AddComponent<Canvas>();
    nlohmann::json old = canvas->Serialize();
    old.erase("renderMode"); // a scene saved before world canvases
    old["sortOrder"] = 2;

    const auto fresh = GameObject::Create("Fresh");
    auto *freshCanvas = fresh->AddComponent<Canvas>();
    freshCanvas->Deserialize(old);
    EXPECT_EQ(freshCanvas->GetRenderMode(), CanvasRenderMode::ScreenSpaceOverlay);
    EXPECT_EQ(freshCanvas->GetSortOrder(), 2);

    // World space before loading, so the load visibly changes it
    const auto target = GameObject::Create("Loaded");
    auto *loaded = target->AddComponent<Canvas>();
    loaded->SetRenderMode(CanvasRenderMode::WorldSpace);
    nlohmann::json unknown = canvas->Serialize();
    unknown["renderMode"] = "ScreenSpaceCamera";
    loaded->Deserialize(unknown);
    EXPECT_EQ(loaded->GetRenderMode(), CanvasRenderMode::ScreenSpaceOverlay) << "an unknown name reads as overlay";
}

// ============================================================================
// Drawing
// ============================================================================

TEST(WorldCanvasDrawTest, OnlyAWorldSpaceRootCanvasDrawsInTheTransparentQueue)
{
    const auto scene = Scene::Create("WorldCanvas_Queues");
    const auto overlay = AddOverlayCanvas(*scene, "Overlay");
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f));

    const Canvas *overlayCanvas = overlay->GetComponent<Canvas>();
    EXPECT_FALSE(overlayCanvas->DrawsInQueue(RenderQueue::Opaque));
    EXPECT_FALSE(overlayCanvas->DrawsInQueue(RenderQueue::Transparent));

    const Canvas *worldCanvas = world->GetComponent<Canvas>();
    EXPECT_EQ(worldCanvas->GetRenderQueue().queue, RenderQueue::Transparent);
    EXPECT_TRUE(worldCanvas->DrawsInQueue(RenderQueue::Transparent));
    EXPECT_FALSE(worldCanvas->DrawsInQueue(RenderQueue::Opaque));
    EXPECT_TRUE(worldCanvas->IsRootCanvas());

    // A world-space canvas under another canvas is an ordinary element of it
    const auto nested = UISystem::CreateCanvas("Nested", CanvasRenderMode::WorldSpace);
    overlay->AddChild(nested, false);
    EXPECT_FALSE(nested->GetComponent<Canvas>()->IsRootCanvas());
    EXPECT_FALSE(nested->GetComponent<Canvas>()->DrawsInQueue(RenderQueue::Transparent));
}

TEST(WorldCanvasDrawTest, TheCanvasIsOneSortedTransparentItemDrawingInHierarchyOrder)
{
    const auto scene = Scene::Create("WorldCanvas_Order");
    AddMarker(*scene, 2, Vector3(0.0f, 0.0f, 5.0f)); // nearest the camera: drawn last
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f));
    const auto back = AddPanel(world, "Back", Rect{0.0f, 0.0f, 200.0f, 100.0f}, 0.25f);
    AddPanel(back, "Child", Rect{10.0f, 10.0f, 20.0f, 20.0f}, 0.5f); // a child over its parent
    AddPanel(world, "Front", Rect{50.0f, 25.0f, 100.0f, 50.0f}, 0.75f); // a later sibling over both
    AddMarker(*scene, 1, Vector3(0.0f, 0.0f, -5.0f)); // furthest: drawn first

    RecordingRenderer renderer;
    scene->Render(&renderer, DefaultCamera());

    EXPECT_EQ(renderer.Sequence(), (std::vector<float>{101.0f, 0.25f, 0.5f, 0.75f, 102.0f}))
        << "the canvas sorts by its position among the transparent items, its graphics stay together in order";
    for (const RecordedDraw &draw : renderer.draws)
    {
        if (draw.tag < 0)
        {
            EXPECT_EQ(draw.state, UISystem::WorldCanvasState());
        }
    }
    constexpr RenderState expected = UISystem::WorldCanvasState();
    EXPECT_TRUE(expected.depthTest) << "hidden behind opaque geometry";
    EXPECT_FALSE(expected.depthWrite);
    EXPECT_EQ(expected.cull, CullMode::None);
    EXPECT_TRUE(expected.blend);
}

TEST(WorldCanvasDrawTest, ModelIsCanvasToWorldTimesTheRectMatrix)
{
    const auto scene = Scene::Create("WorldCanvas_Model");
    const auto world = AddWorldCanvas(*scene, "World", Vector3(1.0f, 2.0f, 3.0f));
    AddPanel(world, "Panel", Rect{20.0f, 10.0f, 50.0f, 40.0f});

    RecordingRenderer renderer;
    scene->Render(&renderer, DefaultCamera());
    ASSERT_EQ(renderer.draws.size(), 1u);
    // The canvas's bottom-left is at (0, 1.5, 3); a canvas unit is 0.01
    ExpectVector(renderer.draws[0].Apply(0.0f, 0.0f), Vector3(0.2f, 1.6f, 3.0f), "the rect's bottom-left");
    ExpectVector(renderer.draws[0].Apply(1.0f, 1.0f), Vector3(0.7f, 2.0f, 3.0f), "the rect's top-right");

    // Rotated half a turn about y: the canvas is seen from behind, mirrored, and still drawn (no culling)
    world->GetPositionable()->SetRotation(Math::Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f),
                                                                          std::numbers::pi_v<float>));
    renderer.draws.clear();
    scene->Render(&renderer, DefaultCamera());
    ASSERT_EQ(renderer.draws.size(), 1u);
    ExpectVector(renderer.draws[0].Apply(0.0f, 0.0f), Vector3(1.8f, 1.6f, 3.0f), "mirrored in x");
}

TEST(WorldCanvasDrawTest, OverlayAndWorldCanvasesDrawInTheirOwnPasses)
{
    const auto scene = Scene::Create("WorldCanvas_Separation");
    const auto overlay = AddOverlayCanvas(*scene, "Overlay");
    AddPanel(overlay, "OverlayPanel", Rect{0.0f, 0.0f, 100.0f, 100.0f}, 0.5f);
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f));
    AddPanel(world, "WorldPanel", Rect{0.0f, 0.0f, 100.0f, 100.0f}, 0.25f);

    RecordingRenderer scenePass;
    scene->Render(&scenePass, DefaultCamera());
    EXPECT_EQ(scenePass.Sequence(), (std::vector<float>{0.25f})) << "the scene pass draws only the world canvas";

    RecordingRenderer uiPass;
    UISystem::Render(*scene, &uiPass, Viewport);
    EXPECT_EQ(uiPass.Sequence(), (std::vector<float>{0.5f})) << "the UI pass draws only the overlay canvas";
    ASSERT_EQ(uiPass.draws.size(), 1u);
    EXPECT_EQ(uiPass.draws[0].state, UISystem::OverlayState());

    EXPECT_EQ(UISystem::CollectGraphics(*scene, Viewport).size(), 1u);
    EXPECT_EQ(UISystem::CollectWorldCanvasGraphics(*world->GetComponent<Canvas>()).size(), 1u);
    EXPECT_TRUE(UISystem::CollectWorldCanvasGraphics(*overlay->GetComponent<Canvas>()).empty());
}

TEST(WorldCanvasDrawTest, ChildrenLayOutInTheCanvasRectAsOnAnOverlay)
{
    const auto scene = Scene::Create("WorldCanvas_Layout");
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f), Vector2{400.0f, 300.0f});
    const auto stretched = UISystem::CreateElement("Stretched");
    stretched->GetComponent<RectTransform>()->StretchToParent();
    stretched->AddComponent<Image>();
    world->AddChild(stretched, false);

    const std::vector<UIDrawItem> items = UISystem::CollectWorldCanvasGraphics(*world->GetComponent<Canvas>());
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].rect, (Rect{0.0f, 0.0f, 400.0f, 300.0f}));
    EXPECT_EQ(world->GetComponent<RectTransform>()->GetRect(), (Rect{0.0f, 0.0f, 400.0f, 300.0f}))
        << "the canvas's own rect, whatever its anchors";
}

TEST(WorldCanvasDrawTest, DisabledOrInactiveWorldCanvasesDrawNothing)
{
    const auto scene = Scene::Create("WorldCanvas_Disabled");
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f));
    AddPanel(world, "Panel", Rect{0.0f, 0.0f, 100.0f, 100.0f});

    world->GetComponent<Canvas>()->SetActive(false);
    RecordingRenderer renderer;
    scene->Render(&renderer, DefaultCamera());
    EXPECT_TRUE(renderer.draws.empty());

    world->GetComponent<Canvas>()->SetActive(true);
    world->SetActive(false);
    scene->Render(&renderer, DefaultCamera());
    EXPECT_TRUE(renderer.draws.empty());
}

// ============================================================================
// Ray hit testing
// ============================================================================

TEST(WorldCanvasHitTest, RayCrossesThePlaneAtTheExpectedCanvasPoint)
{
    const auto scene = Scene::Create("WorldCanvas_Plane");
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f));
    const Canvas::Matrix4 m = world->GetComponent<Canvas>()->GetCanvasToWorldMatrix();

    const Math::Ray ray{Vector3(0.5f, 0.25f, 10.0f), Vector3(0.0f, 0.0f, -1.0f)};
    const std::optional<CanvasRayHit> hit = UISystem::RaycastCanvasPlane(m, ray);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, 10.0f, Tolerance);
    EXPECT_NEAR(hit->canvasPoint.x, 150.0f, 1e-2f) << "(0.5 + 1) / 0.01";
    EXPECT_NEAR(hit->canvasPoint.y, 75.0f, 1e-2f) << "(0.25 + 0.5) / 0.01";

    // From behind: the canvas is two-sided
    const std::optional<CanvasRayHit> behind =
        UISystem::RaycastCanvasPlane(m, Math::Ray{Vector3(0.5f, 0.25f, -4.0f), Vector3(0.0f, 0.0f, 1.0f)});
    ASSERT_TRUE(behind.has_value());
    EXPECT_NEAR(behind->distance, 4.0f, Tolerance);
    EXPECT_NEAR(behind->canvasPoint.x, 150.0f, 1e-2f);

    EXPECT_FALSE(UISystem::RaycastCanvasPlane(m, Math::Ray(Vector3(0.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, 1.0f))))
        << "the plane is behind the ray's origin";
    EXPECT_FALSE(UISystem::RaycastCanvasPlane(m, Math::Ray(Vector3(0.0f, 0.0f, 10.0f), Vector3(1.0f, 0.0f, 0.0f))))
        << "parallel";
    EXPECT_FALSE(UISystem::RaycastCanvasPlane(m, ray, 5.0f)) << "beyond the maximum distance";
    EXPECT_TRUE(UISystem::RaycastCanvasPlane(m, ray, 10.5f));

    // A zero scale is no plane at all
    world->GetPositionable()->SetLocalScale(Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_FALSE(UISystem::RaycastCanvasPlane(world->GetComponent<Canvas>()->GetCanvasToWorldMatrix(), ray));
}

TEST(WorldCanvasHitTest, RotatedAndUnevenlyScaledCanvasesGiveLocalCoordinates)
{
    const auto scene = Scene::Create("WorldCanvas_Rotated");
    const auto world = AddWorldCanvas(*scene, "World", Vector3(1.0f, -2.0f, 0.5f));
    auto *positionable = world->GetPositionable();
    positionable->SetRotation(Math::Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.7f) *
                              Math::Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), -0.4f));
    positionable->SetLocalScale(Vector3(0.02f, 0.005f, 1.0f));
    const Canvas::Matrix4 m = world->GetComponent<Canvas>()->GetCanvasToWorldMatrix();

    // Aim at a known canvas point from an arbitrary origin
    const Vector3 target = m.TransformPoint(Vector3(37.0f, 81.0f, 0.0f));
    const Vector3 origin(3.0f, 4.0f, 9.0f);
    const Vector3 toTarget = target - origin;
    const std::optional<CanvasRayHit> hit =
        UISystem::RaycastCanvasPlane(m, Math::Ray{origin, toTarget.Normalized()});
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, toTarget.Length(), 1e-3f);
    EXPECT_NEAR(hit->canvasPoint.x, 37.0f, 1e-2f);
    EXPECT_NEAR(hit->canvasPoint.y, 81.0f, 1e-2f);
}

TEST(WorldCanvasHitTest, TheNearestCanvasHitWinsAndMissesFallThrough)
{
    const auto scene = Scene::Create("WorldCanvas_Nearest");
    const auto far = AddWorldCanvas(*scene, "Far", Vector3(0.0f, 0.0f, 0.0f));
    const auto farPanel = AddPanel(far, "FarPanel", Rect{0.0f, 0.0f, 200.0f, 100.0f});
    const auto near = AddWorldCanvas(*scene, "Near", Vector3(0.0f, 0.0f, 2.0f));
    const auto nearPanel = AddPanel(near, "NearPanel", Rect{0.0f, 0.0f, 200.0f, 100.0f});
    // Later in the hierarchy but further away: order doesn't matter, distance does
    const auto further = AddWorldCanvas(*scene, "Further", Vector3(0.0f, 0.0f, -3.0f));
    AddPanel(further, "FurtherPanel", Rect{0.0f, 0.0f, 200.0f, 100.0f});

    const Math::Ray ray{Vector3(0.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f)};
    WorldUIHit hit = UISystem::HitTestWorldCanvases(*scene, ray);
    EXPECT_EQ(hit.gameObject, nearPanel.get());
    EXPECT_EQ(hit.canvas, near->GetComponent<Canvas>());
    EXPECT_NEAR(hit.distance, 8.0f, Tolerance);
    EXPECT_NEAR(hit.canvasPoint.x, 100.0f, 1e-2f);

    // Nothing hittable on the near canvas there: the ray goes on to the next one
    nearPanel->GetComponent<Image>()->SetRaycastTarget(false);
    hit = UISystem::HitTestWorldCanvases(*scene, ray);
    EXPECT_EQ(hit.gameObject, farPanel.get());
    EXPECT_NEAR(hit.distance, 10.0f, Tolerance);

    // A ray that crosses the canvas planes outside every panel hits nothing
    hit = UISystem::HitTestWorldCanvases(*scene, Math::Ray{Vector3(5.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, -1.0f)});
    EXPECT_EQ(hit.gameObject, nullptr);

    // Overlay canvases are not part of it
    const auto overlay = AddOverlayCanvas(*scene, "Overlay");
    AddPanel(overlay, "OverlayPanel", Rect{0.0f, 0.0f, 800.0f, 600.0f});
    EXPECT_EQ(UISystem::HitTestWorldCanvases(*scene, ray).gameObject, farPanel.get());
}

TEST(WorldCanvasHitTest, OverlayFirstThenTheCameraRay)
{
    const auto scene = Scene::Create("WorldCanvas_ScreenPoint");
    const Camera camera = DefaultCamera();
    const auto world = AddWorldCanvas(*scene, "World", Vector3(0.0f, 0.0f, 0.0f));
    const auto worldPanel = AddPanel(world, "WorldPanel", Rect{0.0f, 0.0f, 200.0f, 100.0f});
    constexpr std::uint32_t mask = Layers::DefaultRaycastMask;

    EXPECT_EQ(UISystem::HitTestScreenPoint(*scene, &camera, Centre, Viewport, mask), worldPanel.get());
    EXPECT_EQ(UISystem::HitTestScreenPoint(*scene, &camera, Vector2{10.0f, 10.0f}, Viewport, mask), nullptr)
        << "the corner of the view misses the canvas";
    EXPECT_EQ(UISystem::HitTestScreenPoint(*scene, nullptr, Centre, Viewport, mask), nullptr)
        << "no camera, no world canvas";
    EXPECT_EQ(UISystem::HitTestScreenPoint(*scene, &camera, Vector2{-5.0f, 300.0f}, Viewport, mask), nullptr)
        << "outside the viewport";
    EXPECT_EQ(UISystem::HitTestWindowPoint(*scene, Centre, Viewport), nullptr)
        << "the overlay hit test doesn't see world canvases";

    // An overlay element over the same point wins
    const auto overlay = AddOverlayCanvas(*scene, "Overlay");
    const auto overlayPanel = AddPanel(overlay, "OverlayPanel", Rect{300.0f, 200.0f, 200.0f, 200.0f});
    EXPECT_EQ(UISystem::HitTestScreenPoint(*scene, &camera, Centre, Viewport, mask), overlayPanel.get());
}

TEST(WorldCanvasHitTest, ACanvasBehindTheCameraOrBeyondTheFarPlaneIsNotHit)
{
    const auto scene = Scene::Create("WorldCanvas_Behind");
    const Camera camera = DefaultCamera(); // at z = 10 looking down -z; far plane 100
    const auto behind = AddWorldCanvas(*scene, "Behind", Vector3(0.0f, 0.0f, 12.0f));
    AddPanel(behind, "BehindPanel", Rect{0.0f, 0.0f, 200.0f, 100.0f});
    const auto distant = AddWorldCanvas(*scene, "Distant", Vector3(0.0f, 0.0f, -150.0f));
    AddPanel(distant, "DistantPanel", Rect{0.0f, 0.0f, 200.0f, 100.0f});

    EXPECT_EQ(UISystem::HitTestScreenPoint(*scene, &camera, Centre, Viewport, Layers::DefaultRaycastMask), nullptr);
}

// ============================================================================
// A Button on a world canvas, clicked through a PointerDispatcher
// ============================================================================

namespace
{
    class WorldPointerRecorder final : public Component
    {
    public:
        explicit WorldPointerRecorder(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "WorldCanvasTest_PointerRecorder"; }
        void OnMouseDown() override { ++downs; }
        int downs = 0;
    };
}

// The application's default camera (z = 10, looking down -z) and a 1 x 1 unit world canvas at the origin with
// a button (CreateButton: 160 x 40 canvas units, centred) under the middle of the view
class WorldCanvasButtonTest : public ::testing::Test
{
protected:
    std::unique_ptr<Scene> _scene = Scene::Create("WorldCanvasButton");
    Camera _camera = DefaultCamera();
    Input::PointerDispatcher _dispatcher;
    Input::Mouse _mouse{nullptr};
    GameObject::Ptr _canvas;
    GameObject::Ptr _buttonObject;
    GameObject::Ptr _world = GameObject::Create("World");
    Button *_button = nullptr;
    int _clicks = 0;

    void SetUp() override
    {
        _canvas = UISystem::CreateCanvas("WorldCanvas", CanvasRenderMode::WorldSpace);
        _scene->AddRootGameObject(_canvas);
        _buttonObject = UISystem::CreateButton("Button", "Press");
        _canvas->AddChild(_buttonObject, false);
        _button = _buttonObject->GetComponent<Button>();
        _button->SetFadeDuration(0.0f);
        _button->AddOnClick([this] { ++_clicks; });
        _world->AddComponent<WorldPointerRecorder>();

        _dispatcher.SetUIHitProvider([this](const Vector2 &point)
        {
            return UISystem::HitTestScreenPoint(*_scene, &_camera, point, Viewport, _dispatcher.GetPickMask());
        });
        _dispatcher.SetWorldHitProvider([this](const Vector2 &) { return _world.get(); });
    }

    void Frame(const Vector2 &windowPoint, const bool held)
    {
        _mouse.InjectPointer(windowPoint, held ? Input::Mouse::ButtonBit(0) : 0u);
        _mouse.Update();
        _dispatcher.Process(Input::PointerState::FromMouse(_mouse));
    }
};

TEST_F(WorldCanvasButtonTest, ClickingTheButtonThroughTheCameraRay)
{
    Frame(Centre, false);
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted);
    EXPECT_TRUE(_dispatcher.IsPointerOverUI()) << "a world canvas element counts as UI";
    EXPECT_EQ(_dispatcher.GetHovered(), _buttonObject.get());

    Frame(Centre, true);
    EXPECT_EQ(_button->GetState(), Button::State::Pressed);
    Frame(Centre, false);
    EXPECT_EQ(_clicks, 1);
    EXPECT_EQ(_world->GetComponent<WorldPointerRecorder>()->downs, 0) << "the world behind got nothing";

    // Above the canvas the ray misses it and reaches the world
    Frame(Vector2{400.0f, 100.0f}, true);
    EXPECT_FALSE(_dispatcher.IsPointerOverUI());
    EXPECT_EQ(_world->GetComponent<WorldPointerRecorder>()->downs, 1);
    Frame(Vector2{400.0f, 100.0f}, false);
    EXPECT_EQ(_clicks, 1);
}

TEST_F(WorldCanvasButtonTest, MovingTheCanvasMovesWhereTheButtonIs)
{
    // Half a unit to the right: the button's centre is now right of the middle of the view
    _canvas->GetPositionable()->SetPosition(Vector3(2.0f, 0.0f, 0.0f));
    Frame(Centre, true);
    Frame(Centre, false);
    EXPECT_EQ(_clicks, 0);

    // Where (2, 0, 0) projects: x = 400 + 2 / (10 tan 30 deg * 4 / 3) * 400
    const float x = 400.0f + 2.0f / (10.0f * std::tan(std::numbers::pi_v<float> / 6.0f) * (4.0f / 3.0f)) * 400.0f;
    Frame(Vector2{x, 300.0f}, true);
    Frame(Vector2{x, 300.0f}, false);
    EXPECT_EQ(_clicks, 1);
}
