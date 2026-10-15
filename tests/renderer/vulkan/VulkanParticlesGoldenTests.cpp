#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/ParticleSystem.hpp"

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// The golden-image tests of the particle system on the Vulkan backend (#43, stage D): the six rendered tests of
// tests/engine/rendering/SoftwareParticlesGoldenTests.cpp with the same scenes, camera and sizes, drawn by Scene::Render on a
// Vulkan device (Mesa lavapipe in CI; no window, the offscreen target) and read back, then what only a backend that blends
// can show. The particles are CPU billboards rebuilt every frame into ONE mesh that is UpdateMesh'd in place, drawn in the
// Transparent queue (blended, no depth write, cull None) with the unlit shader, colour as vertex colour; the material has an
// alpha cutoff of 0.01.
//
// Where the particle's alpha is 1 the expectations are the software test's (alpha 1 blends to exactly the colour). Where it
// is not, they are OpenGL's: AFullyFadedParticleIsNotDrawn expects the frame to equal the clear colour, because an alpha-0
// particle contributes nothing: here it is the material's 0.01 alpha cutoff that discards the fragment, and it would blend
// to nothing anyway (SRC_ALPHA 0 x colour + 1 x destination), not because the renderer skips a draw it cannot blend as
// the software renderer does. The Vulkan-only tests: an Additive system whose two overlapping particles add, a playing
// system over more frames than are in flight with births and deaths (the one mesh grows and shrinks), and a particle
// mesh destroyed in the frame that drew it.
//
// The checks are structural (colours, pixel counts, where the ink lies) with tolerances, never exact images. The scenes
// are opaque-coloured (alpha 1) except the faded one, so no software frame is compared: the software renderer cannot draw a
// blended scene (BlendReference.hpp).

using namespace N2Engine;
using namespace VulkanTesting;
using namespace VulkanTesting::Draw;

namespace
{
    struct Extent
    {
        int count = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1;
    };

    /// The pixels that are not `background`: how many, and their bounding box
    Extent CoverageExcept(const Frame& frame, const Rgb background)
    {
        Extent c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (!(frame.At(x, y) == background))
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
    constexpr float kUnitsAcross = 4.0f; // the camera sees 4 x 4 world units
    constexpr float kPixelsPerUnit = static_cast<float>(kSize) / kUnitsAcross;

    /// An orthographic camera on +Z looking at the origin
    Camera MakeCamera()
    {
        Camera camera;
        camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
        camera.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
        return camera;
    }

    /// A scene holding one object with a started CPU ParticleSystem that has no stream: tests burst into it
    struct FxScene
    {
        std::unique_ptr<Scene> scene;
        GameObject::Ptr object;
        Rendering::ParticleSystem* system = nullptr;
    };

    FxScene MakeFxScene(const std::string& name)
    {
        FxScene setup;
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

    class VulkanParticlesGoldenTest : public VulkanTest
    {
    protected:
        void SetUp() override
        {
            VulkanTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            N2_REQUIRE_BUILT_IN_SHADERS();
            ASSERT_TRUE(R().SetRenderTargetSize(kSize, kSize));
        }

        /// Draws the scene as Application::Render draws the scene pass and reads the frame back
        Frame Draw(Scene& scene, const Camera& camera)
        {
            R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
            R().BeginFrame();
            R().SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            scene.Render(&R(), camera);
            R().EndFrame();
            R().Present();
            return ReadFrameNow();
        }

        Frame ReadFrameNow()
        {
            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(kSize) * kSize * 4), kSize, kSize};
            R().ReadFramebuffer(frame.rgba.data(), kSize, kSize);
            return frame;
        }
    };
}

TEST_F(VulkanParticlesGoldenTest, AParticleDrawsAsASolidSquareOfItsColourAtItsPosition)
{
    FxScene fx = MakeFxScene("VulkanParticles_One");
    fx.system->startSize = 1.0f;
    fx.system->endSize = 1.0f;
    fx.object->GetPositionable()->SetPosition(Math::Vector3(0.5f, -0.5f, 0.0f));
    fx.system->EmitBurst(1);

    const Frame frame = Draw(*fx.scene, MakeCamera());
    const Extent c = CoverageExcept(frame, kBlack);
    ASSERT_GT(c.count, 0) << "the particle drew nothing";

    // Every touched pixel is the particle's red: alpha 1 blends to exactly its colour, nothing else
    int otherColours = 0;
    for (int y = 0; y < kSize; ++y)
    {
        for (int x = 0; x < kSize; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            if (!(pixel == kBlack) && !(pixel == kRed))
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

    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, NoParticlesLeaveTheFrameUntouched)
{
    FxScene fx = MakeFxScene("VulkanParticles_None");
    const Frame frame = Draw(*fx.scene, MakeCamera());
    EXPECT_EQ(CoverageExcept(frame, kBlack).count, 0);
    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, TheInkLiesWhereTheParticlesAre)
{
    FxScene fx = MakeFxScene("VulkanParticles_Sphere");
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
    for (const Math::Vector4& p : particles)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }

    const Frame frame = Draw(*fx.scene, MakeCamera());
    const Extent c = CoverageExcept(frame, kBlack);
    ASSERT_GT(c.count, 0);
    // World x lands on pixel (x + 2) * 16; quads reach 0.25 units (4 px) beyond the particle centres
    EXPECT_NEAR(static_cast<float>(c.minX), (minX - 0.25f + 2.0f) * kPixelsPerUnit, 2.5f);
    EXPECT_NEAR(static_cast<float>(c.maxX + 1), (maxX + 0.25f + 2.0f) * kPixelsPerUnit, 2.5f);
    // Inside the sphere's footprint plus the quads: never beyond 1.25 units from the centre
    EXPECT_GE(c.minX, static_cast<int>((2.0f - 1.25f) * kPixelsPerUnit) - 2);
    EXPECT_LE(c.maxX, static_cast<int>((2.0f + 1.25f) * kPixelsPerUnit) + 2);
    EXPECT_GE(c.minY, static_cast<int>((2.0f - 1.25f) * kPixelsPerUnit) - 2);
    EXPECT_LE(c.maxY, static_cast<int>((2.0f + 1.25f) * kPixelsPerUnit) + 2);

    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, AParticleFacesTheCameraWhateverWayItIsTurned)
{
    FxScene fx = MakeFxScene("VulkanParticles_Facing");
    fx.system->startSize = 1.0f;
    fx.system->endSize = 1.0f;
    fx.system->EmitBurst(1);

    const Frame straight = Draw(*fx.scene, MakeCamera());
    const int straightCount = CoverageExcept(straight, kBlack).count;
    ASSERT_GT(straightCount, 100);

    // The camera comes round to 45 degrees about Y, still looking at the particle. A quad fixed in the XY plane
    // would be foreshortened to about 70 percent; a billboard is as big as before.
    Camera turned;
    turned.SetPosition(Math::Vector3(3.5f, 0.0f, 3.5f));
    turned.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));
    turned.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
    const Frame angled = Draw(*fx.scene, turned);
    const int angledCount = CoverageExcept(angled, kBlack).count;
    EXPECT_NEAR(static_cast<float>(angledCount), static_cast<float>(straightCount), 0.12f * static_cast<float>(straightCount))
        << "straight " << straightCount << ", angled " << angledCount;

    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, ColourAndSizeFollowTheParticlesAge)
{
    FxScene fx = MakeFxScene("VulkanParticles_Age");
    Time::SetTimeScale(1.0f);
    ASSERT_TRUE(Time::SetFixedTimestep(0.02));
    fx.system->lifetimeMin = 1.0f;
    fx.system->lifetimeMax = 1.0f;
    fx.system->startColor = Common::Color(1.0f, 0.0f, 0.0f, 1.0f);
    fx.system->endColor = Common::Color(0.0f, 0.0f, 1.0f, 1.0f);
    fx.system->startSize = 2.0f;
    fx.system->endSize = 0.5f;
    fx.system->EmitBurst(1);

    const Frame young = Draw(*fx.scene, MakeCamera());
    const Extent youngCoverage = CoverageExcept(young, kBlack);
    ASSERT_GT(youngCoverage.count, 0);
    const Rgb youngPixel = young.At(kSize / 2, kSize / 2);
    EXPECT_EQ(youngPixel, kRed) << "red at birth";

    for (int i = 0; i < 25; ++i)
    {
        fx.system->OnFixedUpdate(); // half a second
    }
    ASSERT_EQ(fx.system->GetAliveCount(), 1u);
    const Frame middle = Draw(*fx.scene, MakeCamera());
    const Rgb middlePixel = middle.At(kSize / 2, kSize / 2);
    EXPECT_NEAR(static_cast<int>(middlePixel.r), 128, 4);
    EXPECT_EQ(middlePixel.g, 0);
    EXPECT_NEAR(static_cast<int>(middlePixel.b), 128, 4);
    EXPECT_LT(CoverageExcept(middle, kBlack).count, youngCoverage.count) << "the quad shrank (2 to 1.25 units)";

    for (int i = 0; i < 30; ++i)
    {
        fx.system->OnFixedUpdate();
    }
    EXPECT_EQ(fx.system->GetAliveCount(), 0u) << "past its one second lifetime";
    const Frame gone = Draw(*fx.scene, MakeCamera());
    EXPECT_EQ(CoverageExcept(gone, kBlack).count, 0);

    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, AFullyFadedParticleIsNotDrawn)
{
    FxScene fx = MakeFxScene("VulkanParticles_Faded");
    fx.system->startColor = Common::Color(1.0f, 0.0f, 0.0f, 0.0f); // alpha 0 from the start
    fx.system->endColor = Common::Color(1.0f, 0.0f, 0.0f, 0.0f);
    fx.system->EmitBurst(5);
    const Frame frame = Draw(*fx.scene, MakeCamera());
    // Nothing is visible, and not because the renderer skips the draw (the software renderer, which cannot blend, does):
    // alpha 0 contributes nothing under GL's equation (SRC_ALPHA 0 x red + ONE_MINUS_SRC_ALPHA 1 x the clear colour), and
    // the particle material's 0.01 alpha cutoff discards the fragment before it gets that far. The frame is the clear.
    EXPECT_EQ(CoverageExcept(frame, kBlack).count, 0);
    EXPECT_EQ(frame.Count(kBlack), kSize * kSize);
    fx.system->CleanupRenderResources(&R());
}

// ============================================================================
// What only a backend that blends shows
// ============================================================================

TEST_F(VulkanParticlesGoldenTest, AnAdditiveSystemAddsWhereTwoParticlesOverlap)
{
    // The software renderer draws Additive as Alpha (it cannot blend), so this is Vulkan only. Two red particles of
    // (128, 0, 0), alpha 1, 2 units across, one unit apart in x: one covers x -1.5..0.5, the other -0.5..1.5. Additive
    // blending (SRC_ALPHA, ONE): one alone is 128 over the black clear, where they overlap 128 + 128 clamps to 255.
    FxScene fx = MakeFxScene("VulkanParticles_Additive");
    fx.system->blendMode = Rendering::ParticleBlend::Additive;
    fx.system->startSize = 2.0f;
    fx.system->endSize = 2.0f;
    fx.system->startColor = Common::Color(128.0f / 255.0f, 0.0f, 0.0f, 1.0f);
    fx.system->endColor = Common::Color(128.0f / 255.0f, 0.0f, 0.0f, 1.0f);
    fx.object->GetPositionable()->SetPosition(Math::Vector3(-0.5f, 0.0f, 0.0f));
    fx.system->EmitBurst(1);
    fx.object->GetPositionable()->SetPosition(Math::Vector3(0.5f, 0.0f, 0.0f));
    fx.system->EmitBurst(1);
    ASSERT_EQ(fx.system->GetAliveCount(), 2u);

    const Frame frame = Draw(*fx.scene, MakeCamera());
    // World x lands on pixel (x + 2) * 16: the left particle spans pixels 8..39, the right 24..55; y 16..47
    const Rgb leftOnly = frame.At(14, 32);
    const Rgb overlap = frame.At(32, 32);
    const Rgb rightOnly = frame.At(50, 32);
    EXPECT_NEAR(leftOnly.r, 128, 1) << "one particle alone";
    EXPECT_EQ(leftOnly.g, 0);
    EXPECT_NEAR(rightOnly.r, 128, 1);
    EXPECT_EQ(overlap.r, 255) << "two particles add and clamp";
    EXPECT_EQ(overlap.g, 0);
    EXPECT_EQ(overlap.b, 0);
    EXPECT_EQ(frame.At(2, 2), kBlack);
    EXPECT_EQ(frame.At(32, 56), kBlack) << "above both";

    // The same two particles as Alpha (alpha 1) just replace one another: no pixel is brighter than one particle
    fx.system->blendMode = Rendering::ParticleBlend::Alpha;
    const Frame alpha = Draw(*fx.scene, MakeCamera());
    EXPECT_NEAR(alpha.At(32, 32).r, 128, 1) << "Alpha: the overlap is one particle's colour";
    EXPECT_EQ(alpha.Count(Rgb{255, 0, 0}), 0);

    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, APlayingSystemOverMoreFramesThanAreInFlightWithBirthsAndDeathsDrawsEveryFrame)
{
    // One mesh updated every frame while particles are born and die: the vertex count grows, holds and shrinks to none.
    // Particles do not move (speed and gravity 0) inside the sphere of radius 1, so they all stay in view.
    FxScene fx = MakeFxScene("VulkanParticles_Playing");
    Time::SetTimeScale(1.0f);
    ASSERT_TRUE(Time::SetFixedTimestep(0.02));
    fx.system->shape = Particles::EmitterShape::Sphere;
    fx.system->shapeRadius = 1.0f;
    fx.system->emissionRate = 100.0f; // two per fixed step
    fx.system->lifetimeMin = 0.2f;    // ten steps
    fx.system->lifetimeMax = 0.2f;
    fx.system->startSize = 0.5f;
    fx.system->endSize = 0.5f;
    fx.system->Play();

    std::size_t mostAlive = 0;
    bool grew = false;
    bool shrank = false;
    std::size_t previous = 0;
    for (int frameNumber = 0; frameNumber < 40; ++frameNumber)
    {
        if (frameNumber == 14)
        {
            fx.system->Stop(); // no more births: the rest die
        }
        fx.system->OnFixedUpdate();
        const std::size_t alive = fx.system->GetAliveCount();
        mostAlive = std::max(mostAlive, alive);
        grew = grew || alive > previous;
        shrank = shrank || alive < previous;
        previous = alive;

        const Frame frame = Draw(*fx.scene, MakeCamera());
        const int covered = CoverageExcept(frame, kBlack).count;
        if (alive > 0)
        {
            EXPECT_GT(covered, 0) << "frame " << frameNumber << ": " << alive << " alive";
        }
        else
        {
            EXPECT_EQ(covered, 0) << "frame " << frameNumber << ": none alive";
        }
        // Whatever is drawn is the particles' red, nothing else (alpha 1)
        int otherColours = 0;
        for (int y = 0; y < kSize; ++y)
        {
            for (int x = 0; x < kSize; ++x)
            {
                const Rgb pixel = frame.At(x, y);
                otherColours += (!(pixel == kBlack) && !(pixel == kRed)) ? 1 : 0;
            }
        }
        EXPECT_EQ(otherColours, 0) << "frame " << frameNumber;
    }
    EXPECT_GT(mostAlive, 8u) << "many particles were alive at once";
    EXPECT_TRUE(grew);
    EXPECT_TRUE(shrank);
    EXPECT_EQ(fx.system->GetAliveCount(), 0u) << "they all died";

    fx.system->CleanupRenderResources(&R());
}

TEST_F(VulkanParticlesGoldenTest, AParticleMeshDestroyedInTheFrameThatDrewItStillShowsInThatFrame)
{
    FxScene fx = MakeFxScene("VulkanParticles_Destroyed");
    fx.system->startSize = 1.0f;
    fx.system->endSize = 1.0f;
    fx.system->EmitBurst(3);
    const Frame first = Draw(*fx.scene, MakeCamera()); // makes the mesh and the material
    ASSERT_GT(CoverageExcept(first, kBlack).count, 0);
    for (int i = 0; i < 3; ++i)
    {
        R().BeginFrame();
        R().EndFrame(); // every frame slot flushed
    }
    const std::uint32_t meshes = R().GetLiveMeshCountForTest();
    ASSERT_GE(meshes, 1u);

    // The draw is recorded, then the system gives its mesh (and material) back inside the same open frame
    const Camera camera = MakeCamera();
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
    R().BeginFrame();
    R().SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
    fx.scene->Render(&R(), camera);
    fx.system->CleanupRenderResources(&R());
    R().EndFrame();
    const Frame frame = ReadFrameNow();
    EXPECT_EQ(CoverageExcept(frame, kBlack).count, CoverageExcept(first, kBlack).count)
        << "the frame that drew the mesh still has it";
    EXPECT_EQ(R().GetLiveMeshCountForTest(), meshes - 1u) << "and it is gone from the renderer";
    for (int i = 0; i < 3; ++i)
    {
        R().BeginFrame();
        R().EndFrame();
    }
    EXPECT_EQ(R().GetPendingDeletionCountForTest(), 0u);
}
