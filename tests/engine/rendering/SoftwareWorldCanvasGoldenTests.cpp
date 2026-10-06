#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

// Golden-image tests of a world-space canvas: drawn by Scene::Render (the canvas is one Transparent-queue item)
// on a headless SoftwareRenderer (no window or GPU) and read back. An Image filling the canvas, seen by a
// perspective camera at an angle, must cover the canvas's projected quad (worked out here from the camera's
// matrices), and an opaque cube in front of it must hide it (depth test), while one behind it doesn't. The
// checks are structural (colours inside and outside a margin around the projected edges), never exact images.

using namespace N2Engine;
using Math::Vector2;
using Math::Vector3;
using Rendering::BuiltinMesh;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Rendering::ShadingModel;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Size = 64; // square frames

    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

    constexpr Rgb kBlack{0, 0, 0};
    constexpr Rgb kRed{255, 0, 0};
    constexpr Rgb kBlue{0, 0, 255};

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        /// y counts up from the bottom row
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        [[nodiscard]] int Count(const Rgb colour) const
        {
            int count = 0;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    count += At(x, y) == colour ? 1 : 0;
                }
            }
            return count;
        }
    };

    /// A square perspective camera (45 degrees) at `position`, looking at the origin with y up
    Camera CameraAt(const Vector3 &position)
    {
        Camera camera;
        camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(position);
        camera.LookAt(Vector3(0.0f, 0.0f, 0.0f));
        return camera;
    }

    /// A world point to frame pixels (x right, y up from the bottom row), through the camera's matrices
    Vector2 Project(const Camera &camera, const Vector3 &point)
    {
        const Math::Matrix<float, 4, 4> viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
        const float x = viewProjection(0, 0) * point.x + viewProjection(0, 1) * point.y +
                        viewProjection(0, 2) * point.z + viewProjection(0, 3);
        const float y = viewProjection(1, 0) * point.x + viewProjection(1, 1) * point.y +
                        viewProjection(1, 2) * point.z + viewProjection(1, 3);
        const float w = viewProjection(3, 0) * point.x + viewProjection(3, 1) * point.y +
                        viewProjection(3, 2) * point.z + viewProjection(3, 3);
        return Vector2{(x / w * 0.5f + 0.5f) * static_cast<float>(Size), (y / w * 0.5f + 0.5f) * static_cast<float>(Size)};
    }

    /// The signed distance in pixels from a point to a convex quad's boundary: positive inside, negative outside
    /// (for corners in either winding)
    float SignedDistanceToQuad(const std::array<Vector2, 4> &quad, const Vector2 &point)
    {
        // The winding, from the quad's signed area
        float area = 0.0f;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const Vector2 &a = quad[i];
            const Vector2 &b = quad[(i + 1) % 4];
            area += a.x * b.y - b.x * a.y;
        }
        const float sign = area >= 0.0f ? 1.0f : -1.0f;

        float distance = 1e9f;
        for (std::size_t i = 0; i < 4; ++i)
        {
            const Vector2 &a = quad[i];
            const Vector2 &b = quad[(i + 1) % 4];
            const float ex = b.x - a.x;
            const float ey = b.y - a.y;
            const float length = std::sqrt(ex * ex + ey * ey);
            // Left of the edge (counter-clockwise) is inside
            const float side = (ex * (point.y - a.y) - ey * (point.x - a.x)) / length * sign;
            distance = std::min(distance, side);
        }
        return distance;
    }

    /// Builds a scene, draws it as Application::Render does for the scene pass, and reads it back
    Frame Render(const std::function<void(Scene &)> &build, const Camera &camera)
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4), Size, Size};
        {
            auto scene = Scene::Create("SoftwareWorldCanvasGolden");
            build(*scene);
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            scene.reset(); // the scene and its components go before the renderer shuts down
        }
        renderer.Shutdown();
        return frame;
    }

    /// A 2 x 2 unit world canvas centred on the origin, facing +Z, filled by a red Image. Returns the canvas.
    GameObject::Ptr AddRedCanvas(Scene &scene)
    {
        const auto canvas = UI::UISystem::CreateCanvas("WorldCanvas", UI::CanvasRenderMode::WorldSpace);
        canvas->GetComponent<UI::Canvas>()->SetSize(Vector2{200.0f, 200.0f}); // at 0.01 units per canvas unit
        const auto element = UI::UISystem::CreateElement("Fill");
        element->GetComponent<UI::RectTransform>()->StretchToParent();
        element->AddComponent<UI::Image>()->SetColor(Common::Color{1.0f, 0.0f, 0.0f, 1.0f});
        canvas->AddChild(element, false);
        scene.AddRootGameObject(canvas);
        return canvas;
    }

    /// An opaque blue unlit cube, `size` units across, at `position`
    void AddBlueCube(Scene &scene, const Vector3 &position, const float size)
    {
        auto gameObject = GameObject::Create("Cube");
        auto *renderer = gameObject->AddComponent<MeshRenderer>();
        renderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Cube));
        auto material = Material::Create(ShadingModel::Unlit);
        material->SetBaseColor(Common::Color{0.0f, 0.0f, 1.0f, 1.0f});
        renderer->SetMaterial(0, material);
        gameObject->GetPositionable()->SetPosition(position);
        gameObject->GetPositionable()->SetLocalScale(Vector3(size, size, size));
        scene.AddRootGameObject(gameObject);
    }

    // Up, to the right and in front of the canvas: its projection is an irregular quad, not a rectangle
    const Vector3 CameraPosition(2.0f, 1.0f, 4.0f);
}

TEST(SoftwareWorldCanvasGoldenTest, AnImageCoversTheCanvassProjectedQuad)
{
    const Camera camera = CameraAt(CameraPosition);
    const Frame frame = Render([](Scene &scene) { AddRedCanvas(scene); }, camera);

    const std::array<Vector2, 4> quad = {Project(camera, Vector3(-1.0f, -1.0f, 0.0f)),
                                         Project(camera, Vector3(1.0f, -1.0f, 0.0f)),
                                         Project(camera, Vector3(1.0f, 1.0f, 0.0f)),
                                         Project(camera, Vector3(-1.0f, 1.0f, 0.0f))};

    // Every pixel clearly inside the projected quad is red, every pixel clearly outside is background; a pixel
    // and a half on either side of each edge is left to rasterisation
    int inside = 0;
    int wrongInside = 0;
    int wrongOutside = 0;
    for (int y = 0; y < Size; ++y)
    {
        for (int x = 0; x < Size; ++x)
        {
            const float distance = SignedDistanceToQuad(
                quad, Vector2{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
            if (distance > 1.5f)
            {
                ++inside;
                wrongInside += frame.At(x, y) == kRed ? 0 : 1;
            }
            else if (distance < -1.5f)
            {
                wrongOutside += frame.At(x, y) == kBlack ? 0 : 1;
            }
        }
    }
    EXPECT_GT(inside, 300) << "the canvas must take up a good part of the frame for the check to mean anything";
    EXPECT_EQ(wrongInside, 0) << "pixels inside the projected canvas that aren't the Image's red";
    EXPECT_EQ(wrongOutside, 0) << "pixels outside the projected canvas that aren't background";

    // Not a screen-aligned rectangle: the near (right) edge is taller on screen than the far (left) one
    const float rightHeight = quad[2].y - quad[1].y;
    const float leftHeight = quad[3].y - quad[0].y;
    EXPECT_GT(rightHeight, leftHeight + 1.0f) << "perspective";
}

TEST(SoftwareWorldCanvasGoldenTest, AnOpaqueCubeInFrontHidesTheCanvas)
{
    const Camera camera = CameraAt(CameraPosition);
    // On the line from the camera to the canvas's centre, about 40% of the way: it projects onto the centre
    const Vector3 between = CameraPosition * 0.6f;
    const Frame front = Render([&between](Scene &scene)
    {
        AddRedCanvas(scene);
        AddBlueCube(scene, between, 0.3f);
    }, camera);

    const Vector2 centre = Project(camera, Vector3(0.0f, 0.0f, 0.0f));
    const int cx = static_cast<int>(centre.x);
    const int cy = static_cast<int>(centre.y);
    EXPECT_EQ(front.At(cx, cy), kBlue) << "the cube in front hides the canvas (the canvas is depth-tested)";
    EXPECT_GT(front.Count(kBlue), 20);
    EXPECT_GT(front.Count(kRed), 100) << "the rest of the canvas still shows around the cube";
    // A corner of the canvas, well away from the cube, is still red
    const Vector2 corner = Project(camera, Vector3(-0.8f, 0.8f, 0.0f));
    EXPECT_EQ(front.At(static_cast<int>(corner.x), static_cast<int>(corner.y)), kRed);

    // The same cube behind the canvas: the canvas, drawn after it in the Transparent queue, passes the depth
    // test and covers it
    const Frame behind = Render([&between](Scene &scene)
    {
        AddRedCanvas(scene);
        AddBlueCube(scene, between * -1.0f, 0.3f);
    }, camera);
    EXPECT_EQ(behind.At(cx, cy), kRed) << "the canvas is in front of the cube";
    EXPECT_EQ(behind.Count(kBlue), 0) << "the cube is entirely behind the canvas, which covers its projection";
}
