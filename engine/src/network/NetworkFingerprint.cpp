#include "engine/network/NetworkFingerprint.hpp"

#include <algorithm>
#include <format>
#include <vector>

#include "engine/Version.hpp"
#include "engine/network/NetworkHash.hpp"
#include "engine/network/NetworkRpc.hpp"
#include "engine/network/ReplicationCodec.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/JsonLimits.hpp"

namespace N2Engine::Network
{
    std::string_view ToString(const JoinRefusal refusal) noexcept
    {
        switch (refusal)
        {
        case JoinRefusal::None:
            return "none";
        case JoinRefusal::WireVersion:
            return "wire-version";
        case JoinRefusal::EngineVersion:
            return "engine-version";
        case JoinRefusal::SceneContent:
            return "scene-content";
        case JoinRefusal::Registry:
            return "registry";
        case JoinRefusal::ObjectCount:
            return "object-count";
        }
        return "unknown";
    }

    std::uint64_t HashSceneJson(const nlohmann::json &scene)
    {
        const std::string text =
            JsonLimits::IterativeDump(scene, -1, ' ', false, nlohmann::json::error_handler_t::replace);
        Fnv1a64 hash;
        hash.String("n2net-scene-1");
        hash.String(text);
        return hash.Value();
    }

    std::uint64_t HashRegistry(const PrefabTable &prefabs, const InputLayout &inputLayout)
    {
        ComponentRegistry &registry = ComponentRegistry::Instance();
        std::vector<std::string> types = registry.GetRegisteredTypes();
        std::sort(types.begin(), types.end());
        Fnv1a64 hash;
        hash.String("n2net-registry-4");
        hash.U32(static_cast<std::uint32_t>(types.size()));
        for (const std::string &type : types)
        {
            hash.String(type);
            // What changes the bytes of replicated fields on the wire: which fields a type replicates and how each is
            // encoded (kind, shape, step, range, enum options). A LuaComponent's script fields are per instance, not per
            // type: they are covered by the layout hash every state entry carries.
            std::vector<const FieldInfo *> replicated;
            const std::optional<ComponentSchema> schema = registry.Describe(type);
            if (schema.has_value())
            {
                for (const FieldInfo &field : schema->fields)
                {
                    if (field.replicated && IsReplicableKind(field.kind))
                    {
                        replicated.push_back(&field);
                    }
                }
            }
            hash.U32(static_cast<std::uint32_t>(replicated.size()));
            for (const FieldInfo *field : replicated)
            {
                HashFieldFormat(hash, *field);
            }
        }
        // The C++ RPCs: which each type declares, who may call them, their delivery and parameters
        RpcRegistry::Instance().HashInto(hash);
        hash.U64(prefabs.Hash());
        hash.U64(inputLayout.Hash());
        return hash.Value();
    }

    std::string SanitizeText(const std::string_view text, const std::size_t maxLength)
    {
        std::string clean(text.substr(0, maxLength));
        for (char &character : clean)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20 || byte > 0x7E)
            {
                character = '?';
            }
        }
        return clean;
    }

    Fingerprint MakeFingerprint(const std::string_view scenePath, const std::uint64_t sceneHash,
                                const std::uint32_t sceneObjectCount, const PrefabTable &prefabs,
                                const InputLayout &inputLayout)
    {
        Fingerprint fingerprint;
        fingerprint.engineVersion = SanitizeText(EngineVersion(), kMaxEngineVersionLength);
        fingerprint.sceneHash = sceneHash;
        fingerprint.registryHash = HashRegistry(prefabs, inputLayout);
        fingerprint.scenePath = SanitizeText(scenePath, kMaxScenePathLength);
        fingerprint.sceneObjectCount = sceneObjectCount;
        fingerprint.prefabCount = static_cast<std::uint16_t>(prefabs.Size());
        return fingerprint;
    }

    JoinRefusal CompareFingerprints(const Fingerprint &server, const Fingerprint &client) noexcept
    {
        if (server.engineVersion != client.engineVersion)
        {
            return JoinRefusal::EngineVersion;
        }
        if (server.sceneHash != client.sceneHash)
        {
            return JoinRefusal::SceneContent;
        }
        if (server.registryHash != client.registryHash || server.prefabCount != client.prefabCount)
        {
            return JoinRefusal::Registry;
        }
        if (server.sceneObjectCount != client.sceneObjectCount)
        {
            return JoinRefusal::ObjectCount;
        }
        return JoinRefusal::None;
    }

    std::string DescribeRefusal(const JoinRefusal refusal, const Fingerprint &server, const Fingerprint &client)
    {
        switch (refusal)
        {
        case JoinRefusal::None:
            return "the fingerprints agree";
        case JoinRefusal::WireVersion:
            return "the server and this client speak different network wire versions";
        case JoinRefusal::EngineVersion:
            return std::format("the server runs engine version '{}' and this client runs '{}'", server.engineVersion,
                               client.engineVersion);
        case JoinRefusal::SceneContent:
            return std::format("the server's scene '{}' (content hash 0x{:016X}) differs from this client's scene '{}' "
                               "(content hash 0x{:016X}): both must load the same scene file",
                               server.scenePath, server.sceneHash, client.scenePath, client.sceneHash);
        case JoinRefusal::Registry:
            return std::format("the registered component types, prefabs or networked input layout differ (server: {} prefab(s), registry hash "
                               "0x{:016X}; this client: {} prefab(s), registry hash 0x{:016X}): both must register the "
                               "same component types, prefabs and input layout",
                               server.prefabCount, server.registryHash, client.prefabCount, client.registryHash);
        case JoinRefusal::ObjectCount:
            return std::format("the server's scene has {} network object(s) and this client's has {}",
                               server.sceneObjectCount, client.sceneObjectCount);
        }
        return "the fingerprints differ";
    }
}
