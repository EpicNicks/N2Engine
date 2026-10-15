#pragma once

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "CrossBackendDiff.hpp"
#include "VulkanTestFixture.hpp"

// What the Vulkan lighting and normal-map/PBR tests share (#43, stage C): they mirror the RENDERED tests of
// tests/renderer/software/SoftwareLightingTests.cpp and SoftwareNormalMapPbrTests.cpp, which draw a quad facing +z with
// identity matrices on a 64x64 target. Here every draw is made on BOTH renderers (the Vulkan one under test and a headless
// SoftwareRenderer, the reference), the two frames are compared with the proposal's lit criterion (CrossBackendDiff.hpp:
// mean error <= 2/255, <= 1% of the pixels over 8/255, a 1-pixel coverage-edge border, an exclusion cap), and the Vulkan
// frame is what the test's own structural expectations (the software tests' numbers) are checked against.
//
// One thing differs from the software tests, on purpose: the software renderer infers "has a normal texture" from the
// texture being set, while OpenGL and Vulkan (like Material::ApplyUniforms in the engine) set the uHas*Texture flags
// themselves. DualMaterial::SetAuxTexture sets the flag on the Vulkan material the way ApplyUniforms does.

namespace VulkanTesting::Lit
{
    using Renderer::Common::AuxTexture;
    using Renderer::Common::IMaterial;
    using Renderer::Common::IMesh;
    using Renderer::Common::ITexture;
    using Renderer::Common::MeshData;
    using Renderer::Common::RenderState;
    using Renderer::Common::SceneLightingData;
    using Renderer::Common::Vertex;
    using Renderer::Software::SoftwareRenderer;
    using Renderer::Vulkan::VulkanRenderer;
    using N2Engine::Math::Vector3;

    inline constexpr int Size = 64;

    inline constexpr float Identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;

        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }

        [[nodiscard]] Diff::Image AsImage() const { return {rgba, Size, Size}; }
    };

    /// A quad over x, y in -1..1 at z = 0, facing +z, u across x and v up y, with a tangent +x of handedness `w` when
    /// `tangent` (a zero tangent otherwise)
    inline MeshData Quad(const bool tangent = false, const float w = 1.0f)
    {
        const float t = tangent ? 1.0f : 0.0f;
        const float h = tangent ? w : 0.0f;
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {t, 0, 0, h}},
            Vertex{{1, -1, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}, {t, 0, 0, h}},
            Vertex{{1, 1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}, {t, 0, 0, h}},
            Vertex{{-1, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}, {t, 0, 0, h}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    /// The world x of pixel column px at the middle row: the quad spans -1..1 over the frame
    inline float WorldX(const int px)
    {
        return (static_cast<float>(px) + 0.5f - static_cast<float>(Size) * 0.5f) / (static_cast<float>(Size) * 0.5f);
    }

    inline constexpr float Degrees(const float degrees)
    {
        return degrees * 3.14159265f / 180.0f;
    }

    /// A texture made on both renderers
    struct DualTexture
    {
        ITexture* vulkan = nullptr;
        ITexture* software = nullptr;
    };

    struct DualMesh
    {
        IMesh* vulkan = nullptr;
        IMesh* software = nullptr;
    };

    /// A material made on both renderers: every call goes to both
    class DualMaterial
    {
    public:
        DualMaterial() = default;
        DualMaterial(IMaterial* vulkanMaterial, IMaterial* softwareMaterial) : vulkan(vulkanMaterial), software(softwareMaterial) {}

        void SetFloat(const std::string& name, const float value) { vulkan->SetFloat(name, value); software->SetFloat(name, value); }
        void SetInt(const std::string& name, const int value) { vulkan->SetInt(name, value); software->SetInt(name, value); }
        void SetColor(const std::string& name, const float r, const float g, const float b, const float a)
        {
            vulkan->SetColor(name, r, g, b, a);
            software->SetColor(name, r, g, b, a);
        }
        void SetVec3(const std::string& name, const float x, const float y, const float z)
        {
            vulkan->SetVec3(name, x, y, z);
            software->SetVec3(name, x, y, z);
        }
        /// Sets the base texture on both, and on the Vulkan material uHasTexture, as Material::ApplyUniforms does (OpenGL and
        /// Vulkan take the flag from the caller, and SetTexture does not touch it; the software renderer uses the texture)
        void SetTexture(const DualTexture& texture)
        {
            vulkan->SetTexture(texture.vulkan);
            software->SetTexture(texture.software);
            vulkan->SetInt("uHasTexture", texture.vulkan != nullptr ? 1 : 0);
        }
        /// Sets (or, with a null texture, clears) an aux texture, and on the Vulkan material the uHas*Texture flag that goes
        /// with it, as Material::ApplyUniforms does (the software renderer works the flag out itself)
        void SetAuxTexture(const AuxTexture which, const DualTexture& texture)
        {
            vulkan->SetAuxTexture(which, texture.vulkan);
            software->SetAuxTexture(which, texture.software);
            const char* flag = which == AuxTexture::Emissive    ? "uHasEmissiveTexture"
                : which == AuxTexture::Occlusion                 ? "uHasOcclusionTexture"
                : which == AuxTexture::Normal                    ? "uHasNormalTexture"
                                                                 : "uHasMetallicRoughnessTexture";
            vulkan->SetInt(flag, texture.vulkan != nullptr ? 1 : 0);
        }
        [[nodiscard]] ITexture* GetAuxTexture(const AuxTexture which) const { return vulkan->GetAuxTexture(which); }

        IMaterial* vulkan = nullptr;
        IMaterial* software = nullptr;
    };

    /// The lit scene both renderers draw: a lit and an unlit white material and the quad meshes, identity view and
    /// projection (world space is the quad's), lighting set inside the frame as the engine does
    class LitScene
    {
    public:
        explicit LitScene(VulkanRenderer& vulkanRenderer) : _vulkan(vulkanRenderer)
        {
            EXPECT_TRUE(_software.Initialize(nullptr, Size, Size));
            _software.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            _vulkan.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            mesh = MakeMesh(Quad(false));
            tangentMesh = MakeMesh(Quad(true, 1.0f));
            mirroredTangentMesh = MakeMesh(Quad(true, -1.0f));
            lit = MakeMaterial(_vulkan.GetStandardLitShader(), _software.GetStandardLitShader());
            unlit = MakeMaterial(_vulkan.GetStandardUnlitShader(), _software.GetStandardUnlitShader());
        }

        ~LitScene() { _software.Shutdown(); }

        LitScene(const LitScene&) = delete;
        LitScene& operator=(const LitScene&) = delete;

        DualMesh MakeMesh(const MeshData& data)
        {
            DualMesh made{_vulkan.CreateMesh(data), _software.CreateMesh(data)};
            EXPECT_NE(made.vulkan, nullptr);
            EXPECT_NE(made.software, nullptr);
            return made;
        }

        DualMaterial MakeMaterial(Renderer::Common::IShader* vulkanShader, Renderer::Common::IShader* softwareShader)
        {
            IMaterial* v = _vulkan.CreateMaterial(vulkanShader);
            IMaterial* s = _software.CreateMaterial(softwareShader);
            EXPECT_NE(v, nullptr);
            EXPECT_NE(s, nullptr);
            return DualMaterial(v, s);
        }

        /// A 4-channel texture on both; `srgb` makes it an sRGB texture (TextureOptions::srgb), as GpuCache does for a lit
        /// material's colour textures in linear lighting
        DualTexture Texture(const std::vector<std::uint8_t>& rgba, const std::uint32_t width, const std::uint32_t height,
                            const bool srgb = false)
        {
            Renderer::Common::TextureOptions nearest;
            nearest.filter = Renderer::Common::TextureFilter::Nearest;
            nearest.srgb = srgb;
            DualTexture made{_vulkan.CreateTexture(rgba.data(), width, height, 4, nearest),
                             _software.CreateTexture(rgba.data(), width, height, 4, nearest)};
            EXPECT_NE(made.vulkan, nullptr);
            EXPECT_NE(made.software, nullptr);
            return made;
        }

        /// One draw on both renderers; the lit cross diff against the software frame is EXPECTed (and the measured numbers
        /// printed), the Vulkan frame returned for the test's own checks. `model`, `state`, `projection` as the software
        /// tests pass them
        Frame Draw(const SceneLightingData& lighting, DualMaterial& material, const DualMesh* which = nullptr,
                   const float* model = Identity, const RenderState& state = RenderState{},
                   const Vector3& camera = Vector3(0, 0, 100), const float* projection = Identity)
        {
            const DualMesh& drawn = which != nullptr ? *which : mesh;
            const Frame vulkan = DrawOn(_vulkan, lighting, material.vulkan, drawn.vulkan, model, state, camera, projection);
            const Frame software = DrawOn(_software, lighting, material.software, drawn.software, model, state, camera, projection);
            ++_draws;
            const Diff::Options options = Diff::Lit();
            const Diff::Result result = Diff::Compare(vulkan.AsImage(), software.AsImage(), options);
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            std::cout << "[ cross-diff ] " << info->test_suite_name() << "." << info->name() << " draw " << _draws << ": "
                      << result.Describe() << std::endl;
            EXPECT_TRUE(Diff::Agree(vulkan.AsImage(), software.AsImage(), options))
                << "draw " << _draws << " of the test";
            return vulkan;
        }

        /// The Vulkan renderer alone (for the checks that have no software counterpart)
        Frame DrawVulkanOnly(const SceneLightingData& lighting, IMaterial* material, IMesh* mesh2,
                             const Vector3& camera = Vector3(0, 0, 100))
        {
            return DrawOn(_vulkan, lighting, material, mesh2, Identity, RenderState{}, camera, Identity);
        }

        DualMesh mesh;
        DualMesh tangentMesh;         // tangent +x, w = +1
        DualMesh mirroredTangentMesh; // tangent +x, w = -1
        DualMaterial lit;
        DualMaterial unlit;

    private:
        static Frame DrawOn(Renderer::Common::IRenderer& renderer, const SceneLightingData& lighting, IMaterial* material,
                            IMesh* drawn, const float* model, const RenderState& state, const Vector3& camera,
                            const float* projection)
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, projection);
            renderer.UpdateSceneLighting(lighting, camera);
            renderer.DrawMesh(drawn, model, material, state);
            renderer.EndFrame();
            renderer.Present();
            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            return frame;
        }

        VulkanRenderer& _vulkan;
        SoftwareRenderer _software;
        int _draws = 0;
    };

    /// The fixture of the lit tests: the Vulkan fixture (validation on) with a 64x64 target, the shaders required, and the
    /// dual scene
    class VulkanLitTest : public VulkanTest
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
            ASSERT_TRUE(R().SetRenderTargetSize(Size, Size));
            _scene = std::make_unique<LitScene>(R());
        }

        void TearDown() override
        {
            _scene.reset(); // before the renderers shut down
            VulkanTest::TearDown();
        }

        LitScene& S() { return *_scene; }

    private:
        std::unique_ptr<LitScene> _scene;
    };

    // ---- Lighting helpers (the software tests' own) ------------------------------------------------------------------

    /// Ambient only, no lights, of the given grey
    inline SceneLightingData AmbientOnly(const float grey)
    {
        SceneLightingData lighting;
        lighting.ambientColor = Vector3(grey, grey, grey);
        return lighting;
    }

    /// One directional light travelling along -z (into the quad), no ambient
    inline SceneLightingData WithDirectional(const float intensity)
    {
        SceneLightingData lighting = AmbientOnly(0.0f);
        Renderer::Common::DirectionalLightData light;
        light.direction = Vector3(0.0f, 0.0f, -1.0f);
        light.intensity = intensity;
        lighting.directionalLights.push_back(light);
        return lighting;
    }

    /// One directional light lighting from `toLight` (so travelling along -toLight), no ambient
    inline SceneLightingData DirectionalFrom(const Vector3& toLight, const float intensity)
    {
        SceneLightingData lighting = AmbientOnly(0.0f);
        Renderer::Common::DirectionalLightData light;
        light.direction = Vector3(-toLight.x, -toLight.y, -toLight.z);
        light.intensity = intensity;
        lighting.directionalLights.push_back(light);
        return lighting;
    }
}
