#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Texture.hpp"

#include "WebGpuTestFixture.hpp"

// User shaders, stage 5a, acceptance on WebGPU (orchestrator/proposals/user-shaders.md section 13 item 3): the same test as
// tests/renderer/vulkan/VulkanUserShaderMatGoldenTests.cpp with the same fixtures and the same expected colours, on Dawn over Mesa
// lavapipe: a `.mat` file in a project names a `.n2shader` and the values of its parameters, the engine loads both, finds the cooked
// payload under .import/shaders/<uuid>/, and Scene::Render draws a quad with it: pixels, not a renderer-level call. The fixture fails any
// test on an uncaptured WebGPU error.

using namespace N2Engine;
using Rendering::BuiltinMesh;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Rendering::Shader;
using Rendering::Texture;
using WebGpuTesting::WebGpuTest;
namespace fs = std::filesystem;

namespace
{
    constexpr int Size = 64;

    fs::path FixtureDir() { return fs::path(std::string(N2_RENDERER_TEST_DIR) + "/fixtures/user-shaders"); }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb&) const = default;
    };

    class WebGpuUserShaderMatTest : public WebGpuTest
    {
    protected:
        static inline fs::path s_root;

        static void WriteText(const std::string& relative, const std::string& text)
        {
            const fs::path path = s_root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        void SetUp() override
        {
            WebGpuTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            ASSERT_TRUE(R().SetRenderTargetSize(Size, Size));

            s_root = fs::temp_directory_path() / "n2engine_webgpu_usershader_mat";
            std::error_code ec;
            fs::remove_all(s_root, ec);
            fs::create_directories(s_root / "assets");
            WriteText("shaders/solid.n2shader", ReadFile(FixtureDir() / "solid.n2shader"));
            WriteText("shaders/textured.n2shader", ReadFile(FixtureDir() / "textured.n2shader"));
            WriteText("materials/default.mat", R"({"shader": "../shaders/solid.n2shader"})");
            WriteText("materials/green.mat", R"({
                "shader": "../shaders/solid.n2shader",
                "params": { "u_Color": [0, 1, 0, 1], "u_Offset": [0, 0, 1] }
            })");
            WriteText("materials/inverted.mat", R"({
                "shader": "../shaders/solid.n2shader",
                "params": { "u_Flag": true }
            })");
            WriteText("materials/textured.mat", R"({
                "shader": "../shaders/textured.n2shader",
                "params": { "u_Tint": [1, 0, 0, 1] }
            })");
            IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "WebGpuUserShaderMatTest"));
            IO::ResourceLoader::Instance().ClearCache();
            IO::ResourceLoader::Instance().Initialize(s_root);
        }

        void TearDown() override
        {
            IO::ResourceLoader::Instance().ClearCache();
            std::error_code ec;
            fs::remove_all(s_root, ec);
            WebGpuTest::TearDown();
        }

        /// The material of a .mat of the project, its shader cooked where the engine looks for the payload
        static std::shared_ptr<Material> LoadMat(const char* resPath, const char* fixture)
        {
            auto material = IO::Resources::Instance().Load<Material>(fs::path(resPath));
            EXPECT_NE(material, nullptr) << resPath;
            if (material && material->GetShader())
            {
                const fs::path destination =
                    IO::ResourceLoader::Instance().GetMetadataRoot() / "shaders" / material->GetShader()->GetUUID().ToString();
                fs::create_directories(destination);
                fs::copy(FixtureDir() / fixture, destination, fs::copy_options::overwrite_existing | fs::copy_options::recursive);
            }
            return material;
        }

        /// A frame of Scene::Render with `material` on the built-in quad, read back (row 0 at the bottom)
        std::vector<std::uint8_t> RenderQuad(const std::shared_ptr<Material>& material)
        {
            R().Clear(0.0f, 0.0f, 0.0f, 1.0f);
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Size) * Size * 4);
            Camera camera;
            camera.SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
            camera.SetPosition(Math::Vector3(0.0f, 0.0f, 2.0f));
            camera.LookAt(Math::Vector3(0.0f, 0.0f, 0.0f));
            {
                auto scene = Scene::Create("WebGpuUserShaderMat");
                auto gameObject = GameObject::Create("Quad");
                auto* meshRenderer = gameObject->AddComponent<MeshRenderer>();
                meshRenderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Quad));
                meshRenderer->SetMaterial(0, material);
                scene->AddRootGameObject(gameObject);
                R().BeginFrame();
                R().SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
                scene->Render(&R(), camera);
                R().EndFrame();
                R().Present();
                R().ReadFramebuffer(pixels.data(), Size, Size);
                scene.reset(); // the scene's GPU resources go before the renderer
            }
            return pixels;
        }

        static Rgb Centre(const std::vector<std::uint8_t>& pixels)
        {
            const std::size_t i = (static_cast<std::size_t>(Size / 2) * Size + Size / 2) * 4;
            return Rgb{pixels[i], pixels[i + 1], pixels[i + 2]};
        }
    };
}

TEST_F(WebGpuUserShaderMatTest, AMatFileDrawsWithItsShaderAndItsParameterValues)
{
    // The shader's defaults: u_Color red, gain 1
    EXPECT_EQ(Centre(RenderQuad(LoadMat("res://materials/default.mat", "solid"))), (Rgb{255, 0, 0}));
    // The .mat's values: green plus the blue offset is cyan (the std140 packing of the vec3 and the bool reaches the shader)
    EXPECT_EQ(Centre(RenderQuad(LoadMat("res://materials/green.mat", "solid"))), (Rgb{0, 255, 255}));
    // A bool parameter set to true inverts the default red
    EXPECT_EQ(Centre(RenderQuad(LoadMat("res://materials/inverted.mat", "solid"))), (Rgb{0, 255, 255}));
}

TEST_F(WebGpuUserShaderMatTest, ASetterCalledEveryFrameChangesThePixelsWithoutMakingANewGpuMaterial)
{
    const auto material = LoadMat("res://materials/default.mat", "solid");
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(Centre(RenderQuad(material)), (Rgb{255, 0, 0}));
    const std::uint64_t gpuVersion = material->GetGpuVersion();
    for (int frame = 0; frame < 4; ++frame)
    {
        const float level = static_cast<float>(frame) / 3.0f;
        ASSERT_TRUE(material->SetColor("u_Color", Common::Color(0.0f, level, 1.0f - level, 1.0f)));
        const Rgb centre = Centre(RenderQuad(material));
        EXPECT_NEAR(centre.g, level * 255.0f, 1.5f) << "frame " << frame;
        EXPECT_NEAR(centre.b, (1.0f - level) * 255.0f, 1.5f) << "frame " << frame;
    }
    EXPECT_EQ(material->GetGpuVersion(), gpuVersion) << "a value is a uniform: nothing is remade";
}

TEST_F(WebGpuUserShaderMatTest, ATextureParameterSetAtRuntimeReachesItsSlotAndTheMaterialIsRemade)
{
    const auto material = LoadMat("res://materials/textured.mat", "textured");
    ASSERT_NE(material, nullptr);
    // No texture: u_Tex reads white, so the tint (red) shows
    EXPECT_EQ(Centre(RenderQuad(material)), (Rgb{255, 0, 0}));
    const std::uint64_t gpuVersion = material->GetGpuVersion();
    ASSERT_TRUE(material->SetTexture("u_Tex", Texture::Create(1, 1, std::vector<std::uint8_t>{0, 255, 0, 255})));
    EXPECT_GT(material->GetGpuVersion(), gpuVersion) << "a texture is part of the GPU material";
    // The green texture times the red tint is black; the second texture slot (u_Add) is black by default: nothing added
    EXPECT_EQ(Centre(RenderQuad(material)), (Rgb{0, 0, 0}));
    ASSERT_TRUE(material->SetColor("u_Tint", Common::Color(1.0f, 1.0f, 1.0f, 1.0f)));
    EXPECT_EQ(Centre(RenderQuad(material)), (Rgb{0, 255, 0}));
}
