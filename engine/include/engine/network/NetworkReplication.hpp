#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "engine/network/NetworkId.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/InterestManagement.hpp"

namespace N2Engine::Network
{
    class NetworkWorld;
    class NetworkObject;

    /// Counters of the server's state replication since it was made. All data: nothing here throws.
    struct ServerReplicationStats
    {
        /// StateUpdate messages built, the objects they carried (one object per message at most) and the fields in them
        std::uint64_t messagesSent = 0;
        std::uint64_t entriesSent = 0;
        std::uint64_t fieldsSent = 0;
        /// Bytes of those messages
        std::uint64_t bytesSent = 0;
        /// Entries that sent every field of the object (a new object, or one whose baseline was lost)
        std::uint64_t fullEntriesSent = 0;
        /// Acks that confirmed a message not yet confirmed
        std::uint64_t messagesAcked = 0;
        /// Messages given up on (no ack in the time allowed, or more in flight than the history holds): the objects they
        /// named are sent in full again
        std::uint64_t messagesExpired = 0;
        /// Acks naming a sequence that was never sent (the client is dropped)
        std::uint64_t acksInvalid = 0;
        /// Objects skipped because one entry did not fit a message
        std::uint64_t entriesTooLarge = 0;
        /// Messages not sent because the client's 32-bit sequence is spent (a client has been sent 2^32 - 2 messages)
        std::uint64_t sequencesExhausted = 0;
        std::uint64_t entriesNotRelevant = 0;
        std::uint64_t entriesBudgetDeferred = 0;
    };

    struct ServerReplicationOptions
    {
        /// The most bytes of one StateUpdate (the header included); clamped to kMaxStateUpdateBytes
        std::size_t maxMessageBytes = kMaxStateUpdateBytes;
        /// The most StateUpdates built for one client in one tick; the objects that did not fit are first in the next
        std::uint32_t maxMessagesPerTick = 4;
        /// The most messages kept per client until acknowledged (the bound of the baseline history); clamped to [8, 256]
        std::uint32_t maxInFlightMessages = 64;
        /// A message not acknowledged after this long is given up on
        std::uint64_t recordTimeoutMicros = 2'000'000;
        /// StateUpdate byte budget per client per snapshot, including headers. 0 uses the message cap.
        std::size_t maxBytesPerTick = 0;
    };

    /**
     * The server half of field replication (netcode stage B2): what it knows each client has, and the messages that
     * bring a client up to date. It does no I/O and reads no clock: NetworkServer calls BeginTick once per send tick,
     * BuildForClient for each client that has joined, OnAck when an ack arrives, and passes the messages to the
     * connections.
     *
     * Per client and per object it keeps a BASELINE per field: the bytes the client has acknowledged (the value of the
     * newest acknowledged message that carried the field), and how many unacknowledged messages carry the field. A tick
     * sends a field when
     *   - the client has acknowledged none yet (a new object: the first message holds the whole object), or
     *   - its current bytes differ from the acknowledged ones, or
     *   - an unacknowledged message carries it (the client may already have that newer value, or may not: sending the
     *     field again is the only way the receiver's state is certain).
     * so the receiver, which applies each entry on top of what it has, always ends with the server's value, whatever was
     * lost, reordered or duplicated: the unreliable channel delivers a message at most once and never one older than
     * one already delivered, and a field that is not in a message is one both ends agree on.
     *
     * A message with no ack for recordTimeoutMicros, or one pushed out by the maxInFlightMessages newer ones, is given up
     * on: every object it named loses its baseline and goes out in full (a late arrival of the old message can then not
     * leave a stale value behind: the full state overwrites it). The history is bounded (maxInFlightMessages messages of
     * at most maxMessageBytes per client), and so is each baseline (at most kMaxReplicatedFieldsPerObject fields, each at
     * most MaxEncodedBytes long), and an object's baseline is dropped when the object is despawned.
     */
    class ServerReplicator
    {
    public:
        explicit ServerReplicator(ServerReplicationOptions options = {});

        void AddClient(ClientId client);
        void RemoveClient(ClientId client);
        bool SetObserver(ClientId client, NetworkId observer);
        [[nodiscard]] std::set<std::uint32_t> VisibleObjects(ClientId client) const;
        [[nodiscard]] bool HasClient(ClientId client) const { return _clients.contains(client); }

        /// The object is gone: its baselines go
        void ForgetObject(NetworkId id);

        /// Reads and encodes the current state of every object of the world that has replicated fields. Call once per
        /// tick before BuildForClient. Objects are visited in net id order and fields in layout order, so the same state
        /// always encodes to the same bytes.
        void BeginTick(const NetworkWorld &world);

        /// The StateUpdates for `client` this tick (each a complete message for the unreliable channel), at most
        /// maxMessagesPerTick of them, none when nothing changed since what the client acknowledged. `nowMicros`
        /// stamps the messages (their ack deadline). `maxMessageBytes` caps this client's message size (the connection's
        /// largest unreliable message), at most the options' value. `serverTick` is the server's fixed tick (32 bits)
        /// the states are read on: it goes into every message's header (netcode stage B3), where the client's
        /// interpolation reads it. `inputTick` (netcode stage B6) is the stamp of the newest input frame applied to this
        /// client's objects, or nullopt: it goes into every message's header, where the client's prediction reads it.
        [[nodiscard]] std::vector<std::vector<std::uint8_t>> BuildForClient(
            ClientId client, std::uint64_t nowMicros, std::size_t maxMessageBytes, std::uint32_t serverTick = 0,
            std::optional<std::uint32_t> inputTick = std::nullopt);

        /// Applies a client's ack. False when it names a sequence that was never sent (nothing is applied).
        bool OnAck(ClientId client, const StateAckMessage &ack);

        [[nodiscard]] const ServerReplicationStats &Stats() const noexcept { return _stats; }
        /// Unacknowledged messages kept for the client (0 for an unknown client)
        [[nodiscard]] std::size_t InFlightMessages(ClientId client) const;
        /// Objects the client has a baseline for
        [[nodiscard]] std::size_t BaselineCount(ClientId client) const;
        /// Objects with replicated fields at the last BeginTick
        [[nodiscard]] std::size_t ReplicatedObjectCount() const noexcept { return _current.size(); }

    private:
        struct FieldBaseline
        {
            std::vector<std::uint8_t> bytes;
            /// The sequence of the message `bytes` came from; 0 when the client has acknowledged none
            std::uint32_t ackedSequence = 0;
            /// Unacknowledged messages that carry this field
            std::uint32_t inFlight = 0;
        };

        struct ObjectBaseline
        {
            std::uint16_t layoutHash = 0;
            std::vector<FieldBaseline> fields;
        };

        struct SentField
        {
            std::uint8_t index = 0;
            std::vector<std::uint8_t> bytes;
        };

        struct SentEntry
        {
            std::uint32_t id = 0;
            std::uint16_t layoutHash = 0;
            std::vector<SentField> fields;
        };

        struct SentRecord
        {
            std::uint32_t sequence = 0;
            std::uint64_t sentAt = 0;
            std::vector<SentEntry> entries;
        };

        struct ClientState
        {
            std::map<std::uint32_t, ObjectBaseline> baselines;
            std::deque<SentRecord> records;
            std::uint32_t nextSequence = 1;
            /// The first net id the next tick looks at (0: from the start)
            std::uint32_t cursor = 0;
            NetworkId observer;
            std::map<std::uint32_t, double> priorities;
        };

        struct PendingMessage
        {
            std::vector<std::uint8_t> entries;
            std::vector<SentEntry> sent;
        };

        void ExpireRecords(ClientState &state, std::uint64_t nowMicros);
        void ExpireFront(ClientState &state);
        void ApplyRecord(ClientState &state, const SentRecord &record);
        void Flush(ClientState &state, PendingMessage &pending, std::uint64_t nowMicros, std::uint32_t serverTick,
                   std::optional<std::uint32_t> inputTick, std::vector<std::vector<std::uint8_t>> &messages);
        /// The entry for one object (its bytes and what it records), or false when nothing changed
        bool BuildEntry(ClientState &state, std::uint32_t id, const ObjectState &object,
                        std::vector<std::uint8_t> &entryBytes, SentEntry &sent, bool &full);

        ServerReplicationOptions _options;
        ServerReplicationStats _stats;
        LayoutCache _layouts;
        std::map<std::uint32_t, ObjectState> _current;
        std::map<ClientId, ClientState> _clients;
        InterestGrid _interest;
        const NetworkWorld *_world = nullptr;
    };

    /// Counters of the client's state replication since it was made
    struct ClientReplicationStats
    {
        /// Messages applied whole (and acknowledged), applied in part (an entry skipped or refused: not acknowledged),
        /// refused as malformed, ignored as older than one already seen
        std::uint64_t messagesApplied = 0;
        std::uint64_t messagesPartial = 0;
        std::uint64_t messagesMalformed = 0;
        std::uint64_t messagesStale = 0;
        std::uint64_t entriesApplied = 0;
        /// An entry for an object this client does not have (yet)
        std::uint64_t entriesUnknownObject = 0;
        /// An entry written for another layout than this client's for the object
        std::uint64_t entriesLayoutMismatch = 0;
        /// An entry holding a reference to an object this client does not have
        std::uint64_t entriesUnknownReference = 0;
        std::uint64_t entriesMalformed = 0;
        /// A component refused a value the server sent
        std::uint64_t valuesRefused = 0;
    };

    /**
     * The client half of field replication: applies a StateUpdate to the world and keeps what it needs to acknowledge.
     * It only ever writes the replicated fields of objects of the world it was given, and only from a StateUpdate the
     * caller passes (NetworkClient passes the server's); nothing a client sends is ever read by it.
     *
     * A message is acknowledged only when every entry in it was applied: an entry it could not use (an object the client
     * does not have yet, because the reliable spawn is still on its way) leaves the message unacknowledged, and the
     * server, which treats an unacknowledged message as not received, keeps those fields in what it sends.
     */
    class ClientReplicator
    {
    public:
        enum class Result : std::uint8_t
        {
            /// Every entry applied; the message is acknowledged
            Applied,
            /// The message was well-formed, but some entry was skipped or refused: not acknowledged
            Partial,
            /// The message is not a valid StateUpdate
            Malformed,
            /// Not newer than one already seen
            Stale,
        };

        /// Applies one StateUpdate (the bytes of an unreliable message) to `world`
        Result OnStateUpdate(const NetworkWorld &world, std::span<const std::uint8_t> message);

        /// Called for each StateUpdate that is newer than any seen, before its entries are applied, with the server tick
        /// in its header (netcode stage B3: the snapshot interpolation hangs on it)
        void SetMessageObserver(std::function<void(std::uint32_t)> observer) { _messageObserver = std::move(observer); }
        /// Called for each entry applied whole, with the object and the tick of the message; the object's fields hold the
        /// server's values when it runs. It may not call OnStateUpdate.
        void SetEntryObserver(std::function<void(NetworkObject &, std::uint32_t)> observer)
        {
            _entryObserver = std::move(observer);
        }

        /// The input tick in the header of the message being applied (netcode stage B6: the stamp of the newest input the
        /// server applied to this client's objects; the entries' values include that input and none after it). Valid
        /// inside the entry observer; nullopt when the message named none.
        [[nodiscard]] std::optional<std::uint32_t> CurrentInputTick() const noexcept { return _currentInputTick; }

        /// The object is gone: its layout goes
        void ForgetObject(NetworkId id) { _layouts.Forget(id); }

        [[nodiscard]] bool AckPending() const noexcept { return _ackPending; }
        /// The ack to send now (the newest applied sequence and the 32 before it); clears the pending flag
        [[nodiscard]] StateAckMessage TakeAck() noexcept
        {
            _ackPending = false;
            return StateAckMessage{_latest, _bits};
        }

        [[nodiscard]] std::uint32_t LatestApplied() const noexcept { return _latest; }
        [[nodiscard]] const ClientReplicationStats &Stats() const noexcept { return _stats; }

    private:
        void MarkApplied(std::uint32_t sequence) noexcept;

        LayoutCache _layouts;
        ClientReplicationStats _stats;
        std::uint32_t _highestSeen = 0;
        std::uint32_t _latest = 0;
        std::uint32_t _bits = 0;
        bool _ackPending = false;
        std::optional<std::uint32_t> _currentInputTick;
        std::function<void(std::uint32_t)> _messageObserver;
        std::function<void(NetworkObject &, std::uint32_t)> _entryObserver;
    };
}
