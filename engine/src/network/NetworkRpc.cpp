#include "engine/network/NetworkRpc.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr std::uint64_t kMicrosPerSecond = 1'000'000;
        /// The shared bucket of every RPC beyond RpcBudget::kMaxTrackedRpcs
        constexpr std::uint64_t kOverflowKey = ~std::uint64_t{0};
        /// type, net id (5), component index (5), rpc index, table hash (2)
        constexpr std::size_t kMaxRpcHeaderBytes = 14;

        void HashRpc(Fnv1a64 &hash, const RpcDescriptor &rpc)
        {
            hash.String(rpc.name);
            hash.U8(static_cast<std::uint8_t>(rpc.direction));
            hash.U8(rpc.reliable ? 1 : 0);
            hash.U8(rpc.cosmetic ? 1 : 0);
            hash.U32(static_cast<std::uint32_t>(rpc.params.size()));
            for (const FieldInfo &param : rpc.params)
            {
                HashFieldFormat(hash, param);
            }
        }

        std::uint16_t Fold16(const std::uint64_t value) noexcept
        {
            return static_cast<std::uint16_t>((value ^ (value >> 16) ^ (value >> 32) ^ (value >> 48)) & 0xFFFFu);
        }

        bool ValidName(const std::string &name) noexcept
        {
            if (name.empty() || name.size() > kMaxRpcNameLength)
            {
                return false;
            }
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                const auto c = static_cast<unsigned char>(name[i]);
                const bool letter = std::isalpha(c) != 0 || c == '_';
                if (!(letter || (i > 0 && std::isdigit(c) != 0)))
                {
                    return false;
                }
            }
            return true;
        }
    }

    std::string_view ToString(const RpcDirection direction) noexcept
    {
        switch (direction)
        {
        case RpcDirection::ServerToClient:
            return "server-to-client";
        case RpcDirection::ClientToServer:
            return "client-to-server";
        case RpcDirection::ServerToOwner:
            return "server-to-owner";
        }
        return "unknown";
    }

    std::string_view ToString(const RpcSendResult result) noexcept
    {
        switch (result)
        {
        case RpcSendResult::Sent:
            return "sent";
        case RpcSendResult::UnknownObject:
            return "unknown-object";
        case RpcSendResult::UnknownRpc:
            return "unknown-rpc";
        case RpcSendResult::WrongDirection:
            return "wrong-direction";
        case RpcSendResult::NotOwner:
            return "not-owner";
        case RpcSendResult::NoTarget:
            return "no-target";
        case RpcSendResult::BadArguments:
            return "bad-arguments";
        case RpcSendResult::NotSimulationSafe:
            return "not-simulation-safe";
        case RpcSendResult::OverBudget:
            return "over-budget";
        }
        return "unknown";
    }

    std::string ValidateRpcDescriptor(const RpcDescriptor &descriptor)
    {
        if (!ValidName(descriptor.name))
        {
            return "the name must be 1 to 32 letters, digits and underscores, not starting with a digit";
        }
        if (descriptor.params.size() > kMaxRpcParams)
        {
            return std::format("an RPC takes at most {} parameters", kMaxRpcParams);
        }
        std::size_t bytes = kMaxRpcHeaderBytes;
        for (const FieldInfo &param : descriptor.params)
        {
            // References would need the object table of a world: an RPC takes plain values
            if (!IsReplicableKind(param.kind) || param.kind == FieldKind::GameObjectRef || param.kind == FieldKind::AssetRef)
            {
                return std::format("parameter '{}' has a kind that cannot be sent in an RPC ({})", param.name,
                                   FieldKindName(param.kind));
            }
            bytes += MaxEncodedBytes(param);
        }
        if (bytes > kMaxRpcMessageBytes)
        {
            return std::format("the parameters could take {} bytes, over the {} an RPC message holds", bytes,
                               kMaxRpcMessageBytes);
        }
        return {};
    }

    std::uint16_t HashRpcTable(const std::vector<RpcDescriptor> &table)
    {
        Fnv1a64 hash;
        hash.String("n2net-rpc-1");
        hash.U32(static_cast<std::uint32_t>(table.size()));
        for (const RpcDescriptor &rpc : table)
        {
            HashRpc(hash, rpc);
        }
        return Fold16(hash.Value());
    }

    RpcRegistry &RpcRegistry::Instance()
    {
        static RpcRegistry instance;
        return instance;
    }

    bool RpcRegistry::Register(const std::string &componentType, RpcDescriptor descriptor)
    {
        const std::string problem = ValidateRpcDescriptor(descriptor);
        if (!problem.empty())
        {
            Logger::Error(std::format("RegisterRpc: {}::{} refused: {}", componentType, descriptor.name, problem));
            return false;
        }
        std::vector<RpcDescriptor> &list = _rpcs[componentType];
        if (list.size() >= kMaxRpcsPerComponent)
        {
            Logger::Error(std::format("RegisterRpc: {} already has {} RPCs", componentType, kMaxRpcsPerComponent));
            return false;
        }
        const auto position = std::lower_bound(list.begin(), list.end(), descriptor.name,
                                               [](const RpcDescriptor &a, const std::string &name) { return a.name < name; });
        if (position != list.end() && position->name == descriptor.name)
        {
            Logger::Error(std::format("RegisterRpc: {}::{} is already registered", componentType, descriptor.name));
            return false;
        }
        list.insert(position, std::move(descriptor));
        return true;
    }

    const std::vector<RpcDescriptor> *RpcRegistry::Find(const std::string &componentType) const
    {
        const auto found = _rpcs.find(componentType);
        return found == _rpcs.end() || found->second.empty() ? nullptr : &found->second;
    }

    void RpcRegistry::HashInto(Fnv1a64 &hash) const
    {
        hash.String("n2net-rpcs-1");
        std::uint32_t types = 0;
        for (const auto &entry : _rpcs)
        {
            if (!entry.second.empty())
            {
                ++types;
            }
        }
        hash.U32(types);
        for (const auto &[type, list] : _rpcs)
        {
            if (list.empty())
            {
                continue;
            }
            hash.String(type);
            hash.U32(static_cast<std::uint32_t>(list.size()));
            for (const RpcDescriptor &rpc : list)
            {
                HashRpc(hash, rpc);
            }
        }
    }

    void RpcRegistry::Unregister(const std::string &componentType)
    {
        _rpcs.erase(componentType);
    }

    const std::vector<RpcDescriptor> *RpcTableOf(Component &component)
    {
        return component.GetRpcTable();
    }

    bool FindRpc(GameObject &object, Component &component, const std::string_view name, ResolvedRpc &out)
    {
        const std::vector<RpcDescriptor> *const table = RpcTableOf(component);
        if (table == nullptr)
        {
            return false;
        }
        const auto found = std::find_if(table->begin(), table->end(), [&](const RpcDescriptor &r) { return r.name == name; });
        if (found == table->end())
        {
            return false;
        }
        const auto &components = object.GetAllComponents();
        for (std::size_t i = 0; i < components.size(); ++i)
        {
            if (components[i].get() == &component)
            {
                out.component = &component;
                out.componentIndex = i;
                out.rpcIndex = static_cast<std::size_t>(found - table->begin());
                out.descriptor = &*found;
                return true;
            }
        }
        return false;
    }

    bool DecodeRpcHeader(const std::span<const std::uint8_t> bytes, RpcHeader &header) noexcept
    {
        if (bytes.empty() || bytes.size() > kMaxRpcMessageBytes)
        {
            return false;
        }
        const std::uint8_t type = bytes[0];
        if (type != kRpcServerReliableType && type != kRpcServerUnreliableType && type != kRpcClientReliableType &&
            type != kRpcClientUnreliableType)
        {
            return false;
        }
        N2Net::ByteReader reader(bytes.subspan(1));
        std::uint64_t netId = 0;
        std::uint64_t componentIndex = 0;
        if (!ReadVarint(reader, netId) || !ReadVarint(reader, componentIndex))
        {
            return false;
        }
        const std::uint8_t rpcIndex = reader.U8();
        const std::uint16_t hash = reader.U16();
        if (reader.Failed() || netId == 0 || netId > 0xFFFFFFFFull || componentIndex > 0xFFFFull)
        {
            return false;
        }
        header.type = type;
        header.object = NetworkId{static_cast<std::uint32_t>(netId)};
        header.componentIndex = static_cast<std::uint32_t>(componentIndex);
        header.rpcIndex = rpcIndex;
        header.tableHash = hash;
        header.args = bytes.subspan(bytes.size() - reader.Remaining());
        return true;
    }

    std::optional<std::vector<std::uint8_t>> EncodeRpcMessage(const std::uint8_t type, const NetworkId object,
                                                              const ResolvedRpc &rpc, const std::uint16_t tableHash,
                                                              const std::vector<nlohmann::json> &args, std::string &error)
    {
        const RpcDescriptor &descriptor = *rpc.descriptor;
        if (args.size() != descriptor.params.size())
        {
            error = std::format("{} takes {} arguments, {} were given", descriptor.name, descriptor.params.size(),
                                args.size());
            return std::nullopt;
        }
        std::vector<std::uint8_t> out;
        out.reserve(64);
        WireWriter writer(out);
        writer.U8(type);
        writer.Varint(object.value);
        writer.Varint(rpc.componentIndex);
        writer.U8(static_cast<std::uint8_t>(rpc.rpcIndex));
        writer.U16(tableHash);
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            const FieldInfo &param = descriptor.params[i];
            if (const std::optional<std::string> problem = ValidateFieldValue(param, args[i]))
            {
                error = std::format("{}: argument {}: {}", descriptor.name, i + 1, *problem);
                return std::nullopt;
            }
            if (!EncodeField(param, ClampFieldValue(param, args[i]), nullptr, out))
            {
                error = std::format("{}: argument {} cannot be encoded", descriptor.name, i + 1);
                return std::nullopt;
            }
            if (out.size() > kMaxRpcMessageBytes)
            {
                error = std::format("{}: the message is over {} bytes", descriptor.name, kMaxRpcMessageBytes);
                return std::nullopt;
            }
        }
        return out;
    }

    bool DecodeRpcArgs(const RpcDescriptor &descriptor, const std::span<const std::uint8_t> bytes,
                       std::vector<nlohmann::json> &args)
    {
        args.clear();
        if (descriptor.params.size() > kMaxRpcParams)
        {
            return false;
        }
        N2Net::ByteReader reader(bytes);
        args.reserve(descriptor.params.size());
        for (const FieldInfo &param : descriptor.params)
        {
            nlohmann::json value;
            if (DecodeField(param, reader, nullptr, value) != DecodeStatus::Ok || reader.Failed())
            {
                return false;
            }
            if (ValidateFieldValue(param, value).has_value())
            {
                return false;
            }
            args.push_back(ClampFieldValue(param, value));
        }
        return !reader.Failed() && reader.Remaining() == 0;
    }

    RpcLimits RpcLimits::Clamped() const noexcept
    {
        RpcLimits limits = *this;
        limits.maxPerSecond = std::clamp<std::uint32_t>(limits.maxPerSecond, 1u, 1000u);
        limits.maxPerRpcPerSecond = std::clamp<std::uint32_t>(limits.maxPerRpcPerSecond, 1u, 1000u);
        limits.maxBytesPerSecond = std::clamp<std::uint32_t>(limits.maxBytesPerSecond, 64u, 1u << 20);
        limits.maxQueued = std::clamp<std::uint32_t>(limits.maxQueued, 1u, 256u);
        limits.maxSendPerSecond = std::clamp<std::uint32_t>(limits.maxSendPerSecond, 1u, 2000u);
        return limits;
    }

    RpcBudget::Verdict RpcBudget::Admit(const std::uint64_t nowMicros, const std::uint64_t rpcKey, const std::size_t bytes,
                                        const RpcLimits &limits)
    {
        if (nowMicros < _windowStart || nowMicros - _windowStart >= kMicrosPerSecond)
        {
            _windowStart = nowMicros;
            _count = 0;
            _bytes = 0;
            _perRpc.clear();
        }
        std::uint64_t key = rpcKey;
        auto found = _perRpc.find(key);
        if (found == _perRpc.end())
        {
            if (_perRpc.size() >= kMaxTrackedRpcs)
            {
                key = kOverflowKey;
            }
            found = _perRpc.try_emplace(key, 0u).first;
        }
        if (_count >= limits.maxPerSecond || found->second >= limits.maxPerRpcPerSecond)
        {
            return Verdict::OverRate;
        }
        if (static_cast<std::uint64_t>(_bytes) + bytes > limits.maxBytesPerSecond)
        {
            return Verdict::OverBytes;
        }
        ++_count;
        ++found->second;
        _bytes += static_cast<std::uint32_t>(bytes);
        return Verdict::Allowed;
    }

    bool RpcBudget::AdmitSend(const std::uint64_t nowMicros, const RpcLimits &limits)
    {
        if (nowMicros < _send.start || nowMicros - _send.start >= kMicrosPerSecond)
        {
            _send.start = nowMicros;
            _send.count = 0;
        }
        if (_send.count >= limits.maxSendPerSecond)
        {
            return false;
        }
        ++_send.count;
        return true;
    }

    bool ThrottledLog::Allow() noexcept
    {
        ++_count;
        return _count <= 5 || _count % 1024 == 0;
    }

    void RunPendingRpc(NetworkWorld &world, const PendingRpc &call, const bool onServer, RpcStats &stats)
    {
        NetworkObject *const networked = world.Find(call.object);
        if (networked == nullptr)
        {
            ++stats.rejectedUnknown;
            return;
        }
        GameObject &object = networked->GetGameObject();
        const auto &components = object.GetAllComponents();
        if (call.componentIndex >= components.size() || components[call.componentIndex] == nullptr)
        {
            ++stats.rejectedUnknown;
            return;
        }
        Component &component = *components[call.componentIndex];
        if (component.IsDestroyed())
        {
            ++stats.rejectedUnknown;
            return;
        }
        const std::vector<RpcDescriptor> *const table = RpcTableOf(component);
        if (table == nullptr || call.rpcIndex >= table->size() || HashRpcTable(*table) != call.tableHash)
        {
            ++stats.rejectedUnknown;
            return;
        }
        RpcContext context;
        context.sender = call.sender;
        context.object = call.object;
        context.onServer = onServer;
        bool ok = false;
        try
        {
            ok = component.InvokeRpc((*table)[call.rpcIndex], context, call.args);
        }
        catch (const std::exception &e)
        {
            Logger::Error(std::format("RPC {}: the handler threw: {}", (*table)[call.rpcIndex].name, e.what()));
        }
        catch (...)
        {
            Logger::Error(std::format("RPC {}: the handler threw", (*table)[call.rpcIndex].name));
        }
        if (ok)
        {
            ++stats.executed;
        }
        else
        {
            ++stats.handlerFailed;
        }
    }
}
