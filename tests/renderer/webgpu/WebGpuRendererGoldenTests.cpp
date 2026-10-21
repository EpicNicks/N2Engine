#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "common/CrossBackendDiff.hpp"
#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"

// Golden-image tests of the WebGPU backend's unlit drawing (#155, stage G3a): the scenes of the Vulkan stage B goldens
// (tests/renderer/vulkan/VulkanUnlitDrawTests.cpp, VulkanMeshGoldenTests.cpp) drawn straight through the IRenderer interface,
// each on the WebGPU renderer (Dawn over Mesa lavapipe in CI) and on a headless SOFTWARE renderer, and the two frames compared
// pixel by pixel with the cross-backend diff the Vulkan tests use (tests/renderer/common/CrossBackendDiff.hpp). The software
// renderer is the reference. A scene is one function of an IRenderer, so both renderers run the same calls.
//
// Tolerances, per scene kind (each is the least that holds, and is printed with the numbers in the test log as "[ cross-diff ]"):
//   - EXACT (0/255, every pixel, nothing left out): axis-aligned quads on pixel boundaries with flat colours or nearest-sampled
//     textures. The software renderer is exact there and so is the GPU; any difference is a bug. Flat colours, depth order,
//     index ranges, the alpha cutoff and nearest textures of 1 to 4 channels are in this class.
//   - STRICT with edges (the default 2/255, the pixels within one pixel of any colour change in either image left out): scenes
//     with triangle edges that are not on pixel boundaries (cull modes, a rotated and projected cube). The rasterisation tie
//     rules differ between backends, so the pixels ON an edge may fall on either side; everything else is held to 2/255.
//   - LIT (mean error <= 2/255, <= 1% of the pixels over 8/255): linearly filtered textures and interpolated vertex colours.
//     The GPU filters with fixed-point weights, the software renderer in floating point.
// The software renderer has no mipmaps, so a minified mipmapped texture is checked against a CPU reference of the level the
// sampler's LOD picks instead (the last test).
//
// "Compare also against the Vulkan goldens": the Vulkan renderer is compiled out of the webgpu CI job, so no live WebGPU versus
// Vulkan frame diff runs here. The Vulkan goldens are structural (counts, colours, positions), and WebGpuRendererUnlitDrawTests.cpp
// asserts the same values scene by scene; the cross-diff against the software renderer is the same one the Vulkan goldens use.
//
// A sRGB texture is the one place the backends differ by design: the GPU decodes before filtering, the software renderer after.
// With nearest sampling the two agree (one texel), which is what the sRGB scene uses.

using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::IRenderer;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Software::SoftwareRenderer;
namespace Diff = VulkanTesting::Diff;

namespace
{
    using Scene = std::function<void(IRenderer&)>;

    TextureOptions Options(TextureFilter filter, TextureWrap wrap, bool mipmaps = false, bool srgb = false)
    {
        TextureOptions options;
        options.filter = filter;
        options.wrap = wrap;
        options.mipmaps = mipmaps;
        options.srgb = srgb;
        return options;
    }

    IMaterial* Flat(IRenderer& renderer, float r, float g, float b, float a = 1.0f)
    {
        IMaterial* material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
        material->SetColor("uAlbedo", r, g, b, a);
        return material;
    }

    /// One frame on `renderer`: the clear colour, then the scene (which makes its resources and draws between BeginFrame and
    /// EndFrame itself), then the frame read back, bottom row first
    Diff::Image RenderOn(IRenderer& renderer, const Scene& scene)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        scene(renderer);
        renderer.Present();
        Diff::Image image;
        image.width = Size;
        image.height = Size;
        image.rgba.assign(static_cast<std::size_t>(Size) * Size * 4, 0);
        renderer.ReadFramebuffer(image.rgba.data(), Size, Size);
        return image;
    }

    /// The options of the three classes above
    Diff::Options Exact()
    {
        Diff::Options options;
        options.tolerance = 0;
        return options;
    }

    Diff::Options StrictWithEdges()
    {
        Diff::Options options; // 2/255
        options.edgeBorder = 1;
        options.coverageEdges = false; // every colour change counts as an edge, face to face as well as against the background
        options.maxExcludedFraction = 0.35;
        return options;
    }

    Diff::Options Lit()
    {
        return Diff::Lit({0, 0, 0});
    }

    /// Row-major 4x4 products and transforms, as the engine has them (translation in the last column)
    using Matrix = std::array<float, 16>;

    Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix out{};
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    sum += a[row * 4 + k] * b[k * 4 + column];
                }
                out[row * 4 + column] = sum;
            }
        }
        return out;
    }

    Matrix RotationY(float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
    }

    Matrix RotationX(float angle)
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        return {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1};
    }

    Matrix Perspective(float verticalFovRadians, float aspect, float nearPlane, float farPlane)
    {
        const float f = 1.0f / std::tan(verticalFovRadians / 2.0f);
        return {f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (farPlane + nearPlane) / (nearPlane - farPlane),
                2.0f * farPlane * nearPlane / (nearPlane - farPlane), 0, 0, -1, 0};
    }

    /// A cube of side 2 centred on the origin, a different flat colour per face, wound counter-clockwise seen from outside
    MeshData Cube()
    {
        struct Face
        {
            float u[3], v[3], n[3], colour[3];
        };
        // u x v = n, so (c - u - v), (c + u - v), (c + u + v), (c - u + v) is counter-clockwise seen from the n side
        const Face faces[6] = {
            {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {1.0f, 0.2f, 0.2f}},   // +X
            {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}, {0.2f, 1.0f, 0.2f}},  // -X
            {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {0.2f, 0.2f, 1.0f}},   // +Y
            {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {1.0f, 1.0f, 0.2f}},  // -Y
            {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1.0f, 0.2f, 1.0f}},   // +Z
            {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, {0.2f, 1.0f, 1.0f}},  // -Z
        };
        MeshData data;
        for (const Face& face : faces)
        {
            const std::uint32_t base = static_cast<std::uint32_t>(data.vertices.size());
            const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int corner = 0; corner < 4; ++corner)
            {
                Vertex vertex{};
                for (int axis = 0; axis < 3; ++axis)
                {
                    vertex.position[axis] = face.n[axis] + face.u[axis] * signs[corner][0] + face.v[axis] * signs[corner][1];
                    vertex.normal[axis] = face.n[axis];
                    vertex.color[axis] = face.colour[axis];
                }
                vertex.color[3] = 1.0f;
                vertex.texCoord[0] = signs[corner][0] > 0 ? 1.0f : 0.0f;
                vertex.texCoord[1] = signs[corner][1] > 0 ? 1.0f : 0.0f;
                data.vertices.push_back(vertex);
            }
            for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
            {
                data.indices.push_back(base + index);
            }
        }
        return data;
    }

    /// A 4 x 4 RGBA texture whose 16 texels are all different and none is black
    std::vector<std::uint8_t> Texels4x4()
    {
        std::vector<std::uint8_t> texels;
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                texels.push_back(static_cast<std::uint8_t>(x * 80 + 15));
                texels.push_back(static_cast<std::uint8_t>(y * 80 + 15));
                texels.push_back(static_cast<std::uint8_t>(255 - x * 50 - y * 20));
                texels.push_back(255);
            }
        }
        return texels;
    }

    /// The next mip level the way the renderer builds it (see WebGpuResources.cpp): the equal-weight average of the texels each
    /// destination texel covers, rounded half up (the 2x2 box for the halvings used here)
    std::vector<std::uint8_t> Halve(const std::vector<std::uint8_t>& source, std::uint32_t width, std::uint32_t height,
                                    std::uint32_t texelBytes)
    {
        const std::uint32_t halfWidth = std::max(1u, width / 2);
        const std::uint32_t halfHeight = std::max(1u, height / 2);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(halfWidth) * halfHeight * texelBytes);
        for (std::uint32_t y = 0; y < halfHeight; ++y)
        {
            for (std::uint32_t x = 0; x < halfWidth; ++x)
            {
                for (std::uint32_t c = 0; c < texelBytes; ++c)
                {
                    const auto at = [&](std::uint32_t sx, std::uint32_t sy)
                    { return static_cast<std::uint32_t>(source[(static_cast<std::size_t>(sy) * width + sx) * texelBytes + c]); };
                    const std::uint32_t sum = at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1);
                    out[(static_cast<std::size_t>(y) * halfWidth + x) * texelBytes + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        return out;
    }

    class WebGpuGoldenTest : public WebGpuDrawTest
    {
    protected:
        /// Renders `scene` on WebGPU and on a fresh headless software renderer and expects the two to agree under `options`
        void ExpectSameAsSoftware(const std::string& what, const Scene& scene, const Diff::Options& options)
        {
            const Diff::Image webgpu = RenderOn(R(), scene);
            SoftwareRenderer software;
            ASSERT_TRUE(software.Initialize(nullptr, Size, Size));
            const Diff::Image reference = RenderOn(software, scene);
            software.Shutdown();

            // A scene that drew nothing agrees with itself: both frames must hold something other than the clear colour
            const auto drawn = [](const Diff::Image& image)
            {
                for (std::size_t i = 0; i < image.rgba.size(); i += 4)
                {
                    if (image.rgba[i] != 0 || image.rgba[i + 1] != 0 || image.rgba[i + 2] != 0)
                    {
                        return true;
                    }
                }
                return false;
            };
            EXPECT_TRUE(drawn(webgpu)) << what << ": the WebGPU frame is empty";
            EXPECT_TRUE(drawn(reference)) << what << ": the software frame is empty";

            const Diff::Result result = Diff::Compare(webgpu, reference, options);
            std::cout << "[ cross-diff ] " << what << ": " << result.Describe() << std::endl;
            EXPECT_TRUE(Diff::Agree(webgpu, reference, options)) << what;
        }
    };
}

// ============================================================================
// Exact: pixel-aligned quads
// ============================================================================

TEST_F(WebGpuGoldenTest, FlatColourQuadsAgreeWithTheSoftwareRendererExactly)
{
    ExpectSameAsSoftware("flat quads", [](IRenderer& r)
    {
        IMesh* big = r.CreateMesh(Quad(-0.75f, -0.75f, 0.25f, 0.25f, 0.0f));
        IMesh* small = r.CreateMesh(Quad(-0.25f, -0.25f, 0.75f, 0.5f, -0.5f));
        IMesh* strip = r.CreateMesh(Quad(-1.0f, 0.5f, 1.0f, 1.0f, 0.5f));
        IMaterial* red = Flat(r, 1.0f, 0.0f, 0.0f);
        IMaterial* green = Flat(r, 0.0f, 1.0f, 0.0f);
        IMaterial* blue = Flat(r, 0.0f, 0.0f, 1.0f);
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(strip, Identity, blue);
        r.DrawMesh(big, Identity, red);
        r.DrawMesh(small, Identity, green);
        r.EndFrame();
    }, Exact());
}

TEST_F(WebGpuGoldenTest, DepthOrderTestAndWriteAgreeWithTheSoftwareRendererExactly)
{
    ExpectSameAsSoftware("depth order", [](IRenderer& r)
    {
        // z -0.5 is nearer than z 0.5 (GL). One pair of quads in each quadrant, each with a different state
        const auto quad = [&](float x0, float y0, float x1, float y1, float z) { return r.CreateMesh(Quad(x0, y0, x1, y1, z)); };
        IMesh* upperLeftNear = quad(-1.0f, 0.0f, 0.0f, 1.0f, -0.5f);
        IMesh* upperLeftFar = quad(-1.0f, 0.0f, 0.0f, 1.0f, 0.5f);
        IMesh* upperRightNear = quad(0.0f, 0.0f, 1.0f, 1.0f, -0.5f);
        IMesh* upperRightFar = quad(0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
        IMesh* lowerLeftNear = quad(-1.0f, -1.0f, 0.0f, 0.0f, -0.5f);
        IMesh* lowerLeftFar = quad(-1.0f, -1.0f, 0.0f, 0.0f, 0.5f);
        IMesh* lowerRightNear = quad(0.0f, -1.0f, 1.0f, 0.0f, -0.5f);
        IMesh* lowerRightFar = quad(0.0f, -1.0f, 1.0f, 0.0f, 0.5f);
        IMaterial* red = Flat(r, 1.0f, 0.0f, 0.0f);
        IMaterial* green = Flat(r, 0.0f, 1.0f, 0.0f);
        RenderState noTest;
        noTest.depthTest = false;
        RenderState noWrite;
        noWrite.depthWrite = false;
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(upperLeftFar, Identity, green);   // far first, then near: near (red)
        r.DrawMesh(upperLeftNear, Identity, red);
        r.DrawMesh(upperRightNear, Identity, red);   // near first, then far: near (red)
        r.DrawMesh(upperRightFar, Identity, green);
        r.DrawMesh(lowerLeftNear, Identity, red, noTest); // no test: the later one (green)
        r.DrawMesh(lowerLeftFar, Identity, green, noTest);
        r.DrawMesh(lowerRightNear, Identity, red, noWrite); // near wrote no depth: the far one shows (green)
        r.DrawMesh(lowerRightFar, Identity, green);
        r.EndFrame();
    }, Exact());
}

TEST_F(WebGpuGoldenTest, IndexRangesAgreeWithTheSoftwareRendererExactly)
{
    ExpectSameAsSoftware("index ranges", [](IRenderer& r)
    {
        IMesh* mesh = r.CreateMesh(TwoHalves());
        IMaterial* red = Flat(r, 1.0f, 0.0f, 0.0f);
        IMaterial* green = Flat(r, 0.0f, 1.0f, 0.0f);
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(mesh, Identity, red, RenderState{}, IndexRange{0, 6});
        r.DrawMesh(mesh, Identity, green, RenderState{}, IndexRange{6, 6});
        r.EndFrame();
    }, Exact());
}

TEST_F(WebGpuGoldenTest, ANearestTexturedQuadAgreesWithTheSoftwareRendererExactlyForOneToFourChannels)
{
    for (std::uint32_t channels = 1; channels <= 4; ++channels)
    {
        const std::vector<std::uint8_t> rgba = Texels4x4();
        std::vector<std::uint8_t> texels;
        for (std::size_t i = 0; i < 16; ++i)
        {
            for (std::uint32_t c = 0; c < channels; ++c)
            {
                texels.push_back(rgba[i * 4 + c]);
            }
        }
        ExpectSameAsSoftware("nearest texture, " + std::to_string(channels) + " channels", [&](IRenderer& r)
        {
            // 4 x 4 texels over 32 x 32 pixels: 8 pixels per texel, every pixel centre well inside its texel
            ITexture* texture = r.CreateTexture(texels.data(), 4, 4, channels, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge));
            IMaterial* material = r.CreateMaterial(r.GetStandardUnlitShader(), texture);
            IMesh* quad = r.CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
            r.BeginFrame();
            r.SetViewProjection(Identity, Identity);
            r.DrawMesh(quad, Identity, material);
            r.EndFrame();
        }, Exact());
    }
}

TEST_F(WebGpuGoldenTest, ANearestRepeatedTextureAgreesWithTheSoftwareRendererExactly)
{
    ExpectSameAsSoftware("nearest repeat", [](IRenderer& r)
    {
        // uv 0..3 across: three repeats of the 4 x 4 texture in 48 pixels, 4 pixels per texel
        const std::vector<std::uint8_t> texels = Texels4x4();
        ITexture* texture = r.CreateTexture(texels.data(), 4, 4, 4, Options(TextureFilter::Nearest, TextureWrap::Repeat));
        IMaterial* material = r.CreateMaterial(r.GetStandardUnlitShader(), texture);
        MeshData data = Quad(-0.75f, -0.75f, 0.75f, 0.75f, 0.0f);
        for (Vertex& vertex : data.vertices)
        {
            vertex.texCoord[0] *= 3.0f;
            vertex.texCoord[1] *= 3.0f;
        }
        IMesh* quad = r.CreateMesh(data);
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(quad, Identity, material);
        r.EndFrame();
    }, Exact());
}

TEST_F(WebGpuGoldenTest, TheAlphaCutoffAndTheTextureTimesAlbedoTimesVertexColourAgreeWithTheSoftwareRendererExactly)
{
    ExpectSameAsSoftware("alpha cutoff and colour product", [](IRenderer& r)
    {
        // Left texel transparent blue, right texel opaque yellow: cut out at 0.5, the red backdrop shows through
        const std::uint8_t texels[8] = {0, 0, 255, 0, 255, 255, 0, 255};
        ITexture* texture = r.CreateTexture(texels, 2, 1, 4, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge));
        IMaterial* cutout = r.CreateMaterial(r.GetStandardUnlitShader(), texture);
        cutout->SetFloat("uAlphaCutoff", 0.5f);
        cutout->SetColor("uAlbedo", 0.0f, 1.0f, 1.0f, 1.0f); // yellow x cyan = green
        IMaterial* red = Flat(r, 1.0f, 0.0f, 0.0f);
        IMesh* front = r.CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f)); // vertex colour (1, 1, 0.5)
        IMesh* back = r.CreateMesh(Quad(-0.75f, -0.75f, 0.75f, 0.75f, 0.5f));
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(front, Identity, cutout);
        r.DrawMesh(back, Identity, red);
        r.EndFrame();
    }, Exact());
}

TEST_F(WebGpuGoldenTest, ANearestSrgbTextureAgreesWithTheSoftwareRendererWithinTheStrictTolerance)
{
    // The GPU decodes an sRGB texel when it samples (RGBA8UnormSrgb), the software renderer through a table after filtering:
    // one texel per pixel (nearest) is the case where they must agree, to the strict 2/255
    Diff::Options strict; // 2/255, nothing left out
    ExpectSameAsSoftware("nearest sRGB texture", [](IRenderer& r)
    {
        const std::vector<std::uint8_t> texels = Texels4x4();
        ITexture* texture = r.CreateTexture(texels.data(), 4, 4, 4, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge, false, true));
        IMaterial* material = r.CreateMaterial(r.GetStandardUnlitShader(), texture);
        IMesh* quad = r.CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(quad, Identity, material);
        r.EndFrame();
    }, strict);
}

// ============================================================================
// Strict with edges: triangles and projection
// ============================================================================

TEST_F(WebGpuGoldenTest, EveryCullModeAgreesWithTheSoftwareRenderer)
{
    ExpectSameAsSoftware("cull modes", [](IRenderer& r)
    {
        // A 3 x 2 grid of cells: columns Back, Front, None; the top row counter-clockwise triangles, the bottom row clockwise.
        // Each is the 1.6 wide triangle of the Triangle() helper scaled into its cell
        IMesh* ccw = r.CreateMesh(Triangle(true));
        IMesh* cw = r.CreateMesh(Triangle(false));
        IMaterial* red = Flat(r, 1.0f, 0.0f, 0.0f);
        IMaterial* green = Flat(r, 0.0f, 1.0f, 0.0f);
        const CullMode modes[3] = {CullMode::Back, CullMode::Front, CullMode::None};
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        for (int column = 0; column < 3; ++column)
        {
            for (int row = 0; row < 2; ++row)
            {
                // scale 1/3.2 in x and 1/2.4 in y, then to the cell centre
                const float centreX = -2.0f / 3.0f + static_cast<float>(column) * (2.0f / 3.0f);
                const float centreY = row == 0 ? 0.5f : -0.5f;
                const float model[16] = {0.3f, 0, 0, centreX, 0, 0.4f, 0, centreY, 0, 0, 1, 0, 0, 0, 0, 1};
                RenderState state;
                state.cull = modes[column];
                r.DrawMesh(row == 0 ? ccw : cw, model, row == 0 ? red : green, state);
            }
        }
        r.EndFrame();
    }, StrictWithEdges());
}

TEST_F(WebGpuGoldenTest, ARotatedProjectedCubeAgreesWithTheSoftwareRendererAwayFromItsEdges)
{
    ExpectSameAsSoftware("projected cube", [](IRenderer& r)
    {
        IMesh* cube = r.CreateMesh(Cube());
        IMaterial* white = Flat(r, 1.0f, 1.0f, 1.0f);
        // model: Ry(0.6) * Rx(0.45); view: the camera 5 units back; projection: GL perspective, 45 degrees, square
        const Matrix model = Multiply(RotationY(0.6f), RotationX(0.45f));
        const Matrix view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -5, 0, 0, 0, 1};
        const Matrix projection = Perspective(0.7853982f, 1.0f, 0.1f, 50.0f);
        r.BeginFrame();
        r.SetViewProjection(view.data(), projection.data());
        r.DrawMesh(cube, model.data(), white);
        r.EndFrame();
    }, StrictWithEdges());
}

TEST_F(WebGpuGoldenTest, TwoCubesWithTheDepthTestAgreeWithTheSoftwareRendererAwayFromTheirEdges)
{
    ExpectSameAsSoftware("two cubes", [](IRenderer& r)
    {
        IMesh* cube = r.CreateMesh(Cube());
        IMaterial* white = Flat(r, 1.0f, 1.0f, 1.0f);
        IMaterial* tint = Flat(r, 1.0f, 0.5f, 0.75f);
        const Matrix view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -7, 0, 0, 0, 1};
        const Matrix projection = Perspective(0.7853982f, 1.0f, 0.1f, 50.0f);
        // Two overlapping cubes, the farther one drawn last: the depth test decides, not the order
        Matrix nearCube = Multiply(RotationY(0.3f), RotationX(0.2f));
        nearCube[3] = -0.8f;
        nearCube[7] = 0.3f;
        nearCube[11] = 1.0f;
        Matrix farCube = Multiply(RotationY(-0.5f), RotationX(0.6f));
        farCube[3] = 0.9f;
        farCube[7] = -0.2f;
        farCube[11] = -1.5f;
        r.BeginFrame();
        r.SetViewProjection(view.data(), projection.data());
        r.DrawMesh(cube, nearCube.data(), white);
        r.DrawMesh(cube, farCube.data(), tint);
        r.EndFrame();
    }, StrictWithEdges());
}

// ============================================================================
// Lit class: filtering and interpolation
// ============================================================================

TEST_F(WebGpuGoldenTest, ALinearlyFilteredMagnifiedTextureAgreesWithTheSoftwareRendererInTheLitClass)
{
    ExpectSameAsSoftware("linear texture, clamp", [](IRenderer& r)
    {
        // 4 x 4 texels magnified 16 times over the frame (no mipmaps: the software renderer has none)
        const std::vector<std::uint8_t> texels = Texels4x4();
        ITexture* texture = r.CreateTexture(texels.data(), 4, 4, 4, Options(TextureFilter::Linear, TextureWrap::ClampToEdge));
        IMaterial* material = r.CreateMaterial(r.GetStandardUnlitShader(), texture);
        IMesh* quad = r.CreateMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(quad, Identity, material);
        r.EndFrame();
    }, Lit());
}

TEST_F(WebGpuGoldenTest, ALinearlyFilteredRepeatedTextureAgreesWithTheSoftwareRendererInTheLitClass)
{
    ExpectSameAsSoftware("linear texture, repeat", [](IRenderer& r)
    {
        // uv 0..2.5: the wrap at the seams is part of the picture
        const std::vector<std::uint8_t> texels = Texels4x4();
        ITexture* texture = r.CreateTexture(texels.data(), 4, 4, 4, Options(TextureFilter::Linear, TextureWrap::Repeat));
        IMaterial* material = r.CreateMaterial(r.GetStandardUnlitShader(), texture);
        MeshData data = Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
        for (Vertex& vertex : data.vertices)
        {
            vertex.texCoord[0] *= 2.5f;
            vertex.texCoord[1] *= 2.5f;
        }
        IMesh* quad = r.CreateMesh(data);
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(quad, Identity, material);
        r.EndFrame();
    }, Lit());
}

TEST_F(WebGpuGoldenTest, InterpolatedVertexColoursAgreeWithTheSoftwareRendererInTheLitClass)
{
    ExpectSameAsSoftware("vertex colour gradient", [](IRenderer& r)
    {
        MeshData data = Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
        const float corners[4][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f}};
        for (int i = 0; i < 4; ++i)
        {
            data.vertices[static_cast<std::size_t>(i)].color[0] = corners[i][0];
            data.vertices[static_cast<std::size_t>(i)].color[1] = corners[i][1];
            data.vertices[static_cast<std::size_t>(i)].color[2] = corners[i][2];
        }
        IMesh* quad = r.CreateMesh(data);
        IMaterial* white = Flat(r, 1.0f, 1.0f, 1.0f);
        r.BeginFrame();
        r.SetViewProjection(Identity, Identity);
        r.DrawMesh(quad, Identity, white);
        r.EndFrame();
    }, Lit());
}

// ============================================================================
// WebGPU only: the CPU-generated mip chain, as the sampler uses it
// ============================================================================

TEST_F(WebGpuGoldenTest, AMinifiedMipmappedTextureShowsTheCpuGeneratedLevelTheLodSelects)
{
    // 16 x 16 texels over exactly 8 x 8 pixels: two texels per pixel, LOD 1. With a nearest sampler (nearest mip filter) every
    // LOD from 0.5 to 1.5 picks level 1, so each pixel is one texel of level 1 (8 x 8), which is the CPU box filter of level 0.
    // The software renderer has no mipmaps, so the reference is computed here
    std::vector<std::uint8_t> level0;
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 16; ++x)
        {
            level0.push_back(static_cast<std::uint8_t>(x * 15 + 10));
            level0.push_back(static_cast<std::uint8_t>(y * 15 + 10));
            level0.push_back(static_cast<std::uint8_t>((x * 7 + y * 13) % 200 + 20));
            level0.push_back(255);
        }
    }
    const std::vector<std::uint8_t> level1 = Halve(level0, 16, 16, 4);
    ITexture* texture = R().CreateTexture(level0.data(), 16, 16, 4, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge, true));
    ASSERT_NE(texture, nullptr);
    IMaterial* material = R().CreateMaterial(_unlit, texture);
    IMesh* quad = MakeMesh(Quad(-0.125f, -0.125f, 0.125f, 0.125f, 0.0f)); // pixels 28..35
    ASSERT_NE(material, nullptr);
    ASSERT_NE(quad, nullptr);

    const Frame frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, material); });
    int wrong = 0;
    for (int j = 0; j < 8; ++j)
    {
        for (int i = 0; i < 8; ++i)
        {
            const Rgb expected{level1[(static_cast<std::size_t>(j) * 8 + i) * 4], level1[(static_cast<std::size_t>(j) * 8 + i) * 4 + 1],
                               level1[(static_cast<std::size_t>(j) * 8 + i) * 4 + 2]};
            if (!(frame.At(28 + i, 28 + j) == expected))
            {
                ++wrong;
            }
        }
    }
    EXPECT_EQ(wrong, 0) << "of the 64 pixels, the ones that are not the level-1 texel of the CPU chain";

    // The same texture without mipmaps shows level 0 (no level to pick): one pixel per second texel, sampled at the centres
    ITexture* flat = R().CreateTexture(level0.data(), 16, 16, 4, Options(TextureFilter::Nearest, TextureWrap::ClampToEdge, false));
    ASSERT_NE(flat, nullptr);
    IMaterial* flatMaterial = R().CreateMaterial(_unlit, flat);
    ASSERT_NE(flatMaterial, nullptr);
    const Frame unmipped = RenderFrameOf([&] { R().DrawMesh(quad, Identity, flatMaterial); });
    int matchesLevel1 = 0;
    for (int j = 0; j < 8; ++j)
    {
        for (int i = 0; i < 8; ++i)
        {
            const std::size_t k = (static_cast<std::size_t>(j) * 8 + i) * 4;
            matchesLevel1 += unmipped.At(28 + i, 28 + j) == Rgb{level1[k], level1[k + 1], level1[k + 2]} ? 1 : 0;
        }
    }
    EXPECT_LT(matchesLevel1, 64) << "without a mip chain the frame is not the level-1 picture";
}
