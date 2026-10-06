#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "GltfTestBuilder.hpp"
#include "TextureTestSupport.hpp"

// Shared by the Model asset, instantiation, golden and Lua model tests: small glTF models built in memory
namespace ModelTestSupport
{
    using GltfTest::Builder;

    /// A 2 x 2 BMP (red | green over blue | yellow), embedded as the model's image (the decoder reads it by content)
    inline std::vector<std::uint8_t> CheckerBmp()
    {
        using TextureTestSupport::Rgb;
        return TextureTestSupport::MakeBmp(2, 2, std::vector<Rgb>{Rgb{255, 0, 0}, Rgb{0, 255, 0}, Rgb{0, 0, 255},
                                                                  Rgb{255, 255, 0}});
    }

    /**
     * "robot": two meshes, "Body" (one triangle: 3 vertices once normals are generated flat) and "Arm" (a quad: 6),
     * in that order or reversed (a re-export that reorders them); materials "Red" (unlit, textured with the
     * embedded image "Albedo") and "Green" (unlit); nodes "Robot" (Body, at (0, 1, 0)) with the child "Arm" (Arm, at
     * (1, 0, 0), scale 2, turned 90 degrees about y).
     */
    inline Builder Robot(const bool reversed = false)
    {
        Builder b;
        const int image = b.AddImage("Albedo", CheckerBmp(), "image/bmp");
        const int texture = b.AddTexture(image, true, true);
        nlohmann::json red;
        red["name"] = "Red";
        red["pbrMetallicRoughness"] = {{"baseColorFactor", {1.0, 0.0, 0.0, 1.0}}, {"baseColorTexture", {{"index", texture}}}};
        red["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        const int redMaterial = b.AddMaterial(red);
        b.UseExtension("KHR_materials_unlit");
        const int greenMaterial = b.AddUnlitMaterial("Green", 0.0f, 1.0f, 0.0f);

        const auto addBody = [&b, redMaterial]
        {
            const int position = b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3);
            const int uv = b.AddFloats({0, 1, 1, 1, 0, 0}, 2);
            return b.AddMesh("Body", {Builder::Primitive(position, -1, redMaterial, -1, uv)});
        };
        const auto addArm = [&b, greenMaterial]
        {
            const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
            const int indices = b.AddIndicesU16({0, 1, 2, 0, 2, 3});
            return b.AddMesh("Arm", {Builder::Primitive(position, indices, greenMaterial)});
        };
        int body = -1;
        int arm = -1;
        if (reversed)
        {
            arm = addArm();
            body = addBody();
        }
        else
        {
            body = addBody();
            arm = addArm();
        }
        const float s = 0.70710678f;
        const int armNode = b.AddNode("Arm", {{"mesh", arm}, {"translation", {1, 0, 0}}, {"scale", {2, 2, 2}},
                                              {"rotation", {0, s, 0, s}}});
        const int robotNode = b.AddNode("Robot", {{"mesh", body}, {"translation", {0, 1, 0}}, {"children", {armNode}}});
        b.SetScene({robotNode});
        return b;
    }
}
