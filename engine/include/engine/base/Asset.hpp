#pragma once

#include <math/UUID.hpp>

#include "engine/serialization/MathSerialization.hpp"
#include "engine/io/ResourcePath.hpp"

namespace N2Engine::Base
{
    class Asset
    {
    protected:
        Math::UUID _uuid;
        IO::ResourcePath _resourcePath;
        bool _isSubResource = false;

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
    };
}
