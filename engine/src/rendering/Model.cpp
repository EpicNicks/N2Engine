#include "engine/rendering/Model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>

#include <assetimport/GltfImporter.hpp>
#include <assetimport/ModelImporters.hpp>

#include "engine/GameObjectScene.hpp" // GameObject's template members
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/Resources.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/rendering/MeshRenderer.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        // Lives here because anything that uses Model links this file. RegisterLoader is public too, and
        // ResourceLoader::Initialize calls it, for programs that never name Model.
        struct ModelLoaderRegistrar
        {
            ModelLoaderRegistrar()
            {
                Model::RegisterLoader();
            }
        } g_modelLoaderRegistrar;

        static_assert(sizeof(AssetImport::ImportedVertex) == sizeof(Renderer::Common::Vertex),
                      "the importer's vertex mirrors the engine's");

        std::optional<std::vector<std::uint8_t>> ReadModelFile(const std::filesystem::path &path, std::string &error)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                error = "can't open the file";
                return std::nullopt;
            }
            const std::streamoff size = file.tellg();
            if (size < 0)
            {
                error = "can't read the file";
                return std::nullopt;
            }
            if (static_cast<unsigned long long>(size) > AssetImport::kMaxModelFileBytes)
            {
                error = std::format("the file is {} bytes, over the {}-byte limit", size, AssetImport::kMaxModelFileBytes);
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            if (size > 0 && !file.read(reinterpret_cast<char *>(bytes.data()), size))
            {
                error = "can't read the file";
                return std::nullopt;
            }
            return bytes;
        }

        // A project model's settings come from its .meta; anything else (or a file outside the project) uses the
        // defaults
        ModelSettings SettingsFor(const std::filesystem::path &path)
        {
            const IO::ResourceLoader &loader = IO::ResourceLoader::Instance();
            if (loader.GetAssetsRoot().empty())
            {
                return {};
            }
            try
            {
                if (const IO::AssetMetadata *meta = loader.GetMetadata(loader.MakeResourcePath(path)))
                {
                    return Model::ParseSettings(meta->customData);
                }
            }
            catch (const std::exception &)
            {
                // Not a path ResourceLoader can express, so not one of its assets
            }
            return {};
        }

        void WarnBadSetting(const std::string_view key, const nlohmann::json &value, const std::string_view expected)
        {
            Logger::Warn(std::format("Model settings: ignoring \"{}\": {} (expected {})", key, value.dump(), expected));
        }

        AssetImport::ModelImportSettings ToImportSettings(const ModelSettings &settings)
        {
            AssetImport::ModelImportSettings result;
            result.scale = settings.scale;
            result.importMaterials = settings.importMaterials;
            result.mergeSubmeshesByMaterial = settings.mergeSubmeshesByMaterial;
            switch (settings.generateTangents)
            {
            case ModelTangents::Never:
                result.generateTangents = AssetImport::TangentGeneration::Never;
                break;
            case ModelTangents::Always:
                result.generateTangents = AssetImport::TangentGeneration::Always;
                break;
            case ModelTangents::IfMissing:
            default:
                result.generateTangents = AssetImport::TangentGeneration::IfMissing;
                break;
            }
            switch (settings.generateNormals)
            {
            case ModelNormals::Never:
                result.generateNormals = AssetImport::NormalGeneration::Never;
                break;
            case ModelNormals::Always:
                result.generateNormals = AssetImport::NormalGeneration::Always;
                break;
            case ModelNormals::IfMissing:
            default:
                result.generateNormals = AssetImport::NormalGeneration::IfMissing;
                break;
            }
            return result;
        }

        AlphaMode ToAlphaMode(const AssetImport::ImportedAlphaMode mode)
        {
            switch (mode)
            {
            case AssetImport::ImportedAlphaMode::Mask: return AlphaMode::Mask;
            case AssetImport::ImportedAlphaMode::Blend: return AlphaMode::Blend;
            case AssetImport::ImportedAlphaMode::Opaque:
            default: return AlphaMode::Opaque;
            }
        }

        std::shared_ptr<Mesh> MakeMesh(const AssetImport::ImportedMesh &imported)
        {
            if (imported.submeshes.empty() || imported.vertices.empty() || imported.indices.empty())
            {
                return nullptr;
            }
            Renderer::Common::MeshData data;
            data.vertices.resize(imported.vertices.size());
            for (std::size_t i = 0; i < imported.vertices.size(); ++i)
            {
                const AssetImport::ImportedVertex &from = imported.vertices[i];
                Renderer::Common::Vertex &to = data.vertices[i];
                for (std::size_t c = 0; c < 3; ++c)
                {
                    to.position[c] = from.position[c];
                    to.normal[c] = from.normal[c];
                }
                to.texCoord[0] = from.texCoord[0];
                to.texCoord[1] = from.texCoord[1];
                for (std::size_t c = 0; c < 4; ++c)
                {
                    to.color[c] = from.color[c];
                    to.tangent[c] = from.tangent[c];
                }
            }
            data.indices = imported.indices;
            std::vector<Submesh> submeshes;
            submeshes.reserve(imported.submeshes.size());
            for (const AssetImport::ImportedSubmesh &from : imported.submeshes)
            {
                Submesh submesh;
                submesh.firstIndex = from.firstIndex;
                submesh.indexCount = from.indexCount;
                submeshes.push_back(submesh);
            }
            return Mesh::Create(std::move(data), std::move(submeshes));
        }
    }

    // ===== Settings and keys =====

    ModelSettings Model::ParseSettings(const nlohmann::json &customData, const ModelSettings &defaults)
    {
        ModelSettings settings = defaults;
        if (!customData.is_object())
        {
            return settings;
        }
        const auto modelIt = customData.find("model");
        if (modelIt == customData.end())
        {
            return settings;
        }
        if (!modelIt->is_object())
        {
            WarnBadSetting("model", *modelIt, "an object");
            return settings;
        }
        for (const auto &[key, value] : modelIt->items())
        {
            if (key == "scale")
            {
                if (value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() > 0.0)
                    settings.scale = value.get<float>();
                else
                    WarnBadSetting(key, value, "a number above 0");
            }
            else if (key == "importMaterials")
            {
                if (value.is_boolean())
                    settings.importMaterials = value.get<bool>();
                else
                    WarnBadSetting(key, value, "true or false");
            }
            else if (key == "generateNormals")
            {
                const std::string name = value.is_string() ? value.get<std::string>() : std::string{};
                if (name == "never")
                    settings.generateNormals = ModelNormals::Never;
                else if (name == "ifMissing")
                    settings.generateNormals = ModelNormals::IfMissing;
                else if (name == "always")
                    settings.generateNormals = ModelNormals::Always;
                else
                    WarnBadSetting(key, value, "\"never\", \"ifMissing\" or \"always\"");
            }
            else if (key == "generateTangents")
            {
                const std::string name = value.is_string() ? value.get<std::string>() : std::string{};
                if (name == "never")
                    settings.generateTangents = ModelTangents::Never;
                else if (name == "ifMissing")
                    settings.generateTangents = ModelTangents::IfMissing;
                else if (name == "always")
                    settings.generateTangents = ModelTangents::Always;
                else
                    WarnBadSetting(key, value, "\"never\", \"ifMissing\" or \"always\"");
            }
            else if (key == "pbrMaterials")
            {
                if (value.is_boolean())
                    settings.pbrMaterials = value.get<bool>();
                else
                    WarnBadSetting(key, value, "true or false");
            }
            else if (key == "mergeSubmeshesByMaterial")
            {
                if (value.is_boolean())
                    settings.mergeSubmeshesByMaterial = value.get<bool>();
                else
                    WarnBadSetting(key, value, "true or false");
            }
            else
            {
                Logger::Warn(std::format("Model settings: ignoring unknown key \"{}\"", key));
            }
        }
        return settings;
    }

    std::vector<std::string> Model::MakeSubAssetKeys(const std::string_view kind, const std::vector<std::string> &names)
    {
        std::set<std::string> seen;
        bool unique = true;
        for (const std::string &name : names)
        {
            if (name.empty() || !seen.insert(name).second)
            {
                unique = false;
                break;
            }
        }
        std::vector<std::string> keys;
        keys.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            keys.push_back(unique ? std::format("{}/{}", kind, names[i]) : std::format("{}/{}", kind, i));
        }
        return keys;
    }

    // ===== Loading =====

    void Model::RegisterLoader()
    {
        static std::once_flag registered;
        std::call_once(registered, []
        {
            IO::ResourceLoader::Instance().RegisterSubAssetParentType("Model");
            for (const std::string &extension : AssetImport::GetModelExtensions())
            {
                IO::Resources::Instance().RegisterLoader(extension, LoadModelFromFile);
            }
        });
    }

    std::shared_ptr<Model> Model::LoadFromMemory(const std::span<const std::uint8_t> fileBytes,
                                                 const std::filesystem::path &baseDirectory,
                                                 const ModelSettings &settings, const std::string_view name,
                                                 const std::string_view extension)
    {
        auto model = std::make_shared<Model>();
        model->_name = name.empty() ? std::string("Model") : std::string(name);
        if (!model->Import(fileBytes, baseDirectory, settings, model->_name, extension))
        {
            return nullptr;
        }
        return model;
    }

    bool Model::Load(const std::filesystem::path &path)
    {
        std::string error;
        const auto bytes = ReadModelFile(path, error);
        if (!bytes)
        {
            Logger::Error(std::format("Cannot load model {}: {}", path.string(), error));
            return false;
        }
        _name = path.stem().string();
        if (_name.empty())
        {
            _name = "Model";
        }
        return Import(*bytes, path.parent_path(), SettingsFor(path), path.string(), path.extension().string());
    }

    bool Model::Import(const std::span<const std::uint8_t> fileBytes, const std::filesystem::path &baseDirectory,
                       const ModelSettings &settings, const std::string &debugName, const std::string_view extension)
    {
        // The importer for the file's extension; anything no built importer reads (no extension, a runtime buffer) is glTF
        static const AssetImport::GltfImporter gltf;
        const AssetImport::IModelImporter *importer = AssetImport::FindModelImporter(extension);
        if (importer == nullptr)
        {
            std::string lower(extension);
            std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == ".fbx" || lower == ".obj")
            {
                Logger::Error(std::format("Cannot load model {}: this build has no FBX/OBJ importer (build with "
                                          "N2ENGINE_MODEL_UFBX=ON)", debugName));
                return false;
            }
            importer = &gltf;
        }
        auto imported = importer->Import(fileBytes, baseDirectory, ToImportSettings(settings));
        if (!imported)
        {
            Logger::Error(std::format("Cannot load model {}: {} ({})", debugName, imported.error().message,
                                      AssetImport::ToString(imported.error().code)));
            return false;
        }
        AssetImport::ImportedScene &scene = *imported;
        for (const std::string &warning : scene.warnings)
        {
            Logger::Warn(std::format("Model {}: {}", debugName, warning));
        }

        _settings = settings;
        _warnings = std::move(scene.warnings);
        _prefab.reset();

        // Textures: one per image
        std::vector<std::string> imageNames;
        imageNames.reserve(scene.images.size());
        for (const auto &image : scene.images)
        {
            imageNames.push_back(image.name);
        }
        _textureKeys = MakeSubAssetKeys("texture", imageNames);
        _textures.assign(scene.images.size(), nullptr);
        for (std::size_t i = 0; i < scene.images.size(); ++i)
        {
            const AssetImport::ImportedModelImage &image = scene.images[i];
            if (image.bytes.empty())
            {
                continue;
            }
            TextureSettings textureSettings;
            textureSettings.srgb = image.colour; // colour (base colour, emissive) or data; read in linear lighting
            textureSettings.filter =
                image.nearest ? Renderer::Common::TextureFilter::Nearest : Renderer::Common::TextureFilter::Linear;
            textureSettings.wrap =
                image.clamp ? Renderer::Common::TextureWrap::ClampToEdge : Renderer::Common::TextureWrap::Repeat;
            textureSettings.flipY = true; // with the importer's v flip: images show upright
            auto texture = Texture::CreateFromEncoded(image.bytes, textureSettings,
                                                      std::format("{}#{}", debugName, _textureKeys[i]));
            _textures[i] = std::move(texture);
        }

        // Materials
        const auto textureAt = [this](const std::int32_t index) -> std::shared_ptr<Texture>
        {
            return index >= 0 && static_cast<std::size_t>(index) < _textures.size() ? _textures[static_cast<std::size_t>(index)]
                                                                                    : nullptr;
        };
        std::vector<std::string> materialNames;
        materialNames.reserve(scene.materials.size());
        for (const auto &material : scene.materials)
        {
            materialNames.push_back(material.name);
        }
        _materialKeys = MakeSubAssetKeys("material", materialNames);
        _materials.clear();
        _materials.reserve(scene.materials.size());
        for (std::size_t i = 0; i < scene.materials.size(); ++i)
        {
            const AssetImport::ImportedMaterial &from = scene.materials[i];
            auto material = Material::Create(from.unlit ? ShadingModel::Unlit
                                                        : (_settings.pbrMaterials ? ShadingModel::Pbr : ShadingModel::Lit));
            material->SetBaseColor(Common::Color(from.baseColor[0], from.baseColor[1], from.baseColor[2], from.baseColor[3]));
            if (auto texture = textureAt(from.baseColorTexture))
            {
                material->SetBaseColorTexture(std::move(texture));
            }
            material->SetAlphaMode(ToAlphaMode(from.alphaMode));
            material->SetAlphaCutoff(from.alphaCutoff);
            material->SetDoubleSided(from.doubleSided);
            material->SetSmoothness(from.smoothness);
            material->SetMetallic(from.metallic);
            material->SetEmissive(Common::Color(from.emissive[0], from.emissive[1], from.emissive[2], 1.0f));
            material->SetNormalTexture(textureAt(from.normalTexture));
            material->SetNormalScale(from.normalScale);
            material->SetOcclusionTexture(textureAt(from.occlusionTexture));
            material->SetOcclusionStrength(from.occlusionStrength);
            material->SetMetallicRoughnessTexture(textureAt(from.metallicRoughnessTexture));
            material->SetEmissiveTexture(textureAt(from.emissiveTexture));
            _materials.push_back(std::move(material));
        }

        // Meshes, and each submesh's material
        std::vector<std::string> meshNames;
        meshNames.reserve(scene.meshes.size());
        for (const auto &mesh : scene.meshes)
        {
            meshNames.push_back(mesh.name);
        }
        _meshKeys = MakeSubAssetKeys("mesh", meshNames);
        _meshes.assign(scene.meshes.size(), nullptr);
        _submeshMaterials.assign(scene.meshes.size(), {});
        for (std::size_t i = 0; i < scene.meshes.size(); ++i)
        {
            for (const AssetImport::ImportedSubmesh &submesh : scene.meshes[i].submeshes)
            {
                const bool valid = submesh.materialIndex >= 0 &&
                                   static_cast<std::size_t>(submesh.materialIndex) < _materials.size();
                _submeshMaterials[i].push_back(valid ? submesh.materialIndex : -1);
            }
            // Not marked as sub-assets here: only a project model's are (ResourceLoader does it when it gives them
            // their deterministic UUIDs). A model loaded from memory is made at runtime, like Texture::Create.
            _meshes[i] = MakeMesh(scene.meshes[i]);
        }

        // Nodes
        _nodes.clear();
        _nodes.reserve(scene.nodes.size());
        for (const AssetImport::ImportedNode &from : scene.nodes)
        {
            ModelNode node;
            node.name = from.name;
            node.position = Math::Vector3(from.translation[0], from.translation[1], from.translation[2]);
            // glTF stores x, y, z, w; the engine's Quaternion takes w first
            node.rotation = Math::Quaternion(from.rotation[3], from.rotation[0], from.rotation[1], from.rotation[2]);
            node.scale = Math::Vector3(from.scale[0], from.scale[1], from.scale[2]);
            const bool hasMesh = from.meshIndex >= 0 && static_cast<std::size_t>(from.meshIndex) < _meshes.size();
            node.meshIndex = hasMesh ? from.meshIndex : -1;
            node.children.assign(from.children.begin(), from.children.end());
            _nodes.push_back(std::move(node));
        }
        _rootNodes.assign(scene.rootNodes.begin(), scene.rootNodes.end());
        _loaded = true;
        return true;
    }

    // ===== Sub-assets =====

    std::vector<Base::SubAssetRef> Model::GetSubAssets() const
    {
        std::vector<Base::SubAssetRef> subAssets;
        for (std::size_t i = 0; i < _meshes.size(); ++i)
        {
            if (_meshes[i])
                subAssets.push_back(Base::SubAssetRef{_meshKeys[i], _meshes[i]});
        }
        for (std::size_t i = 0; i < _materials.size(); ++i)
        {
            if (_materials[i])
                subAssets.push_back(Base::SubAssetRef{_materialKeys[i], _materials[i]});
        }
        for (std::size_t i = 0; i < _textures.size(); ++i)
        {
            if (_textures[i])
                subAssets.push_back(Base::SubAssetRef{_textureKeys[i], _textures[i]});
        }
        return subAssets;
    }

    std::shared_ptr<Base::Asset> Model::FindSubAsset(const std::string_view key) const
    {
        for (std::size_t i = 0; i < _meshKeys.size() && i < _meshes.size(); ++i)
        {
            if (_meshKeys[i] == key)
                return _meshes[i];
        }
        for (std::size_t i = 0; i < _materialKeys.size() && i < _materials.size(); ++i)
        {
            if (_materialKeys[i] == key)
                return _materials[i];
        }
        for (std::size_t i = 0; i < _textureKeys.size() && i < _textures.size(); ++i)
        {
            if (_textureKeys[i] == key)
                return _textures[i];
        }
        return nullptr;
    }

    bool Model::AreSubAssetsInUse() const
    {
        for (const auto &mesh : _meshes)
        {
            if (mesh && mesh.use_count() > 1)
                return true;
        }
        for (const auto &material : _materials)
        {
            if (material && material.use_count() > 1)
                return true;
        }
        for (const auto &texture : _textures)
        {
            if (!texture)
                continue;
            // This model's list, plus every slot of its own materials that uses it
            long internal = 1;
            for (const auto &material : _materials)
            {
                if (!material)
                    continue;
                for (const Texture *slot :
                     {material->GetBaseColorTexture().get(), material->GetNormalTexture().get(),
                      material->GetOcclusionTexture().get(), material->GetMetallicRoughnessTexture().get(),
                      material->GetEmissiveTexture().get()})
                {
                    internal += slot == texture.get() ? 1 : 0;
                }
            }
            if (texture.use_count() > internal)
                return true;
        }
        return false;
    }

    // ===== Instantiation =====

    namespace
    {
        std::shared_ptr<GameObject> BuildTemplate(const std::vector<ModelNode> &nodes, const std::size_t index)
        {
            const ModelNode &node = nodes[index];
            auto gameObject = GameObject::Create(node.name.empty() ? std::format("Node {}", index) : node.name);
            gameObject->CreatePositionable();
            Positionable *positionable = gameObject->GetPositionable();
            positionable->SetLocalPosition(node.position);
            positionable->SetLocalRotation(node.rotation);
            positionable->SetLocalScale(node.scale);
            if (node.meshIndex >= 0)
            {
                // Empty in the prefab: Instantiate gives it the mesh and materials directly, so a model that isn't a
                // project file (no stable sub-asset UUIDs) instantiates the same way
                gameObject->AddComponent<MeshRenderer>();
            }
            for (const std::size_t child : node.children)
            {
                if (child < nodes.size())
                {
                    gameObject->AddChild(BuildTemplate(nodes, child), false);
                }
            }
            return gameObject;
        }
    }

    const nlohmann::json &Model::GetPrefabJson() const
    {
        if (!_prefab)
        {
            const auto root = GameObject::Create(_name);
            root->CreatePositionable();
            for (const std::size_t index : _rootNodes)
            {
                if (index < _nodes.size())
                {
                    root->AddChild(BuildTemplate(_nodes, index), false);
                }
            }
            _prefab = root->Serialize();
        }
        return *_prefab;
    }

    void Model::Attach(GameObject &gameObject, const std::size_t nodeIndex) const
    {
        const ModelNode &node = _nodes[nodeIndex];
        if (node.meshIndex >= 0)
        {
            const auto meshIndex = static_cast<std::size_t>(node.meshIndex);
            if (auto *renderer = gameObject.GetComponent<MeshRenderer>(); renderer && _meshes[meshIndex])
            {
                renderer->SetMesh(_meshes[meshIndex]);
                std::vector<std::shared_ptr<Material>> slots;
                for (const int material : _submeshMaterials[meshIndex])
                {
                    slots.push_back(material >= 0 ? _materials[static_cast<std::size_t>(material)] : nullptr);
                }
                renderer->SetMaterials(std::move(slots));
            }
        }
        // The prefab's children are in node order, skipping none (BuildTemplate makes one per valid child)
        const auto &children = gameObject.GetChildren();
        std::size_t next = 0;
        for (const std::size_t child : node.children)
        {
            if (child >= _nodes.size())
            {
                continue;
            }
            if (next >= children.size())
            {
                break;
            }
            Attach(*children[next++], child);
        }
    }

    std::shared_ptr<GameObject> Model::Instantiate() const
    {
        if (!_loaded)
        {
            Logger::Error(std::format("Model::Instantiate: the model '{}' isn't loaded", _name));
            return nullptr;
        }
        std::shared_ptr<GameObject> root = PrefabManager::InstantiatePrefab(GetPrefabJson());
        if (!root)
        {
            return nullptr;
        }
        const auto &children = root->GetChildren();
        std::size_t next = 0;
        for (const std::size_t index : _rootNodes)
        {
            if (index >= _nodes.size())
            {
                continue;
            }
            if (next >= children.size())
            {
                break;
            }
            Attach(*children[next++], index);
        }
        return root;
    }

    std::shared_ptr<Base::Asset> LoadModelFromFile(const std::filesystem::path &path)
    {
        auto model = std::make_shared<Model>();
        if (!model->Load(path))
        {
            return nullptr;
        }
        return model;
    }
}
