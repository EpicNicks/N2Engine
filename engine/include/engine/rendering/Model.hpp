#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/base/Asset.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Texture.hpp"

namespace N2Engine
{
    class GameObject;
}

namespace N2Engine::Rendering
{
    /// What a model does about missing vertex normals (the "generateNormals" import setting)
    enum class ModelNormals : std::uint8_t
    {
        Never,     ///< "never": keep the file's; a mesh without them gets (0, 0, 1)
        IfMissing, ///< "ifMissing" (the default): flat normals for meshes the file gives none (glTF's rule)
        Always     ///< "always": ignore the file's and generate flat normals everywhere
    };

    /**
     * A model's import settings, from its .meta file's customData:
     *   {"model": {"scale": 1.0, "importMaterials": true, "generateNormals": "ifMissing",
     *              "mergeSubmeshesByMaterial": false}}
     * Every key is optional; the defaults are below.
     */
    struct ModelSettings
    {
        /// Multiplies every vertex position and node translation (0.01 for a model made in centimetres). Above 0.
        float scale = 1.0f;
        /// Make Material and Texture sub-assets from the file's materials and images. Off, every submesh draws with
        /// MeshRenderer's default material (lit white).
        bool importMaterials = true;
        ModelNormals generateNormals = ModelNormals::IfMissing;
        /// One submesh per material in each mesh, joining primitives that share a material
        bool mergeSubmeshesByMaterial = false;

        friend bool operator==(const ModelSettings &, const ModelSettings &) = default;
    };

    /// A node of a model's hierarchy: a name, a local transform and an optional mesh
    struct ModelNode
    {
        std::string name;
        Math::Vector3 position{0.0f, 0.0f, 0.0f};
        Math::Quaternion rotation = Math::Quaternion::Identity;
        Math::Vector3 scale{1.0f, 1.0f, 1.0f};
        /// An index into GetMeshes(), or -1
        int meshIndex = -1;
        /// Indices into GetNodes()
        std::vector<std::size_t> children;
    };

    /**
     * A 3D model file (`"Model"`: .gltf with its .bin files and images, or .glb): a node hierarchy plus the meshes,
     * materials and textures inside the file, which are sub-assets. CPU data only, so it loads headless.
     *
     * Sub-assets are keyed by kind and name ("mesh/Body", "material/Paint", "texture/Albedo") when every name of
     * that kind is non-empty and unique, and by index ("mesh/0", "mesh/1", ...) for every one of that kind
     * otherwise. A project model's sub-assets get deterministic UUIDs from ResourceUUID::FromSubAsset when it loads,
     * so scene files save references to them like any other asset, and Resources::LoadByUUID of one loads (or
     * reuses) the model and returns it.
     *
     * Instantiate builds a GameObject hierarchy through PrefabManager: one object per node, with the node's name and
     * local transform, and a MeshRenderer whose material slots are the mesh's submesh materials.
     */
    class Model final : public Base::Asset
    {
    public:
        Model() = default;

        /**
         * A model from a file's bytes held in memory (glTF JSON or GLB), with external files (buffers, images)
         * resolved inside `baseDirectory`. `name` names the root object Instantiate makes and the log messages.
         * nullptr, with an error logged, if it can't be imported. It isn't a project file, so its meshes, materials
         * and textures are runtime assets, like Texture::Create's: random UUIDs, not marked as sub-assets, and a
         * saved reference to one (a scene, a .mat) doesn't resolve in a later run. Load project models through
         * Resources or ResourceLoader for stable UUIDs.
         */
        [[nodiscard]] static std::shared_ptr<Model> LoadFromMemory(std::span<const std::uint8_t> fileBytes,
                                                                   const std::filesystem::path &baseDirectory = {},
                                                                   const ModelSettings &settings = {},
                                                                   std::string_view name = "Model");

        /// The settings in a .meta customData object (its "model" member), on top of `defaults`. Missing keys keep
        /// the default; a key with the wrong type, an unknown value or an unknown name is ignored with a warning.
        [[nodiscard]] static ModelSettings ParseSettings(const nlohmann::json &customData,
                                                         const ModelSettings &defaults = {});

        /// "mesh/<name>" for each name when all are non-empty and unique, else "mesh/<index>" for all (`kind` "mesh")
        [[nodiscard]] static std::vector<std::string> MakeSubAssetKeys(std::string_view kind,
                                                                       const std::vector<std::string> &names);

        /// Registers the .gltf/.glb loader with Resources (and so ResourceLoader), and "Model" as a type with
        /// sub-assets. Model.cpp already does this at static initialisation, and ResourceLoader::Initialize calls it
        /// too; calling it again is harmless.
        static void RegisterLoader();

        /// Loads a .gltf or .glb file, with settings from its .meta when ResourceLoader tracks it
        bool Load(const std::filesystem::path &path) override;
        /// What GetResourceType() returns, for code that needs it without an instance (asset metadata, a field's asset type)
        static constexpr std::string_view ResourceTypeName = "Model";
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        [[nodiscard]] bool IsLoaded() const { return _loaded; }
        /// The name Instantiate gives the root object: the file's name without its extension
        [[nodiscard]] const std::string &GetName() const { return _name; }
        [[nodiscard]] const ModelSettings &GetSettings() const { return _settings; }

        [[nodiscard]] const std::vector<ModelNode> &GetNodes() const { return _nodes; }
        /// The nodes at the top of the hierarchy (the root object's children)
        [[nodiscard]] const std::vector<std::size_t> &GetRootNodes() const { return _rootNodes; }
        /// One per mesh in the file; nullptr for one with no triangles
        [[nodiscard]] const std::vector<std::shared_ptr<Mesh>> &GetMeshes() const { return _meshes; }
        /// For mesh i, each submesh's index into GetMaterials(), or -1 (the default material)
        [[nodiscard]] const std::vector<std::vector<int>> &GetSubmeshMaterials() const { return _submeshMaterials; }
        /// One per material in the file (none when importMaterials is off)
        [[nodiscard]] const std::vector<std::shared_ptr<Material>> &GetMaterials() const { return _materials; }
        /// One per image in the file; nullptr for one that couldn't be read or decoded
        [[nodiscard]] const std::vector<std::shared_ptr<Texture>> &GetTextures() const { return _textures; }
        /// What the importer ignored or couldn't read (each also logged as a warning)
        [[nodiscard]] const std::vector<std::string> &GetImportWarnings() const { return _warnings; }

        /// Every mesh, material and texture with its key (null ones left out)
        [[nodiscard]] std::vector<Base::SubAssetRef> GetSubAssets() const override;
        [[nodiscard]] std::shared_ptr<Base::Asset> FindSubAsset(std::string_view key) const override;
        /// Whether a mesh, material or texture is held by anything besides this model (and, for a texture, its
        /// materials): a MeshRenderer, a GPU cache entry, a script
        [[nodiscard]] bool AreSubAssetsInUse() const override;

        /**
         * A new GameObject hierarchy for this model, in no scene (add it to one with Scene::AddRootGameObject or
         * GameObject::AddChild): a root object named GetName() with one child object per root node, and so on down.
         * Each object has its node's name and local transform; a node with a mesh gets a MeshRenderer drawing it,
         * with material slot i holding submesh i's material. Every call makes fresh GameObject and component UUIDs
         * (PrefabManager::InstantiatePrefab). nullptr for a model that didn't load.
         */
        [[nodiscard]] std::shared_ptr<GameObject> Instantiate() const;

        /// The prefab JSON Instantiate builds from (made once): the hierarchy with empty MeshRenderers, whose
        /// meshes and materials Instantiate then fills in
        [[nodiscard]] const nlohmann::json &GetPrefabJson() const;

    private:
        bool Import(std::span<const std::uint8_t> fileBytes, const std::filesystem::path &baseDirectory,
                    const ModelSettings &settings, const std::string &debugName);
        void Attach(GameObject &gameObject, std::size_t nodeIndex) const;

        bool _loaded = false;
        std::string _name = "Model";
        ModelSettings _settings;
        std::vector<ModelNode> _nodes;
        std::vector<std::size_t> _rootNodes;
        std::vector<std::shared_ptr<Mesh>> _meshes;
        std::vector<std::vector<int>> _submeshMaterials;
        std::vector<std::shared_ptr<Material>> _materials;
        std::vector<std::shared_ptr<Texture>> _textures;
        std::vector<std::string> _meshKeys;
        std::vector<std::string> _materialKeys;
        std::vector<std::string> _textureKeys;
        std::vector<std::string> _warnings;
        mutable std::optional<nlohmann::json> _prefab;
    };

    /// The loader registered for .gltf and .glb
    std::shared_ptr<Base::Asset> LoadModelFromFile(const std::filesystem::path &path);
}
