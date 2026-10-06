#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <math/UUID.hpp>

#include "engine/serialization/MathSerialization.hpp"
#include "engine/io/ResourcePath.hpp"

namespace N2Engine::Base
{
    class Asset;

    /// One asset inside another asset's file (a mesh, material or texture of a model), with the key that names it
    /// there ("mesh/Body", "material/0")
    struct SubAssetRef
    {
        std::string key;
        std::shared_ptr<Asset> asset;
    };

    class Asset
    {
    protected:
        Math::UUID _uuid;
        IO::ResourcePath _resourcePath;
        /// A sub-asset: made by its parent's loader (a model's meshes, materials and textures), with _resourcePath
        /// the parent's file and _subAssetKey its name inside it
        bool _isSubResource = false;
        std::string _subAssetKey;

    public:
        virtual ~Asset() = default;
        Asset();
        explicit Asset(Math::UUID uuid);
        [[nodiscard]] Math::UUID GetUUID() const;
        void SetUUID(const Math::UUID &uuid);

        const IO::ResourcePath& GetResourcePath() const { return _resourcePath; }
        void SetResourcePath(const IO::ResourcePath& path) { _resourcePath = path; }

        virtual nlohmann::json Serialize() const
        {
            nlohmann::json j;
            j["uuid"] = _uuid.ToString();
            j["resourcePath"] = _resourcePath;  // Uses custom serializer
            return j;
        }

        virtual void Deserialize(const nlohmann::json& j)
        {
            if (j.contains("uuid"))
            {
                _uuid = j["uuid"].get<Math::UUID>();
            }
            if (j.contains("resourcePath"))
            {
                _resourcePath = j["resourcePath"].get<IO::ResourcePath>();
            }
        }

        virtual bool Load(const std::filesystem::path& path) { return false; }
        virtual std::string GetResourceType() const = 0;

        /// Whether this asset lives inside another asset's file (see SubAssetRef). Its resource path is then the
        /// parent's, and GetSubAssetKey names it inside the parent.
        [[nodiscard]] bool IsSubResource() const { return _isSubResource; }
        /// The key naming this sub-asset inside its parent ("mesh/Body"); empty for an ordinary asset
        [[nodiscard]] const std::string& GetSubAssetKey() const { return _subAssetKey; }
        /// Marks this asset as the sub-asset `key` of its parent (the parent's loader calls it)
        void MarkAsSubAsset(std::string key)
        {
            _isSubResource = true;
            _subAssetKey = std::move(key);
        }

        /// The assets inside this one's file (a model's meshes, materials and textures), each with its key. Empty for
        /// an asset without sub-assets. ResourceLoader gives each a deterministic UUID (ResourceUUID::FromSubAsset)
        /// when the parent loads.
        [[nodiscard]] virtual std::vector<SubAssetRef> GetSubAssets() const { return {}; }

        /// The sub-asset with this key, or nullptr
        [[nodiscard]] virtual std::shared_ptr<Asset> FindSubAsset(const std::string_view key) const
        {
            for (SubAssetRef &sub : GetSubAssets())
            {
                if (sub.key == key)
                {
                    return std::move(sub.asset);
                }
            }
            return nullptr;
        }
    };
}
