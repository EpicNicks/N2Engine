#include "assetimport/UfbxImporter.hpp"
#include "assetimport/TangentGeneration.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ufbx/ufbx.h>

namespace N2Engine::AssetImport
{
    namespace
    {
        namespace fs = std::filesystem;

        using ImportResult = std::expected<ImportedScene, ModelImportError>;

        constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
        /// An .mtl file is plain text; over this it is skipped
        constexpr std::size_t kMaxMtlBytes = std::size_t{16} * 1024 * 1024;
        /// How many of ufbx's own warnings are passed on to the log
        constexpr std::size_t kMaxUfbxWarnings = 8;

        std::unexpected<ModelImportError> Fail(const ModelImportErrorCode code, std::string message)
        {
            return std::unexpected(ModelImportError{code, std::move(message)});
        }

        struct SceneFree
        {
            void operator()(ufbx_scene *scene) const { ufbx_free_scene(scene); }
        };
        using ScenePtr = std::unique_ptr<ufbx_scene, SceneFree>;

        float F(const ufbx_real value)
        {
            return static_cast<float>(value);
        }

        std::string Text(const ufbx_string &text)
        {
            return text.data != nullptr ? std::string(text.data, text.length) : std::string();
        }

        /// A warning said once
        class Warnings
        {
        public:
            explicit Warnings(std::vector<std::string> &sink) : _sink(sink) {}

            void Add(std::string text)
            {
                if (std::ranges::find(_sink, text) == _sink.end())
                {
                    _sink.push_back(std::move(text));
                }
            }

        private:
            std::vector<std::string> &_sink;
        };

        ModelImportErrorCode CodeFor(const ufbx_error_type type)
        {
            switch (type)
            {
            case UFBX_ERROR_EMPTY_FILE:
                return ModelImportErrorCode::EmptyInput;
            case UFBX_ERROR_OUT_OF_MEMORY:
            case UFBX_ERROR_MEMORY_LIMIT:
            case UFBX_ERROR_ALLOCATION_LIMIT:
            case UFBX_ERROR_NODE_DEPTH_LIMIT:
                return ModelImportErrorCode::TooLarge;
            case UFBX_ERROR_FEATURE_DISABLED:
                return ModelImportErrorCode::Unsupported;
            case UFBX_ERROR_BAD_INDEX:
            case UFBX_ERROR_BAD_NURBS:
                return ModelImportErrorCode::InvalidData;
            default:
                return ModelImportErrorCode::ParseFailed;
            }
        }

        /// ufbx's last non-binary-looking marker: an FBX file starts "Kaydara FBX Binary" or, as ASCII, has "FBX" in
        /// its first comment line ("; FBX 7.3.0 project file")
        bool LooksLikeFbx(const std::span<const std::uint8_t> bytes)
        {
            constexpr std::string_view binary = "Kaydara FBX Binary";
            if (bytes.size() >= binary.size() && std::memcmp(bytes.data(), binary.data(), binary.size()) == 0)
            {
                return true;
            }
            const std::size_t head = std::min<std::size_t>(bytes.size(), 256);
            const std::string_view text(reinterpret_cast<const char *>(bytes.data()), head);
            return text.find("; FBX") != std::string_view::npos || text.find("FBXHeaderExtension") != std::string_view::npos;
        }

        /// The first `mtllib` name in an OBJ's text, or empty
        std::string FirstMtlLib(const std::span<const std::uint8_t> bytes)
        {
            const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            std::size_t lineStart = 0;
            while (lineStart < text.size())
            {
                std::size_t lineEnd = text.find('\n', lineStart);
                if (lineEnd == std::string_view::npos)
                {
                    lineEnd = text.size();
                }
                std::string_view line = text.substr(lineStart, lineEnd - lineStart);
                lineStart = lineEnd + 1;
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                {
                    line.remove_prefix(1);
                }
                if (line.starts_with("mtllib") && line.size() > 6 && (line[6] == ' ' || line[6] == '\t'))
                {
                    line.remove_prefix(6);
                    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                    {
                        line.remove_prefix(1);
                    }
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    {
                        line.remove_suffix(1);
                    }
                    return std::string(line);
                }
            }
            return {};
        }

        /// Reads a file of at most `maxBytes`, or nullopt (the reason in `error`)
        std::optional<std::vector<std::uint8_t>> ReadCapped(const fs::path &path, const std::size_t maxBytes,
                                                            std::string &error)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                error = "can't open the file";
                return std::nullopt;
            }
            const std::streamoff end = file.tellg();
            if (end < 0)
            {
                error = "can't read the file";
                return std::nullopt;
            }
            const auto size = static_cast<std::size_t>(end);
            if (size > maxBytes)
            {
                error = std::format("the file is {} bytes, over the {}-byte limit", size, maxBytes);
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(size);
            file.seekg(0);
            if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
            {
                error = "can't read the file";
                return std::nullopt;
            }
            return bytes;
        }

        /// A path from a file ('\' separators, as Windows wrote them) with '/' separators
        std::string NormaliseSeparators(std::string path)
        {
            std::ranges::replace(path, '\\', '/');
            return path;
        }

        std::string BareName(const std::string &path)
        {
            const std::size_t slash = path.find_last_of('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string MimeFor(const std::string &name)
        {
            std::string extension = fs::path(name).extension().string();
            std::ranges::transform(extension, extension.begin(),
                                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".png")
                return "image/png";
            if (extension == ".jpg" || extension == ".jpeg")
                return "image/jpeg";
            if (extension == ".tga")
                return "image/x-tga";
            if (extension == ".bmp")
                return "image/bmp";
            return {};
        }

        /// What one import has read so far, against the scene-wide caps
        struct Budget
        {
            std::size_t vertices = 0;
            std::size_t indices = 0;
            std::size_t imageBytes = 0;
        };

        struct VertexKey
        {
            std::array<std::uint32_t, 4> parts{};
            bool operator==(const VertexKey &other) const { return parts == other.parts; }
        };

        struct VertexKeyHash
        {
            std::size_t operator()(const VertexKey &key) const
            {
                std::size_t hash = 0xcbf29ce484222325ULL;
                for (const std::uint32_t part : key.parts)
                {
                    hash ^= part + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
                }
                return hash;
            }
        };

        struct Context
        {
            const ModelImportSettings &settings;
            const fs::path &baseDirectory;
            Warnings &warnings;
            Budget budget;
            ImportedScene &scene;
            /// Scene image index per ufbx file texture
            std::map<const ufbx_texture *, std::int32_t> imageOf;
        };

        ModelImportError OverBudget(const char *what)
        {
            return ModelImportError{ModelImportErrorCode::TooLarge,
                                    std::format("the model has more {} than the {} vertex / {} index limits allow", what,
                                                kMaxSceneVertices, kMaxSceneIndices)};
        }

        // ===== Images and materials =====

        /// The scene image for a texture (made on first use), or -1 when it has no usable file texture
        std::expected<std::int32_t, ModelImportError> ImageFor(Context &context, const ufbx_texture *texture,
                                                               const bool colour)
        {
            if (texture == nullptr)
            {
                return -1;
            }
            if (texture->type != UFBX_TEXTURE_FILE)
            {
                if (texture->file_textures.count == 0)
                {
                    context.warnings.Add("a layered or procedural texture is ignored");
                    return -1;
                }
                texture = texture->file_textures.data[0];
            }
            if (const auto found = context.imageOf.find(texture); found != context.imageOf.end())
            {
                if (colour)
                {
                    context.scene.images[static_cast<std::size_t>(found->second)].colour = true;
                }
                return found->second;
            }

            ImportedModelImage image;
            image.name = Text(texture->name);
            image.colour = colour;
            image.clamp = texture->wrap_u == UFBX_WRAP_CLAMP || texture->wrap_v == UFBX_WRAP_CLAMP;
            const std::string relative = NormaliseSeparators(Text(texture->relative_filename));
            const std::string absolute = NormaliseSeparators(Text(texture->absolute_filename));
            const std::string label = !image.name.empty() ? image.name : (!relative.empty() ? relative : absolute);

            const auto charge = [&](const std::size_t size) { return size <= kMaxModelTotalImageBytes - context.budget.imageBytes; };
            if (texture->content.size > 0 && texture->content.data != nullptr)
            {
                if (texture->content.size > kMaxModelImageBytes)
                {
                    context.warnings.Add(std::format("image '{}' is over the {}-byte limit and is skipped", label,
                                                     kMaxModelImageBytes));
                }
                else
                {
                    if (!charge(texture->content.size))
                    {
                        return std::unexpected(ModelImportError{ModelImportErrorCode::TooLarge,
                                                                std::format("the model's images come to over the {}-byte limit",
                                                                            kMaxModelTotalImageBytes)});
                    }
                    context.budget.imageBytes += texture->content.size;
                    const auto *data = static_cast<const std::uint8_t *>(texture->content.data);
                    image.bytes.assign(data, data + texture->content.size);
                }
                image.uri = !relative.empty() ? relative : BareName(absolute);
                image.mimeType = MimeFor(image.uri);
            }
            else
            {
                // The file the texture names: by its relative path first, then by its bare name (FBX files often hold
                // the absolute path on the artist's machine)
                std::vector<std::string> candidates;
                if (!relative.empty())
                    candidates.push_back(relative);
                for (const std::string &name : {BareName(relative), BareName(absolute)})
                {
                    if (!name.empty() && std::ranges::find(candidates, name) == candidates.end())
                        candidates.push_back(name);
                }
                image.uri = candidates.empty() ? std::string() : candidates.front();
                image.mimeType = MimeFor(image.uri);
                if (candidates.empty())
                {
                    context.warnings.Add(std::format("image '{}' names no file and is skipped", label));
                }
                else if (context.baseDirectory.empty())
                {
                    context.warnings.Add(std::format("image '{}' is in another file but the model has no folder; skipped", label));
                }
                else
                {
                    std::string lastError = "it is not inside the model's folder";
                    bool read = false;
                    for (const std::string &candidate : candidates)
                    {
                        const fs::path path = ResolveModelUri(context.baseDirectory, PercentDecode(candidate));
                        if (path.empty())
                        {
                            continue;
                        }
                        std::error_code sizeError;
                        const std::uintmax_t fileSize = fs::file_size(path, sizeError);
                        if (!sizeError && fileSize <= kMaxModelImageBytes && !charge(static_cast<std::size_t>(fileSize)))
                        {
                            return std::unexpected(ModelImportError{ModelImportErrorCode::TooLarge,
                                                                    std::format("the model's images come to over the {}-byte limit",
                                                                                kMaxModelTotalImageBytes)});
                        }
                        std::string error;
                        auto bytes = ReadCapped(path, kMaxModelImageBytes, error);
                        if (!bytes)
                        {
                            lastError = error;
                            continue;
                        }
                        context.budget.imageBytes += bytes->size();
                        image.uri = candidate;
                        image.mimeType = MimeFor(candidate);
                        image.bytes = std::move(*bytes);
                        read = true;
                        break;
                    }
                    if (!read)
                    {
                        context.warnings.Add(std::format("image '{}' ({}) is skipped: {}", label, candidates.front(), lastError));
                    }
                }
            }
            const auto index = static_cast<std::int32_t>(context.scene.images.size());
            context.scene.images.push_back(std::move(image));
            context.imageOf[texture] = index;
            return index;
        }

        /// A texture on a material map, when the map has an enabled one
        std::expected<std::int32_t, ModelImportError> MapImage(Context &context, const ufbx_material_map &map,
                                                               const bool colour)
        {
            if (map.texture == nullptr || !map.texture_enabled)
            {
                return -1;
            }
            return ImageFor(context, map.texture, colour);
        }

        float Clamp01(const float value)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        }

        std::optional<ModelImportError> ConvertMaterial(Context &context, const ufbx_material &source,
                                                        ImportedMaterial &material)
        {
            material.name = Text(source.name);
            const ufbx_material_pbr_maps &pbr = source.pbr;

            material.unlit = source.features.unlit.enabled;
            material.doubleSided = source.features.double_sided.enabled;

            float factor = 1.0f;
            if (pbr.base_factor.has_value && std::isfinite(F(pbr.base_factor.value_real)))
            {
                factor = F(pbr.base_factor.value_real);
            }
            if (pbr.base_color.has_value)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const float value = F(pbr.base_color.value_vec4.v[c]);
                    material.baseColor[c] = std::isfinite(value) ? std::max(value * factor, 0.0f) : 1.0f;
                }
            }
            if (pbr.opacity.has_value)
            {
                const float opacity = F(pbr.opacity.value_real);
                if (std::isfinite(opacity) && opacity < 1.0f)
                {
                    material.baseColor[3] = Clamp01(opacity);
                    material.alphaMode = ImportedAlphaMode::Blend;
                }
            }
            if (pbr.roughness.has_value)
            {
                material.smoothness = 1.0f - Clamp01(F(pbr.roughness.value_real));
            }
            if (pbr.metalness.has_value)
            {
                material.metallic = Clamp01(F(pbr.metalness.value_real));
            }
            else
            {
                // FBX has no metalness unless the material is PBR: a plain material is not metal (glTF's default of 1
                // is for a material that gives a metallicRoughness)
                material.metallic = 0.0f;
            }
            if (pbr.emission_color.has_value)
            {
                float emissionFactor = 1.0f;
                if (pbr.emission_factor.has_value && std::isfinite(F(pbr.emission_factor.value_real)))
                {
                    emissionFactor = F(pbr.emission_factor.value_real);
                }
                for (int c = 0; c < 3; ++c)
                {
                    const float value = F(pbr.emission_color.value_vec3.v[c]);
                    material.emissive[c] = std::isfinite(value) ? std::max(value * emissionFactor, 0.0f) : 0.0f;
                }
            }

            auto base = MapImage(context, pbr.base_color, true);
            if (!base)
                return base.error();
            material.baseColorTexture = *base;
            auto normal = MapImage(context, pbr.normal_map, false);
            if (!normal)
                return normal.error();
            material.normalTexture = *normal;
            auto occlusion = MapImage(context, pbr.ambient_occlusion, false);
            if (!occlusion)
                return occlusion.error();
            material.occlusionTexture = *occlusion;
            auto emission = MapImage(context, pbr.emission_color, true);
            if (!emission)
                return emission.error();
            material.emissiveTexture = *emission;

            if ((pbr.roughness.texture != nullptr && pbr.roughness.texture_enabled) ||
                (pbr.metalness.texture != nullptr && pbr.metalness.texture_enabled))
            {
                context.warnings.Add("roughness and metalness textures are ignored (only their constant values are used)");
            }
            if (pbr.opacity.texture != nullptr && pbr.opacity.texture_enabled)
            {
                context.warnings.Add("opacity textures are ignored");
            }
            return std::nullopt;
        }

        // ===== Meshes =====

        struct Bucket
        {
            std::int32_t materialIndex = -1;
            std::vector<std::uint32_t> indices;
        };

        std::array<float, 3> Cross(const std::array<float, 3> &a, const std::array<float, 3> &b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        float Dot(const std::array<float, 3> &a, const std::array<float, 3> &b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        /// Normalises in place; false (and unchanged) for a zero or non-finite vector
        bool Normalise(std::array<float, 3> &v)
        {
            const float length = std::sqrt(Dot(v, v));
            if (!(length > 1e-20f) || !std::isfinite(length))
            {
                return false;
            }
            v = {v[0] / length, v[1] / length, v[2] / length};
            return true;
        }

        template <typename Attribute>
        std::uint32_t AttributeIndex(const Attribute &attribute, const std::size_t corner)
        {
            if (!attribute.exists || corner >= attribute.indices.count)
            {
                return kNone;
            }
            const std::uint32_t index = attribute.indices.data[corner];
            return index < attribute.values.count ? index : kNone;
        }

        /// Gives a mesh's vertices without a tangent MikkTSpace tangents (all of them with generateTangents = Always)
        std::optional<ModelImportError> FinishTangents(Context &context, const std::string &meshName, ImportedMesh &mesh)
        {
            if (context.settings.generateTangents == TangentGeneration::Never || mesh.vertices.empty())
            {
                return std::nullopt;
            }
            if (context.settings.generateTangents == TangentGeneration::IfMissing)
            {
                const bool normalMapped = std::ranges::any_of(mesh.submeshes, [&](const ImportedSubmesh &submesh)
                {
                    return submesh.materialIndex >= 0 &&
                           static_cast<std::size_t>(submesh.materialIndex) < context.scene.materials.size() &&
                           context.scene.materials[static_cast<std::size_t>(submesh.materialIndex)].normalTexture >= 0;
                });
                if (!normalMapped)
                {
                    return std::nullopt;
                }
            }
            const bool hasUvs = std::ranges::any_of(mesh.vertices, [](const ImportedVertex &vertex)
            {
                return vertex.texCoord[0] != 0.0f || vertex.texCoord[1] != 0.0f;
            });
            if (!hasUvs)
            {
                return std::nullopt;
            }
            const std::size_t sceneRoom = kMaxSceneVertices - context.budget.vertices;
            const std::size_t limit = std::min(kMaxMeshElements, mesh.vertices.size() + sceneRoom);
            const TangentResult result = GenerateTangents(
                mesh.vertices, mesh.indices,
                context.settings.generateTangents == TangentGeneration::Always ? TangentSelect::All : TangentSelect::Missing,
                limit);
            if (!result.ok)
            {
                context.warnings.Add(std::format("mesh '{}': tangents could not be generated (it keeps what it had)", meshName));
                return std::nullopt;
            }
            context.budget.vertices += result.verticesAdded;
            if (context.budget.vertices > kMaxSceneVertices)
            {
                return OverBudget("vertices");
            }
            if (result.verticesWritten > 0)
            {
                mesh.generatedTangents = true;
            }
            return std::nullopt;
        }

        std::optional<ModelImportError> ConvertMesh(Context &context, const ufbx_mesh &source, const std::size_t meshIndex,
                                                    ImportedMesh &mesh)
        {
            mesh.name = Text(source.name);
            const std::string label = !mesh.name.empty() ? mesh.name : std::format("mesh {}", meshIndex);
            if (!source.vertex_position.exists || source.num_triangles == 0)
            {
                context.warnings.Add(std::format("mesh '{}' has no triangles and makes no mesh", label));
                return std::nullopt;
            }
            if (source.num_indices > kMaxAccessorElements || source.num_triangles * 3 > kMaxMeshElements)
            {
                return ModelImportError{ModelImportErrorCode::TooLarge,
                                        std::format("mesh '{}' is over the {}-element limit", label, kMaxAccessorElements)};
            }

            const bool flat = context.settings.generateNormals == NormalGeneration::Always;
            const bool useFileNormals = !flat && source.vertex_normal.exists;
            const bool useFileTangents = context.settings.generateTangents != TangentGeneration::Always &&
                                         source.vertex_tangent.exists && source.vertex_bitangent.exists &&
                                         useFileNormals && !source.generated_normals;
            mesh.generatedNormals = flat || (source.vertex_normal.exists && source.generated_normals);

            const std::size_t vertexRoom =
                std::min(kMaxMeshElements, kMaxSceneVertices - std::min(kMaxSceneVertices, context.budget.vertices));
            const std::size_t indexRoom =
                std::min(kMaxMeshElements, kMaxSceneIndices - std::min(kMaxSceneIndices, context.budget.indices));

            std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> welded;
            std::vector<Bucket> buckets;
            std::size_t indexTotal = 0;
            bool skippedLowFaces = false;
            std::vector<std::uint32_t> triangulated(std::max<std::size_t>(source.max_face_triangles, 1) * 3);

            const auto emitVertex = [&](const std::size_t corner, const std::optional<std::array<float, 3>> &flatNormal)
                -> std::expected<std::uint32_t, ModelImportError>
            {
                VertexKey key;
                key.parts = {AttributeIndex(source.vertex_position, corner),
                             flatNormal ? kNone : AttributeIndex(source.vertex_normal, corner),
                             AttributeIndex(source.vertex_uv, corner), AttributeIndex(source.vertex_color, corner)};
                if (!flatNormal)
                {
                    if (const auto found = welded.find(key); found != welded.end())
                    {
                        return found->second;
                    }
                }
                if (mesh.vertices.size() >= vertexRoom)
                {
                    return std::unexpected(OverBudget("vertices"));
                }
                ImportedVertex vertex;
                if (key.parts[0] != kNone)
                {
                    const ufbx_vec3 &p = source.vertex_position.values.data[key.parts[0]];
                    vertex.position[0] = F(p.x);
                    vertex.position[1] = F(p.y);
                    vertex.position[2] = F(p.z);
                }
                if (flatNormal)
                {
                    vertex.normal[0] = (*flatNormal)[0];
                    vertex.normal[1] = (*flatNormal)[1];
                    vertex.normal[2] = (*flatNormal)[2];
                }
                else if (useFileNormals && key.parts[1] != kNone)
                {
                    const ufbx_vec3 &n = source.vertex_normal.values.data[key.parts[1]];
                    std::array<float, 3> normal{F(n.x), F(n.y), F(n.z)};
                    if (Normalise(normal))
                    {
                        vertex.normal[0] = normal[0];
                        vertex.normal[1] = normal[1];
                        vertex.normal[2] = normal[2];
                    }
                }
                if (key.parts[2] != kNone)
                {
                    const ufbx_vec2 &uv = source.vertex_uv.values.data[key.parts[2]];
                    vertex.texCoord[0] = F(uv.x);
                    vertex.texCoord[1] = F(uv.y);
                }
                if (key.parts[3] != kNone)
                {
                    const ufbx_vec4 &c = source.vertex_color.values.data[key.parts[3]];
                    for (int i = 0; i < 4; ++i)
                    {
                        const float value = F(c.v[i]);
                        vertex.color[i] = std::isfinite(value) ? value : 1.0f;
                    }
                }
                if (useFileTangents)
                {
                    const std::uint32_t t = AttributeIndex(source.vertex_tangent, corner);
                    const std::uint32_t b = AttributeIndex(source.vertex_bitangent, corner);
                    if (t != kNone && b != kNone)
                    {
                        const ufbx_vec3 &tv = source.vertex_tangent.values.data[t];
                        const ufbx_vec3 &bv = source.vertex_bitangent.values.data[b];
                        std::array<float, 3> tangent{F(tv.x), F(tv.y), F(tv.z)};
                        const std::array<float, 3> bitangent{F(bv.x), F(bv.y), F(bv.z)};
                        const std::array<float, 3> normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
                        if (Normalise(tangent))
                        {
                            vertex.tangent[0] = tangent[0];
                            vertex.tangent[1] = tangent[1];
                            vertex.tangent[2] = tangent[2];
                            vertex.tangent[3] = Dot(Cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
                        }
                    }
                }
                const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                if (!flatNormal)
                {
                    welded.emplace(key, index);
                }
                return index;
            };

            for (const ufbx_mesh_part &part : source.material_parts)
            {
                if (part.num_triangles == 0)
                {
                    skippedLowFaces = skippedLowFaces || part.num_point_faces > 0 || part.num_line_faces > 0;
                    continue;
                }
                std::int32_t materialIndex = -1;
                if (context.settings.importMaterials && part.index < source.materials.count)
                {
                    materialIndex = static_cast<std::int32_t>(source.materials.data[part.index]->typed_id);
                }
                skippedLowFaces = skippedLowFaces || part.num_point_faces > 0 || part.num_line_faces > 0;

                Bucket *bucket = nullptr;
                if (context.settings.mergeSubmeshesByMaterial)
                {
                    for (Bucket &candidate : buckets)
                    {
                        if (candidate.materialIndex == materialIndex)
                            bucket = &candidate;
                    }
                }
                if (bucket == nullptr)
                {
                    buckets.push_back(Bucket{materialIndex, {}});
                    bucket = &buckets.back();
                }

                for (const std::uint32_t faceIndex : part.face_indices)
                {
                    const ufbx_face face = source.faces.data[faceIndex];
                    if (face.num_indices < 3)
                    {
                        continue;
                    }
                    const std::uint32_t triangles =
                        ufbx_triangulate_face(triangulated.data(), triangulated.size(), &source, face);
                    for (std::uint32_t t = 0; t < triangles; ++t)
                    {
                        if (indexTotal + 3 > indexRoom)
                        {
                            return OverBudget("indices");
                        }
                        const std::uint32_t *corners = &triangulated[static_cast<std::size_t>(t) * 3];
                        std::optional<std::array<float, 3>> faceNormal;
                        if (flat)
                        {
                            std::array<std::array<float, 3>, 3> p{};
                            for (int k = 0; k < 3; ++k)
                            {
                                const std::uint32_t at = AttributeIndex(source.vertex_position, corners[k]);
                                if (at != kNone)
                                {
                                    const ufbx_vec3 &v = source.vertex_position.values.data[at];
                                    p[static_cast<std::size_t>(k)] = {F(v.x), F(v.y), F(v.z)};
                                }
                            }
                            std::array<float, 3> normal = Cross({p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]},
                                                                {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]});
                            if (!Normalise(normal))
                            {
                                normal = {0.0f, 0.0f, 1.0f};
                            }
                            faceNormal = normal;
                        }
                        for (int k = 0; k < 3; ++k)
                        {
                            const auto index = emitVertex(corners[k], faceNormal);
                            if (!index)
                            {
                                return index.error();
                            }
                            bucket->indices.push_back(*index);
                        }
                        indexTotal += 3;
                    }
                }
            }
            if (skippedLowFaces)
            {
                context.warnings.Add("points and lines are skipped");
            }

            for (Bucket &bucket : buckets)
            {
                if (bucket.indices.empty())
                {
                    continue;
                }
                ImportedSubmesh submesh;
                submesh.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
                submesh.indexCount = static_cast<std::uint32_t>(bucket.indices.size());
                submesh.materialIndex = bucket.materialIndex;
                mesh.indices.insert(mesh.indices.end(), bucket.indices.begin(), bucket.indices.end());
                mesh.submeshes.push_back(submesh);
            }
            if (mesh.submeshes.empty())
            {
                mesh.vertices.clear();
                return std::nullopt;
            }

            context.budget.vertices += mesh.vertices.size();
            context.budget.indices += mesh.indices.size();
            if (auto error = FinishTangents(context, label, mesh))
            {
                return error;
            }
            return std::nullopt;
        }

        // ===== The whole file =====

        ImportResult ImportUfbx(const std::span<const std::uint8_t> bytes, const fs::path &baseDirectory,
                                const ModelImportSettings &settings)
        {
            if (bytes.empty())
            {
                return Fail(ModelImportErrorCode::EmptyInput, "the model file is empty");
            }
            if (bytes.size() > kMaxModelFileBytes)
            {
                return Fail(ModelImportErrorCode::TooLarge,
                            std::format("the model is {} bytes, over the {}-byte limit", bytes.size(), kMaxModelFileBytes));
            }
            const double scale = std::isfinite(settings.scale) && settings.scale > 0.0f ? settings.scale : 1.0;

            ImportedScene result;
            Warnings warnings(result.warnings);

            const bool isFbx = LooksLikeFbx(bytes);

            ufbx_load_opts options = {};
            options.temp_allocator.memory_limit = kMaxUfbxMemoryBytes;
            options.result_allocator.memory_limit = kMaxUfbxMemoryBytes;
            options.node_depth_limit = static_cast<std::uint32_t>(kMaxNodeDepth);
            options.ignore_animation = true;
            options.skip_skin_vertices = true;
            options.load_external_files = false;
            options.use_blender_pbr_material = true;
            options.generate_missing_normals = settings.generateNormals == NormalGeneration::IfMissing;
            // To the engine's space: right-handed, y up, metres (times the scale setting)
            options.target_axes = ufbx_axes_right_handed_y_up;
            options.target_unit_meters = static_cast<ufbx_real>(1.0 / scale);
            options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
            options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
            options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
            options.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
            options.file_format = isFbx ? UFBX_FILE_FORMAT_FBX : UFBX_FILE_FORMAT_OBJ;
            // An OBJ names neither units nor axes
            options.obj_unit_meters = 1.0;
            options.obj_axes = ufbx_axes_right_handed_y_up;

            std::vector<std::uint8_t> mtl;
            if (!isFbx)
            {
                const std::string library = FirstMtlLib(bytes);
                if (!library.empty())
                {
                    if (baseDirectory.empty())
                    {
                        warnings.Add(std::format("the material library '{}' is in another file but the model has no folder; skipped", library));
                    }
                    else
                    {
                        const fs::path path = ResolveModelUri(baseDirectory, PercentDecode(NormaliseSeparators(library)));
                        std::string error;
                        auto read = path.empty() ? std::optional<std::vector<std::uint8_t>>{}
                                                 : ReadCapped(path, kMaxMtlBytes, error);
                        if (path.empty())
                        {
                            error = "it is not inside the model's folder";
                        }
                        if (read)
                        {
                            mtl = std::move(*read);
                            options.obj_mtl_data.data = mtl.data();
                            options.obj_mtl_data.size = mtl.size();
                        }
                        else
                        {
                            warnings.Add(std::format("the material library '{}' is skipped: {}", library, error));
                        }
                    }
                }
            }

            ufbx_error error = {};
            ScenePtr scenePtr(ufbx_load_memory(bytes.data(), bytes.size(), &options, &error));
            if (!scenePtr)
            {
                std::string message = Text(error.description);
                if (error.info_length > 0)
                {
                    message += std::format(" ({})", std::string(error.info, error.info_length));
                }
                return Fail(CodeFor(error.type), std::format("ufbx could not read the model: {}", message));
            }
            const ufbx_scene &scene = *scenePtr;
            if (!isFbx && scene.meshes.count == 0)
            {
                return Fail(ModelImportErrorCode::ParseFailed, "the file is not a readable FBX or OBJ model (no geometry found)");
            }

            for (std::size_t i = 0; i < scene.metadata.warnings.count && i < kMaxUfbxWarnings; ++i)
            {
                const ufbx_warning &warning = scene.metadata.warnings.data[i];
                warnings.Add(std::format("ufbx: {}", Text(warning.description)));
            }
            if (scene.skin_deformers.count > 0)
                warnings.Add("skins are ignored (the mesh is drawn in its bind pose)");
            if (scene.blend_deformers.count > 0)
                warnings.Add("blend shapes are ignored");
            if (scene.anim_stacks.count > 0)
                warnings.Add("animations are ignored");
            if (scene.cameras.count > 0)
                warnings.Add("cameras are ignored");
            if (scene.lights.count > 0)
                warnings.Add("lights are ignored");
            if (scene.nurbs_surfaces.count > 0 || scene.nurbs_curves.count > 0 || scene.line_curves.count > 0)
                warnings.Add("curves and NURBS surfaces are ignored");

            Context context{settings, baseDirectory, warnings, {}, result, {}};

            if (settings.importMaterials)
            {
                result.materials.resize(scene.materials.count);
                for (std::size_t i = 0; i < scene.materials.count; ++i)
                {
                    if (auto failure = ConvertMaterial(context, *scene.materials.data[i], result.materials[i]))
                    {
                        return std::unexpected(*failure);
                    }
                }
            }

            result.meshes.resize(scene.meshes.count);
            for (std::size_t i = 0; i < scene.meshes.count; ++i)
            {
                if (auto failure = ConvertMesh(context, *scene.meshes.data[i], i, result.meshes[i]))
                {
                    return std::unexpected(*failure);
                }
            }

            // Nodes, without ufbx's root (its children are the scene's root nodes)
            std::vector<std::int32_t> remap(scene.nodes.count, -1);
            std::int32_t nodeCount = 0;
            for (std::size_t i = 0; i < scene.nodes.count; ++i)
            {
                if (!scene.nodes.data[i]->is_root)
                {
                    remap[i] = nodeCount++;
                }
            }
            result.nodes.resize(static_cast<std::size_t>(nodeCount));
            const auto indexOf = [&](const ufbx_node *node) -> std::int32_t
            {
                return node->typed_id < remap.size() ? remap[node->typed_id] : -1;
            };
            for (std::size_t i = 0; i < scene.nodes.count; ++i)
            {
                const ufbx_node &node = *scene.nodes.data[i];
                if (remap[i] < 0)
                {
                    continue;
                }
                ImportedNode &out = result.nodes[static_cast<std::size_t>(remap[i])];
                out.name = Text(node.name);
                const ufbx_transform &t = node.local_transform;
                out.translation[0] = F(t.translation.x);
                out.translation[1] = F(t.translation.y);
                out.translation[2] = F(t.translation.z);
                out.rotation[0] = F(t.rotation.x);
                out.rotation[1] = F(t.rotation.y);
                out.rotation[2] = F(t.rotation.z);
                out.rotation[3] = F(t.rotation.w);
                out.scale[0] = F(t.scale.x);
                out.scale[1] = F(t.scale.y);
                out.scale[2] = F(t.scale.z);
                if (node.mesh != nullptr && node.mesh->typed_id < result.meshes.size())
                {
                    out.meshIndex = static_cast<std::int32_t>(node.mesh->typed_id);
                }
                for (const ufbx_node *child : node.children)
                {
                    if (const std::int32_t childIndex = indexOf(child); childIndex >= 0)
                    {
                        out.children.push_back(static_cast<std::uint32_t>(childIndex));
                    }
                }
            }
            if (scene.root_node != nullptr)
            {
                for (const ufbx_node *child : scene.root_node->children)
                {
                    if (const std::int32_t childIndex = indexOf(child); childIndex >= 0)
                    {
                        result.rootNodes.push_back(static_cast<std::uint32_t>(childIndex));
                    }
                }
            }
            return result;
        }
    }

    std::expected<ImportedScene, ModelImportError> UfbxImporter::Import(
        const std::span<const std::uint8_t> bytes, const std::filesystem::path &baseDirectory,
        const ModelImportSettings &settings) const
    try
    {
        return ImportUfbx(bytes, baseDirectory, settings);
    }
    catch (const std::bad_alloc &)
    {
        return Fail(ModelImportErrorCode::TooLarge, "out of memory while importing the model");
    }
    catch (const std::length_error &)
    {
        return Fail(ModelImportErrorCode::TooLarge, "the model needs a container larger than the library allows");
    }

    bool UfbxImporter::HandlesExtension(const std::string_view extension) const
    {
        return extension == ".fbx" || extension == ".obj";
    }
}
