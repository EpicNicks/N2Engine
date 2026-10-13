#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/ParticleSystem.hpp"

// Golden-image tests through the engine: a ParticleSystem drawn by Scene::Render on a headless SoftwareRenderer (no
// window or GPU) and read back. The checks are structural (colours, pixel counts, where the ink lies) with
// tolerances, never exact images: Debug and Release may round floats differently. The software renderer can't blend,
// so the quads are opaque there.

using namespace N2Engine;
using Renderer::Software::SoftwareRenderer;

namespace
{
    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

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
    };

    Frame ReadBack(const SoftwareRenderer &renderer, const int width, const int height)
    {
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4),
                    width, height};
        renderer.ReadFramebuffer(frame.rgba.data(), width, height);
        return frame;
    }

    constexpr Rgb Black{0, 0, 0};

    struct Coverage
    {
        int count = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1;
    };

    Coverage CoverageExcept(const Frame &frame, const Rgb background)
    {
        Coverage c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (frame.At(x, y) != background)
                {
                    ++c.count;
                    c.minX = std::min(c.minX, x);
                    c.maxX = std::max(c.maxX, x);
                    c.minY = std::min(c.minY, y);
                    c.maxY = std::max(c.maxY, y);
                }
            }
        }
        return c;
    }

    constexpr int kSize = 64;
    constexpr float kUnitsAcross = 4.0f;                              // the camera sees 4 x 4 world units
    constexpr float kPixelsPerUnit = static_cast<float>(kSize) / kUnitsAcross;

    /// An orthographic camera on +Z looking at the origin
    Camera MakeCamera()
    {
        Camera camera;
        camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
        camera.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
        return camera;
    }

    Frame Draw(Scene &scene, const Camera &camera, SoftwareRenderer &renderer)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        renderer.BeginFrame();
        renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
        scene.Render(&renderer, camera);
        renderer.EndFrame();
        renderer.Present();
        return ReadBack(renderer, kSize, kSize);
    }

    /// A scene holding one object with a started CPU ParticleSystem that has no stream: tests burst into it
    struct Setup
    {
        std::unique_ptr<Scene> scene;
        GameObject::Ptr object;
        Rendering::ParticleSystem *system = nullptr;
    };

    Setup MakeSetup(const std::string &name)
    {
        Setup setup;
        setup.scene = Scene::Create(name);
        setup.object = GameObject::Create(name + "_Object");
        setup.system = setup.object->AddComponent<Rendering::ParticleSystem>();
        setup.system->backend = Particles::ParticleBackend::CPU;
        setup.system->playOnAwake = false;
        setup.system->lifetimeMin = 1.0f;
        setup.system->lifetimeMax = 1.0f;
        setup.system->speedMin = 0.0f;
        setup.system->speedMax = 0.0f;
        setup.system->gravityScale = 0.0f;
        setup.system->shapeRadius = 0.0f;
        setup.system->startColor = Common::Color(1.0f, 0.0f, 0.0f, 1.0f);
        setup.system->endColor = Common::Color(1.0f, 0.0f, 0.0f, 1.0f);
        setup.scene->AddRootGameObject(setup.object);
        setup.system->OnAttach(); // the scene would attach it when it plays
        return setup;
    }
}

TEST(SoftwareParticlesGoldenTest, AParticleDrawsAsASolidSquareOfItsColourAtItsPosition)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, kSize, kSize));
    Setup fx = MakeSetup("SoftwareParticles_One");
    fx.system->startSize = 1.0f;
    fx.system->endSize = 1.0f;
    fx.object->GetPositionable()->SetPosition(Math::Vector3(0.5f, -0.5f, 0.0f));
    fx.system->EmitBurst(1);

    const Frame frame = Draw(*fx.scene, MakeCamera(), renderer);
    const Coverage c = CoverageExcept(frame, Black);
    ASSERT_GT(c.count, 0) << "the particle drew nothing";

    // Every touched pixel is the particle's red: nothing blended, nothing else
    int otherColours = 0;
    for (int y = 0; y < kSize; ++y)
    {
        for (int x = 0; x < kSize; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            if (pixel != Black && pixel != Rgb{255, 0, 0})
            {
                ++otherColours;
            }
        }
    }
    EXPECT_EQ(otherColours, 0);

    // A 1 x 1 world unit quad is 16 x 16 pixels, centred on world (0.5, -0.5) = pixel (40, 24)
    const float expectedSide = kPixelsPerUnit;
    EXPECT_NEAR(static_cast<float>(c.maxX - c.minX + 1), expectedSide, 2.0f);
    EXPECT_NEAR(static_cast<float>(c.maxY - c.minY + 1), expectedSide, 2.0f);
    EXPECT_NEAR(static_cast<float>(c.minX + c.maxX + 1) * 0.5f, 40.0f, 1.5f);
    EXPECT_NEAR(static_cast<float>(c.minY + c.maxY + 1) * 0.5f, 24.0f, 1.5f);
    EXPECT_NEAR(static_cast<float>(c.count), expectedSide * expectedSide, 40.0f) << "a filled square";

    fx.system->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}

TEST(SoftwareParticlesGoldenTest, NoParticlesLeaveTheFrameUntouched)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, kSize, kSize));
    Setup fx = MakeSetup("SoftwareParticles_None");
    const Frame frame = Draw(*fx.scene, MakeCamera(), renderer);
    EXPECT_EQ(CoverageExcept(frame, Black).count, 0);
    renderer.Shutdown();
}

TEST(SoftwareParticlesGoldenTest, TheInkLiesWhereTheParticlesAre)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, kSize, kSize));
    Setup fx = MakeSetup("SoftwareParticles_Sphere");
    fx.system->shape = Particles::EmitterShape::Sphere;
    fx.system->shapeRadius = 1.0f;
    fx.system->startSize = 0.5f;
    fx.system->endSize = 0.5f;
    fx.system->EmitBurst(60);
    ASSERT_EQ(fx.system->GetAliveCount(), 60u);

    std::vector<Math::Vector4> particles;
    fx.system->GetParticles(particles);
    float minX = 1e9f;
    float maxX = -1e9f;
    for (const Math::Vector4 &p : particles)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }

    const Frame frame = Draw(*fx.scene, MakeCamera(), renderer);
    const Coverage c = CoverageExcept(frame, Black);
    ASSERT_GT(c.count, 0);
    // World x lands on pixel (x + 2) * 16; quads reach 0.25 units (4 px) beyond the particle centres
    EXPECT_NEAR(static_cast<float>(c.minX), (minX - 0.25f + 2.0f) * kPixelsPerUnit, 2.5f);
    EXPECT_NEAR(static_cast<float>(c.maxX + 1), (maxX + 0.25f + 2.0f) * kPixelsPerUnit, 2.5f);
    // Inside the sphere's footprint plus the quads: never beyond 1.25 units from the centre
    EXPECT_GE(c.minX, static_cast<int>((2.0f - 1.25f) * kPixelsPerUnit) - 2);
    EXPECT_LE(c.maxX, static_cast<int>((2.0f + 1.25f) * kPixelsPerUnit) + 2);
    EXPECT_GE(c.minY, static_cast<int>((2.0f - 1.25f) * kPixelsPerUnit) - 2);
    EXPECT_LE(c.maxY, static_cast<int>((2.0f + 1.25f) * kPixelsPerUnit) + 2);

    fx.system->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}

TEST(SoftwareParticlesGoldenTest, AParticleFacesTheCameraWhateverWayItIsTurned)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, kSize, kSize));
    Setup fx = MakeSetup("SoftwareParticles_Facing");
    fx.system->startSize = 1.0f;
    fx.system->endSize = 1.0f;
    fx.system->EmitBurst(1);

    const Frame straight = Draw(*fx.scene, MakeCamera(), renderer);
    const int straightCount = CoverageExcept(straight, Black).count;
    ASSERT_GT(straightCount, 100);

    // The camera comes round to 45 degrees about Y, still looking at the particle. A quad fixed in the XY plane
    // would be foreshortened to about 70 percent; a billboard is as big as before.
    Camera turned;
    turned.SetPosition(Math::Vector3(3.5f, 0.0f, 3.5f));
    turned.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));
    turned.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
    const Frame angled = Draw(*fx.scene, turned, renderer);
    const int angledCount = CoverageExcept(angled, Black).count;
    EXPECT_NEAR(static_cast<float>(angledCount), static_cast<float>(straightCount), 0.12f * static_cast<float>(straightCount))
        << "straight " << straightCount << ", angled " << angledCount;

    fx.system->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}

TEST(SoftwareParticlesGoldenTest, ColourAndSizeFollowTheParticlesAge)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, kSize, kSize));
    Setup fx = MakeSetup("SoftwareParticles_Age");
    Time::SetTimeScale(1.0f);
    ASSERT_TRUE(Time::SetFixedTimestep(0.02));
    fx.system->lifetimeMin = 1.0f;
    fx.system->lifetimeMax = 1.0f;
    fx.system->startColor = Common::Color(1.0f, 0.0f, 0.0f, 1.0f);
    fx.system->endColor = Common::Color(0.0f, 0.0f, 1.0f, 1.0f);
    fx.system->startSize = 2.0f;
    fx.system->endSize = 0.5f;
    fx.system->EmitBurst(1);

    const Frame young = Draw(*fx.scene, MakeCamera(), renderer);
    const Coverage youngCoverage = CoverageExcept(young, Black);
    ASSERT_GT(youngCoverage.count, 0);
    const Rgb youngPixel = young.At(kSize / 2, kSize / 2);
    EXPECT_EQ(youngPixel, (Rgb{255, 0, 0})) << "red at birth";

    for (int i = 0; i < 25; ++i)
    {
        fx.system->OnFixedUpdate(); // half a second
    }
    ASSERT_EQ(fx.system->GetAliveCount(), 1u);
    const Frame middle = Draw(*fx.scene, MakeCamera(), renderer);
    const Rgb middlePixel = middle.At(kSize / 2, kSize / 2);
    EXPECT_NEAR(static_cast<int>(middlePixel.r), 128, 4);
    EXPECT_EQ(middlePixel.g, 0);
    EXPECT_NEAR(static_cast<int>(middlePixel.b), 128, 4);
    EXPECT_LT(CoverageExcept(middle, Black).count, youngCoverage.count) << "the quad shrank (2 to 1.25 units)";

    for (int i = 0; i < 30; ++i)
    {
        fx.system->OnFixedUpdate();
    }
    EXPECT_EQ(fx.system->GetAliveCount(), 0u) << "past its one second lifetime";
    const Frame gone = Draw(*fx.scene, MakeCamera(), renderer);
    EXPECT_EQ(CoverageExcept(gone, Black).count, 0);

    fx.system->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}

TEST(SoftwareParticlesGoldenTest, AFullyFadedParticleIsNotDrawn)
{
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, kSize, kSize));
    Setup fx = MakeSetup("SoftwareParticles_Faded");
    fx.system->startColor = Common::Color(1.0f, 0.0f, 0.0f, 0.0f); // alpha 0 from the start
    fx.system->endColor = Common::Color(1.0f, 0.0f, 0.0f, 0.0f);
    fx.system->EmitBurst(5);
    const Frame frame = Draw(*fx.scene, MakeCamera(), renderer);
    EXPECT_EQ(CoverageExcept(frame, Black).count, 0) << "the software renderer can't blend, so it skips alpha 0";
    fx.system->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}
