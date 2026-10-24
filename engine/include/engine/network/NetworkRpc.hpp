#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/NetworkHash.hpp"
#include "engine/network/ReplicationCodec.hpp"
#include "engine/network/NetworkId.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine
{
    class Component;
    class GameObject;
}

namespace N2Engine::Network
{
    class NetworkWorld;

    // Remote procedure calls (netcode stage B7). A component declares RPCs: a name, who may call it, whether it is
    // reliable, and a list of typed parameters (the FieldKinds of the replication codecs, ReplicationCodec.hpp). Both
    // ends compute the same list from the same declaration, so the wire carries no names or types:
    //
    //   server to client   0x06 (reliable channel)   or 0x14 (unreliable channel)
    //   client to server   0x86 (reliable channel)   or 0x87 (unreliable channel)
    //     u8 type, varint net id (not 0), varint component index (the position in GameObject::GetAllComponents()),
    //     u8 rpc index (the position in the component's RPC list, sorted by name), u16 table hash (of that list),
    //     then the arguments, one wire value per parameter in order (ReplicationCodec.hpp), and nothing after them.
    //   No message is larger than kMaxRpcMessageBytes.

    /// The largest RPC message either side sends or accepts (under kMaxMessageBytes, the reliable message size)
    inline constexpr std::size_t kMaxRpcMessageBytes = 480;
    /// The most parameters one RPC has
    inline constexpr std::size_t kMaxRpcParams = 8;
    /// The most RPCs one component declares (the rpc index is one byte)
    inline constexpr std::size_t kMaxRpcsPerComponent = 32;
    /// The longest RPC name, in characters
    inline constexpr std::size_t kMaxRpcNameLength = 32;

    /// The type bytes of the RPC messages
    inline constexpr std::uint8_t kRpcServerReliableType = 0x06;
    inline constexpr std::uint8_t kRpcServerUnreliableType = 0x14;
    inline constexpr std::uint8_t kRpcClientReliableType = 0x86;
    inline constexpr std::uint8_t kRpcClientUnreliableType = 0x87;

    /// Who may call an RPC, and where it runs
    enum class RpcDirection : std::uint8_t
    {
        /// The server calls it, every client (or the one named) runs it
        ServerToClient,
        /// A client calls it on an object it owns, the server runs it
        ClientToServer,
        /// The server calls it, only the client that owns the object runs it
        ServerToOwner,
    };

    [[nodiscard]] std::string_view ToString(RpcDirection direction) noexcept;

    /// Who a server's call goes to
    enum class RpcTarget : std::uint8_t
    {
        /// Every client that has joined (ServerToClient only)
        All,
        /// The client given
        Client,
        /// The owner of the object (the only target of ServerToOwner)
        Owner,
    };

    /// What a handler is told about the call
    struct RpcContext
    {
        /// The client that called (kServerClientId when the server did)
        ClientId sender = kServerClientId;
        NetworkId object;
        /// True when the handler runs on the server
        bool onServer = false;
    };

    /// The handler of a C++ RPC: false when the arguments could not be converted to the handler's types
    using RpcHandler = std::function<bool(Component &, const RpcContext &, const std::vector<nlohmann::json> &)>;

    struct RpcDescriptor
    {
        std::string name;
        RpcDirection direction = RpcDirection::ClientToServer;
        bool reliable = true;
        /// True when the RPC does not change the simulation (chat, an emote, a sound). Only these are allowed in the
        /// Deterministic preset (NetworkServerOptions::cosmeticRpcsOnly): a command that changes the simulation goes
        /// into the input stream there.
        bool cosmetic = false;
        std::vector<FieldInfo> params;
        /// Empty for a declaration that has no C++ handler (a Lua RPC: the component runs it, Component::InvokeRpc)
        RpcHandler handler;
    };

    /// What a declaration must satisfy. Returns the first problem, or an empty string.
    [[nodiscard]] std::string ValidateRpcDescriptor(const RpcDescriptor &descriptor);

    /// The 16 bits both ends compare before they read a message's arguments: over the table's names, directions,
    /// delivery and every parameter's wire format
    [[nodiscard]] std::uint16_t HashRpcTable(const std::vector<RpcDescriptor> &table);

    /// The C++ RPCs, by the type name of the component that declares them, each list sorted by name (a name is its index)
    class RpcRegistry
    {
    public:
        [[nodiscard]] static RpcRegistry &Instance();

        /// Adds an RPC to the component type's list. False (nothing added, the reason logged) for an invalid
        /// declaration, a name the type has already, or too many RPCs on the type.
        bool Register(const std::string &componentType, RpcDescriptor descriptor);

        /// The type's RPCs sorted by name; nullptr when it has none
        [[nodiscard]] const std::vector<RpcDescriptor> *Find(const std::string &componentType) const;

        /// Feeds the registry into the network fingerprint (the registry hash): component types in sorted order and,
        /// for each, its RPCs
        void HashInto(Fnv1a64 &hash) const;

        /// Removes every RPC of the type (tests)
        void Unregister(const std::string &componentType);

    private:
        std::map<std::string, std::vector<RpcDescriptor>> _rpcs;
    };

    /// Where an RPC call lands: the component of an object and the declaration in its table
    struct ResolvedRpc
    {
        Component *component = nullptr;
        std::size_t componentIndex = 0;
        std::size_t rpcIndex = 0;
        const RpcDescriptor *descriptor = nullptr;
    };

    /// The component's RPC table (the registry's list for its type, or a Lua component's own)
    [[nodiscard]] const std::vector<RpcDescriptor> *RpcTableOf(Component &component);

    /// Finds the component's RPC `name`; false (nothing set) when the component declares none of that name
    [[nodiscard]] bool FindRpc(GameObject &object, Component &component, std::string_view name, ResolvedRpc &out);

    /// The header of an RPC message
    struct RpcHeader
    {
        std::uint8_t type = 0;
        NetworkId object;
        std::uint32_t componentIndex = 0;
        std::uint8_t rpcIndex = 0;
        std::uint16_t tableHash = 0;
        /// The argument bytes (a view into the message)
        std::span<const std::uint8_t> args;
    };

    /// Decodes the header of an RPC message of any of the four types. Nothing is allocated; false for a message that is
    /// too long, truncated, of another type or whose net id is 0.
    [[nodiscard]] bool DecodeRpcHeader(std::span<const std::uint8_t> bytes, RpcHeader &header) noexcept;

    /// The message for a call: the header and the arguments encoded against the declaration. The arguments are checked
    /// (ValidateFieldValue) and clamped (ClampFieldValue) first. nullopt, with the reason in `error`, for a wrong number
    /// of arguments, a value that is not valid for its parameter or a message over kMaxRpcMessageBytes.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeRpcMessage(std::uint8_t type, NetworkId object,
                                                                            const ResolvedRpc &rpc, std::uint16_t tableHash,
                                                                            const std::vector<nlohmann::json> &args,
                                                                            std::string &error);

    /// Decodes the arguments against the declaration, each checked with ValidateFieldValue and clamped with
    /// ClampFieldValue; false for bytes that are not exactly the arguments (truncated, over a cap, invalid, left over).
    /// Allocates only what the declaration bounds (at most kMaxRpcParams values, strings of at most
    /// kMaxReplicatedStringBytes).
    [[nodiscard]] bool DecodeRpcArgs(const RpcDescriptor &descriptor, std::span<const std::uint8_t> bytes,
                                     std::vector<nlohmann::json> &args);

    // ---- Budgets ------------------------------------------------------------------------------------------------------

    /// What a call to CallRpc did
    enum class RpcSendResult : std::uint8_t
    {
        Sent,
        /// The component's object has no NetworkObject, or it is not registered with a world
        UnknownObject,
        /// The component declares no RPC of that name
        UnknownRpc,
        /// The side that calls may not call an RPC of that direction (a server calling ClientToServer, ...)
        WrongDirection,
        /// A client called an RPC on an object it does not own
        NotOwner,
        /// The client is not joined, or the target names nobody who can receive the call
        NoTarget,
        /// The arguments do not fit the declaration (count, type, range, size)
        BadArguments,
        /// A non-cosmetic RPC where only cosmetic ones are allowed
        NotSimulationSafe,
        /// Over the send budget of every target, or none could be sent
        OverBudget,
    };

    [[nodiscard]] std::string_view ToString(RpcSendResult result) noexcept;

    /// The limits on the RPCs one peer sends. A message over a limit is dropped and counted, never queued.
    struct RpcLimits
    {
        /// RPC messages per second from the peer, all RPCs together
        std::uint32_t maxPerSecond = 60;
        /// RPC messages per second from the peer for one RPC
        std::uint32_t maxPerRpcPerSecond = 30;
        /// RPC message bytes per second from the peer
        std::uint32_t maxBytesPerSecond = 8192;
        /// Calls decoded and waiting for the next dispatch, per peer
        std::uint32_t maxQueued = 64;
        /// RPC messages per second the local side sends to one peer
        std::uint32_t maxSendPerSecond = 120;

        [[nodiscard]] RpcLimits Clamped() const noexcept;
    };

    /// What the RPC layer did. Every count is data; nothing here throws.
    struct RpcStats
    {
        std::uint64_t sent = 0;
        std::uint64_t received = 0;
        std::uint64_t executed = 0;
        /// Dropped on arrival over the rate or byte budget
        std::uint64_t droppedRate = 0;
        std::uint64_t droppedQueueFull = 0;
        /// Refused to send: over the send budget
        std::uint64_t droppedSendBudget = 0;
        std::uint64_t sendFailed = 0;
        /// The direction does not allow the sender to call it (a client calling a ServerToClient RPC, ...)
        std::uint64_t rejectedDirection = 0;
        /// A client called an RPC on an object it does not own
        std::uint64_t rejectedAuthority = 0;
        /// An unknown object, component or rpc index, or a table hash that differs
        std::uint64_t rejectedUnknown = 0;
        /// Arguments that do not decode or validate
        std::uint64_t rejectedArguments = 0;
        /// A non-cosmetic RPC where only cosmetic ones are allowed
        std::uint64_t rejectedSimulation = 0;
        /// A handler that threw, or returned false
        std::uint64_t handlerFailed = 0;
    };

    /// The windows of one peer: total and per-RPC messages, and bytes, per second. The per-RPC table is bounded
    /// (kMaxTrackedRpcs); an RPC beyond it shares one bucket, so a peer cannot grow it.
    class RpcBudget
    {
    public:
        enum class Verdict : std::uint8_t
        {
            Allowed,
            OverRate,
            OverBytes,
        };

        static constexpr std::size_t kMaxTrackedRpcs = 256;

        [[nodiscard]] Verdict Admit(std::uint64_t nowMicros, std::uint64_t rpcKey, std::size_t bytes, const RpcLimits &limits);
        /// Counts a message the local side sends; false when it is over the send budget
        [[nodiscard]] bool AdmitSend(std::uint64_t nowMicros, const RpcLimits &limits);
        [[nodiscard]] std::size_t TrackedRpcs() const noexcept { return _perRpc.size(); }

    private:
        struct Window
        {
            std::uint64_t start = 0;
            std::uint32_t count = 0;
        };

        std::uint64_t _windowStart = 0;
        std::uint32_t _count = 0;
        std::uint32_t _bytes = 0;
        std::uint64_t _perRpcStart = 0;
        std::map<std::uint64_t, std::uint32_t> _perRpc;
        Window _send;
    };

    /// A key for the per-RPC budget: the component index and the rpc index
    [[nodiscard]] constexpr std::uint64_t RpcBudgetKey(const std::uint32_t componentIndex, const std::uint8_t rpcIndex) noexcept
    {
        return (static_cast<std::uint64_t>(componentIndex) << 8) | rpcIndex;
    }

    /// A decoded call waiting for the dispatch (the next Update, in the order the messages arrived, clients in id order)
    struct PendingRpc
    {
        ClientId sender = kServerClientId;
        NetworkId object;
        std::uint32_t componentIndex = 0;
        std::uint8_t rpcIndex = 0;
        std::uint16_t tableHash = 0;
        std::vector<nlohmann::json> args;
    };

    /// Runs a decoded call: finds the object, the component and the declaration again (any of them may be gone since
    /// the message arrived), and calls the handler under a guard. A handler that throws, or a Lua one that ran out of
    /// its budget, is counted in `stats` and does not stop anything else.
    void RunPendingRpc(NetworkWorld &world, const PendingRpc &call, bool onServer, RpcStats &stats);

    /// The logging of one kind of refusal, throttled: the first `kLogBurst` are logged, then one more per 1024
    class ThrottledLog
    {
    public:
        /// True when the caller should log this occurrence
        [[nodiscard]] bool Allow() noexcept;

    private:
        std::uint64_t _count = 0;
    };

    // ---- C++ registration -----------------------------------------------------------------------------------------------

    struct RpcOptions
    {
        RpcDirection direction = RpcDirection::ClientToServer;
        bool reliable = true;
        bool cosmetic = false;
    };

    namespace Detail
    {
        template <typename T, typename... Args, std::size_t... I>
        bool InvokeRpcMethod(T &target, void (T::*method)(const RpcContext &, Args...), const RpcContext &context,
                             const std::vector<nlohmann::json> &args, std::index_sequence<I...>)
        {
            (void)args;
            if (args.size() != sizeof...(Args))
            {
                return false;
            }
            try
            {
                (target.*method)(context, args[I].template get<std::decay_t<Args>>()...);
            }
            catch (const nlohmann::json::exception &)
            {
                return false;
            }
            return true;
        }
    }

    /// Registers `method` of component type T as an RPC of `typeName` (the name GetTypeName returns). The parameters are
    /// the method's arguments after the context; each must be a type FieldTraits maps to a replicable kind (bool, an
    /// integer, a float, a string, a vector, a quaternion, a colour or an enum registered with N2_SERIALIZE_ENUM).
    /// Returns false, with the reason logged, for an invalid declaration.
    template <typename T, typename... Args>
    bool RegisterRpc(const std::string &typeName, std::string name, void (T::*method)(const RpcContext &, Args...),
                     const RpcOptions &options = {})
    {
        RpcDescriptor descriptor;
        descriptor.name = std::move(name);
        descriptor.direction = options.direction;
        descriptor.reliable = options.reliable;
        descriptor.cosmetic = options.cosmetic;
        std::size_t index = 0;
        (void)index;
        (descriptor.params.push_back(MakeFieldInfo<std::decay_t<Args>>("arg" + std::to_string(index++))), ...);
        descriptor.handler = [method](Component &component, const RpcContext &context,
                                      const std::vector<nlohmann::json> &args) -> bool
        {
            T *const target = dynamic_cast<T *>(&component);
            if (target == nullptr)
            {
                return false;
            }
            return Detail::InvokeRpcMethod<T, Args...>(*target, method, context, args, std::index_sequence_for<Args...>{});
        };
        return RpcRegistry::Instance().Register(typeName, std::move(descriptor));
    }
}
