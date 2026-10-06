#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// Builds glTF 2.0 files in memory for the tests (#3 P3), so no third-party model is ever needed: a JSON document
// plus one binary buffer, written out as a .gltf with a base64 data: URI, a .gltf with an external .bin, or a .glb.
// Shared by tests/assetimport (the importer) and tests/engine (the Model asset); header-only, standard library and
// nlohmann only.
namespace GltfTest
{
    // Accessor component types
    constexpr int kByte = 5120;
    constexpr int kUnsignedByte = 5121;
    constexpr int kShort = 5122;
    constexpr int kUnsignedShort = 5123;
    constexpr int kUnsignedInt = 5125;
    constexpr int kFloat = 5126;

    // Primitive modes
    constexpr int kModePoints = 0;
    constexpr int kModeTriangles = 4;
    constexpr int kModeTriangleStrip = 5;
    constexpr int kModeTriangleFan = 6;

    inline std::string Base64(const std::vector<std::uint8_t> &bytes)
    {
        static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        std::size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3)
        {
            const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
            out.push_back(kAlphabet[(v >> 18) & 63]);
            out.push_back(kAlphabet[(v >> 12) & 63]);
            out.push_back(kAlphabet[(v >> 6) & 63]);
            out.push_back(kAlphabet[v & 63]);
        }
        if (i + 1 == bytes.size())
        {
            const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16;
            out.push_back(kAlphabet[(v >> 18) & 63]);
            out.push_back(kAlphabet[(v >> 12) & 63]);
            out += "==";
        }
        else if (i + 2 == bytes.size())
        {
            const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
            out.push_back(kAlphabet[(v >> 18) & 63]);
            out.push_back(kAlphabet[(v >> 12) & 63]);
            out.push_back(kAlphabet[(v >> 6) & 63]);
            out.push_back('=');
        }
        return out;
    }

    inline void PutU32(std::vector<std::uint8_t> &out, const std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    /// A GLB container: the 12-byte header, the JSON chunk (padded with spaces) and, when `bin` isn't empty, the
    /// BIN chunk (padded with zeros)
    inline std::vector<std::uint8_t> MakeGlb(const std::string &json, const std::vector<std::uint8_t> &bin)
    {
        std::string paddedJson = json;
        while (paddedJson.size() % 4 != 0)
        {
            paddedJson.push_back(' ');
        }
        std::vector<std::uint8_t> paddedBin = bin;
        while (paddedBin.size() % 4 != 0)
        {
            paddedBin.push_back(0);
        }
        const std::size_t total = 12 + 8 + paddedJson.size() + (bin.empty() ? 0 : 8 + paddedBin.size());
        std::vector<std::uint8_t> glb;
        glb.reserve(total);
        PutU32(glb, 0x46546C67); // "glTF"
        PutU32(glb, 2);
        PutU32(glb, static_cast<std::uint32_t>(total));
        PutU32(glb, static_cast<std::uint32_t>(paddedJson.size()));
        PutU32(glb, 0x4E4F534A); // "JSON"
        glb.insert(glb.end(), paddedJson.begin(), paddedJson.end());
        if (!bin.empty())
        {
            PutU32(glb, static_cast<std::uint32_t>(paddedBin.size()));
            PutU32(glb, 0x004E4942); // "BIN\0"
            glb.insert(glb.end(), paddedBin.begin(), paddedBin.end());
        }
        return glb;
    }

    inline std::vector<std::uint8_t> ToBytes(const std::string &text)
    {
        return {text.begin(), text.end()};
    }

    /// A glTF document under construction: add data to the one buffer, views and accessors over it, then meshes,
    /// materials and nodes as JSON
    class Builder
    {
    public:
        nlohmann::json doc;
        std::vector<std::uint8_t> bin;

        Builder()
        {
            doc["asset"] = {{"version", "2.0"}, {"generator", "N2Engine tests"}};
        }

        /// Appends raw bytes (4-byte aligned) as a new buffer view; its index
        int AddView(const void *data, const std::size_t size, const int byteStride = 0)
        {
            while (bin.size() % 4 != 0)
            {
                bin.push_back(0);
            }
            const std::size_t offset = bin.size();
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            bin.insert(bin.end(), bytes, bytes + size);
            nlohmann::json view = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}};
            if (byteStride > 0)
            {
                view["byteStride"] = byteStride;
            }
            return Push("bufferViews", std::move(view));
        }

        template <typename T>
        int AddView(const std::vector<T> &values, const int byteStride = 0)
        {
            return AddView(values.data(), values.size() * sizeof(T), byteStride);
        }

        /// An accessor over a buffer view (-1: no view, all zeros); its index
        int AddAccessor(const int view, const int componentType, const std::size_t count, const std::string &type,
                        const bool normalized = false, const std::size_t byteOffset = 0)
        {
            nlohmann::json accessor = {{"componentType", componentType}, {"count", count}, {"type", type}};
            if (view >= 0)
            {
                accessor["bufferView"] = view;
                if (byteOffset > 0)
                {
                    accessor["byteOffset"] = byteOffset;
                }
            }
            if (normalized)
            {
                accessor["normalized"] = true;
            }
            return Push("accessors", std::move(accessor));
        }

        /// Floats as a VEC2/VEC3/VEC4 accessor (`components` per element). VEC3 positions get min/max.
        int AddFloats(const std::vector<float> &values, const int components)
        {
            static const char *kTypes[] = {"", "SCALAR", "VEC2", "VEC3", "VEC4"};
            const int view = AddView(values);
            const std::size_t count = values.size() / static_cast<std::size_t>(components);
            const int accessor = AddAccessor(view, kFloat, count, kTypes[components]);
            if (components == 3 && count > 0)
            {
                std::vector<float> lo = {values[0], values[1], values[2]};
                std::vector<float> hi = lo;
                for (std::size_t i = 0; i < count; ++i)
                {
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        lo[c] = std::min(lo[c], values[i * 3 + c]);
                        hi[c] = std::max(hi[c], values[i * 3 + c]);
                    }
                }
                doc["accessors"][static_cast<std::size_t>(accessor)]["min"] = lo;
                doc["accessors"][static_cast<std::size_t>(accessor)]["max"] = hi;
            }
            return accessor;
        }

        int AddIndicesU8(const std::vector<std::uint8_t> &indices)
        {
            return AddAccessor(AddView(indices), kUnsignedByte, indices.size(), "SCALAR");
        }
        int AddIndicesU16(const std::vector<std::uint16_t> &indices)
        {
            return AddAccessor(AddView(indices), kUnsignedShort, indices.size(), "SCALAR");
        }
        int AddIndicesU32(const std::vector<std::uint32_t> &indices)
        {
            return AddAccessor(AddView(indices), kUnsignedInt, indices.size(), "SCALAR");
        }

        /// A primitive's JSON: POSITION (and NORMAL, TEXCOORD_0 when >= 0), indices when >= 0, material when >= 0
        static nlohmann::json Primitive(const int position, const int indices = -1, const int material = -1,
                                        const int normal = -1, const int texCoord = -1, const int mode = kModeTriangles)
        {
            nlohmann::json primitive;
            primitive["attributes"]["POSITION"] = position;
            if (normal >= 0)
                primitive["attributes"]["NORMAL"] = normal;
            if (texCoord >= 0)
                primitive["attributes"]["TEXCOORD_0"] = texCoord;
            if (indices >= 0)
                primitive["indices"] = indices;
            if (material >= 0)
                primitive["material"] = material;
            if (mode != kModeTriangles)
                primitive["mode"] = mode;
            return primitive;
        }

        int AddMesh(const std::string &name, std::vector<nlohmann::json> primitives)
        {
            nlohmann::json mesh;
            if (!name.empty())
                mesh["name"] = name;
            mesh["primitives"] = std::move(primitives);
            return Push("meshes", std::move(mesh));
        }

        /// An unlit material (KHR_materials_unlit) of one colour
        int AddUnlitMaterial(const std::string &name, const float r, const float g, const float b, const float a = 1.0f)
        {
            nlohmann::json material;
            if (!name.empty())
                material["name"] = name;
            material["pbrMetallicRoughness"]["baseColorFactor"] = {r, g, b, a};
            material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
            UseExtension("KHR_materials_unlit");
            return Push("materials", std::move(material));
        }

        int AddMaterial(nlohmann::json material) { return Push("materials", std::move(material)); }

        /// A node; `extra` is merged in (translation, rotation, scale, matrix, children, mesh)
        int AddNode(const std::string &name, const nlohmann::json &extra = nlohmann::json::object())
        {
            nlohmann::json node = extra.is_object() ? extra : nlohmann::json::object();
            if (!name.empty())
                node["name"] = name;
            return Push("nodes", std::move(node));
        }

        /// An image held in a buffer view
        int AddImage(const std::string &name, const std::vector<std::uint8_t> &encoded, const std::string &mimeType)
        {
            const int view = AddView(encoded);
            nlohmann::json image = {{"bufferView", view}, {"mimeType", mimeType}};
            if (!name.empty())
                image["name"] = name;
            return Push("images", std::move(image));
        }

        /// A texture of an image, nearest-filtered and clamped when asked
        int AddTexture(const int image, const bool nearest = false, const bool clamp = false)
        {
            nlohmann::json sampler = nlohmann::json::object();
            if (nearest)
            {
                sampler["magFilter"] = 9728;
                sampler["minFilter"] = 9728;
            }
            if (clamp)
            {
                sampler["wrapS"] = 33071;
                sampler["wrapT"] = 33071;
            }
            const int samplerIndex = Push("samplers", std::move(sampler));
            return Push("textures", nlohmann::json{{"source", image}, {"sampler", samplerIndex}});
        }

        void SetScene(const std::vector<int> &roots)
        {
            doc["scenes"] = nlohmann::json::array({nlohmann::json{{"nodes", roots}}});
            doc["scene"] = 0;
        }

        void UseExtension(const std::string &name, const bool required = false)
        {
            AddUnique("extensionsUsed", name);
            if (required)
            {
                AddUnique("extensionsRequired", name);
            }
        }

        /// The document with buffers[0] declared (`uri` set when not empty)
        [[nodiscard]] nlohmann::json WithBuffer(const std::string &uri) const
        {
            nlohmann::json out = doc;
            if (!bin.empty())
            {
                nlohmann::json buffer = {{"byteLength", bin.size()}};
                if (!uri.empty())
                    buffer["uri"] = uri;
                out["buffers"] = nlohmann::json::array({buffer});
            }
            return out;
        }

        /// A self-contained .gltf: the buffer as a base64 data: URI
        [[nodiscard]] std::vector<std::uint8_t> ToGltf() const
        {
            return ToBytes(WithBuffer("data:application/octet-stream;base64," + Base64(bin)).dump());
        }

        /// A .gltf whose buffer is the external file `binUri` (write `bin` there)
        [[nodiscard]] std::vector<std::uint8_t> ToGltfExternal(const std::string &binUri) const
        {
            return ToBytes(WithBuffer(binUri).dump());
        }

        /// A .glb: the buffer is the BIN chunk
        [[nodiscard]] std::vector<std::uint8_t> ToGlb() const { return MakeGlb(WithBuffer("").dump(), bin); }

    private:
        int Push(const char *key, nlohmann::json value)
        {
            if (!doc.contains(key))
            {
                doc[key] = nlohmann::json::array();
            }
            doc[key].push_back(std::move(value));
            return static_cast<int>(doc[key].size()) - 1;
        }

        void AddUnique(const char *key, const std::string &name)
        {
            if (!doc.contains(key))
            {
                doc[key] = nlohmann::json::array();
            }
            for (const auto &existing : doc[key])
            {
                if (existing == name)
                    return;
            }
            doc[key].push_back(name);
        }
    };

    /// A quad on the x/y plane facing +Z (two triangles 0-1-2 and 0-2-3 are counter-clockwise from +Z): 4 positions, x
    /// from `left` to `right` and y from -0.5 to 0.5
    inline std::vector<float> QuadPositions(const float left = -0.5f, const float right = 0.5f)
    {
        return {left, -0.5f, 0.0f, right, -0.5f, 0.0f, right, 0.5f, 0.0f, left, 0.5f, 0.0f};
    }

    /// A unit quad split down the middle into two primitives with two materials (left half material 0, right half
    /// material 1), sharing no vertices. Mesh "Halves", node "Quad". Unlit red and green.
    inline Builder TwoMaterialQuad()
    {
        Builder b;
        const int red = b.AddUnlitMaterial("Red", 1.0f, 0.0f, 0.0f);
        const int green = b.AddUnlitMaterial("Green", 0.0f, 1.0f, 0.0f);
        const int left = b.AddFloats(QuadPositions(-0.5f, 0.0f), 3);
        const int right = b.AddFloats(QuadPositions(0.0f, 0.5f), 3);
        const std::vector<std::uint16_t> indices = {0, 1, 2, 0, 2, 3};
        const int leftIndices = b.AddIndicesU16(indices);
        const int rightIndices = b.AddIndicesU16(indices);
        const int mesh = b.AddMesh("Halves", {Builder::Primitive(left, leftIndices, red), Builder::Primitive(right, rightIndices, green)});
        const int node = b.AddNode("Quad", {{"mesh", mesh}});
        b.SetScene({node});
        return b;
    }
}
