#include "engine/network/NetworkReplication.hpp"

#include <algorithm>
#include <bit>
#include <limits>

#include "engine/GameObject.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationCodec.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr std::uint32_t kMinInFlightMessages = 8;
        constexpr std::uint32_t kMaxInFlightMessagesCap = 256;
        /// The smallest message limit that can hold the header and any one entry of a small object
        constexpr std::size_t kMinMessageBytes = 64;
    }

    ServerReplicator::ServerReplicator(ServerReplicationOptions options) : _options(options)
    {
        _options.maxMessageBytes = std::clamp<std::size_t>(_options.maxMessageBytes, kMinMessageBytes, kMaxStateUpdateBytes);
        _options.maxMessagesPerTick = std::max<std::uint32_t>(_options.maxMessagesPerTick, 1);
        _options.maxInFlightMessages =
            std::clamp<std::uint32_t>(_options.maxInFlightMessages, kMinInFlightMessages, kMaxInFlightMessagesCap);
    }

    void ServerReplicator::AddClient(const ClientId client)
    {
        _clients.try_emplace(client);
    }

    void ServerReplicator::RemoveClient(const ClientId client)
    {
        _clients.erase(client);
    }

    void ServerReplicator::ForgetObject(const NetworkId id)
    {
        _layouts.Forget(id);
        _current.erase(id.value);
        for (auto &entry : _clients)
        {
            entry.second.baselines.erase(id.value);
        }
    }

    void ServerReplicator::BeginTick(const NetworkWorld &world)
    {
        _current.clear();
        const WorldReferences references(world);
        for (const auto &entry : world.Objects())
        {
            GameObject &gameObject = entry.second->GetGameObject();
            const ObjectLayout &layout = _layouts.Get(NetworkId{entry.first}, gameObject);
            if (!layout.usable)
            {
                continue;
            }
            _current.emplace(entry.first, EncodeObjectState(layout, gameObject, &references));
        }
    }

    bool ServerReplicator::BuildEntry(ClientState &state, const std::uint32_t id, const ObjectState &object,
                                      std::vector<std::uint8_t> &entryBytes, SentEntry &sent, bool &full)
    {
        ObjectBaseline &baseline = state.baselines[id];
        if (baseline.layoutHash != object.layoutHash || baseline.fields.size() != object.fields.size())
        {
            baseline.layoutHash = object.layoutHash;
            baseline.fields.assign(object.fields.size(), FieldBaseline{});
        }

        std::uint64_t mask = 0;
        for (std::size_t i = 0; i < object.fields.size(); ++i)
        {
            const std::vector<std::uint8_t> &bytes = object.fields[i];
            if (bytes.empty())
            {
                continue;
            }
            const FieldBaseline &known = baseline.fields[i];
            if (known.ackedSequence == 0 || known.inFlight > 0 || known.bytes != bytes)
            {
                mask |= std::uint64_t{1} << i;
            }
        }
        if (mask == 0)
        {
            return false;
        }
        full = static_cast<std::size_t>(std::popcount(mask)) == object.fields.size();

        std::vector<std::uint8_t> payload;
        const std::size_t maskBytes = (object.fields.size() + 7) / 8;
        for (std::size_t b = 0; b < maskBytes; ++b)
        {
            payload.push_back(static_cast<std::uint8_t>((mask >> (8 * b)) & 0xFFu));
        }
        sent.id = id;
        sent.layoutHash = object.layoutHash;
        sent.fields.clear();
        for (std::size_t i = 0; i < object.fields.size(); ++i)
        {
            if (((mask >> i) & 1u) == 0)
            {
                continue;
            }
            payload.insert(payload.end(), object.fields[i].begin(), object.fields[i].end());
            SentField field;
            field.index = static_cast<std::uint8_t>(i);
            field.bytes = object.fields[i];
            sent.fields.push_back(std::move(field));
        }

        entryBytes.clear();
        WireWriter writer(entryBytes);
        writer.Varint(id);
        writer.U16(object.layoutHash);
        writer.Varint(payload.size());
        writer.Bytes(payload);
        return true;
    }

    void ServerReplicator::ExpireFront(ClientState &state)
    {
        const SentRecord &record = state.records.front();
        for (const SentEntry &entry : record.entries)
        {
            const auto found = state.baselines.find(entry.id);
            if (found == state.baselines.end() || found->second.layoutHash != entry.layoutHash)
            {
                continue;
            }
            ObjectBaseline &baseline = found->second;
            for (const SentField &field : entry.fields)
            {
                if (field.index < baseline.fields.size() && baseline.fields[field.index].inFlight > 0)
                {
                    --baseline.fields[field.index].inFlight;
                }
            }
            // Whether the message arrived is not known, so what the client holds is not either: the object goes out in
            // full until a message that carries every field is acknowledged
            for (FieldBaseline &field : baseline.fields)
            {
                field.ackedSequence = 0;
                field.bytes.clear();
            }
        }
        ++_stats.messagesExpired;
        state.records.pop_front();
    }

    void ServerReplicator::ExpireRecords(ClientState &state, const std::uint64_t nowMicros)
    {
        while (!state.records.empty() && (state.records.size() > _options.maxInFlightMessages ||
                                          (nowMicros > state.records.front().sentAt &&
                                           nowMicros - state.records.front().sentAt > _options.recordTimeoutMicros)))
        {
            ExpireFront(state);
        }
    }

    void ServerReplicator::ApplyRecord(ClientState &state, const SentRecord &record)
    {
        for (const SentEntry &entry : record.entries)
        {
            const auto found = state.baselines.find(entry.id);
            if (found == state.baselines.end() || found->second.layoutHash != entry.layoutHash)
            {
                continue;
            }
            ObjectBaseline &baseline = found->second;
            for (const SentField &field : entry.fields)
            {
                if (field.index >= baseline.fields.size())
                {
                    continue;
                }
                FieldBaseline &known = baseline.fields[field.index];
                if (known.inFlight > 0)
                {
                    --known.inFlight;
                }
                // An ack of an older message after a newer one's must not take the baseline back
                if (record.sequence > known.ackedSequence)
                {
                    known.bytes = field.bytes;
                    known.ackedSequence = record.sequence;
                }
            }
        }
    }

    void ServerReplicator::Flush(ClientState &state, PendingMessage &pending, const std::uint64_t nowMicros,
                                 std::vector<std::vector<std::uint8_t>> &messages)
    {
        if (pending.sent.empty())
        {
            return;
        }
        if (state.nextSequence == std::numeric_limits<std::uint32_t>::max())
        {
            // The 32-bit sequence is spent: nothing more goes to this client (a sequence that wrapped would look stale to
            // it, and 0 is not a sequence)
            ++_stats.sequencesExhausted;
            pending.entries.clear();
            pending.sent.clear();
            return;
        }
        // Room for one more: the oldest message still waiting is given up on (the history is bounded)
        while (state.records.size() >= _options.maxInFlightMessages)
        {
            ExpireFront(state);
        }

        SentRecord record;
        record.sequence = state.nextSequence;
        ++state.nextSequence;
        record.sentAt = nowMicros;
        record.entries = std::move(pending.sent);

        StateUpdateHeader header;
        header.sequence = record.sequence;
        header.entryCount = static_cast<std::uint16_t>(record.entries.size());
        std::vector<std::uint8_t> message = EncodeStateUpdateHeader(header);
        message.insert(message.end(), pending.entries.begin(), pending.entries.end());

        std::uint64_t fields = 0;
        for (const SentEntry &entry : record.entries)
        {
            fields += entry.fields.size();
            const auto found = state.baselines.find(entry.id);
            if (found == state.baselines.end() || found->second.layoutHash != entry.layoutHash)
            {
                continue;
            }
            for (const SentField &field : entry.fields)
            {
                if (field.index < found->second.fields.size())
                {
                    ++found->second.fields[field.index].inFlight;
                }
            }
        }
        ++_stats.messagesSent;
        _stats.entriesSent += record.entries.size();
        _stats.fieldsSent += fields;
        _stats.bytesSent += message.size();

        state.records.push_back(std::move(record));
        messages.push_back(std::move(message));
        pending.entries.clear();
        pending.sent.clear();
    }

    std::vector<std::vector<std::uint8_t>> ServerReplicator::BuildForClient(const ClientId client,
                                                                            const std::uint64_t nowMicros,
                                                                            const std::size_t maxMessageBytes)
    {
        std::vector<std::vector<std::uint8_t>> messages;
        const auto found = _clients.find(client);
        if (found == _clients.end())
        {
            return messages;
        }
        ClientState &state = found->second;
        ExpireRecords(state, nowMicros);
        if (_current.empty())
        {
            state.cursor = 0;
            return messages;
        }

        const std::size_t limit = std::max(std::min(maxMessageBytes, _options.maxMessageBytes), kMinMessageBytes);
        PendingMessage pending;
        auto position = _current.lower_bound(state.cursor);
        std::uint32_t nextCursor = 0;
        bool stopped = false;
        for (std::size_t visited = 0; visited < _current.size(); ++visited)
        {
            if (position == _current.end())
            {
                position = _current.begin();
            }
            const std::uint32_t id = position->first;
            std::vector<std::uint8_t> entryBytes;
            SentEntry sent;
            bool full = false;
            if (BuildEntry(state, id, position->second, entryBytes, sent, full))
            {
                if (kStateUpdateHeaderBytes + entryBytes.size() > limit)
                {
                    ++_stats.entriesTooLarge;
                }
                else
                {
                    if (kStateUpdateHeaderBytes + pending.entries.size() + entryBytes.size() > limit)
                    {
                        Flush(state, pending, nowMicros, messages);
                        if (messages.size() >= _options.maxMessagesPerTick)
                        {
                            nextCursor = id;
                            stopped = true;
                            break;
                        }
                    }
                    pending.entries.insert(pending.entries.end(), entryBytes.begin(), entryBytes.end());
                    pending.sent.push_back(std::move(sent));
                    if (full)
                    {
                        ++_stats.fullEntriesSent;
                    }
                }
            }
            ++position;
        }
        if (!stopped)
        {
            Flush(state, pending, nowMicros, messages);
        }
        state.cursor = nextCursor;
        return messages;
    }

    bool ServerReplicator::OnAck(const ClientId client, const StateAckMessage &ack)
    {
        const auto found = _clients.find(client);
        if (found == _clients.end())
        {
            return true;
        }
        ClientState &state = found->second;
        if (ack.latest >= state.nextSequence)
        {
            ++_stats.acksInvalid;
            return false;
        }
        const auto confirm = [&state, this](const std::uint32_t sequence)
        {
            // Records ascend by sequence
            const auto record = std::lower_bound(state.records.begin(), state.records.end(), sequence,
                                                 [](const SentRecord &candidate, const std::uint32_t wanted)
                                                 { return candidate.sequence < wanted; });
            if (record == state.records.end() || record->sequence != sequence)
            {
                return;
            }
            ApplyRecord(state, *record);
            state.records.erase(record);
            ++_stats.messagesAcked;
        };
        confirm(ack.latest);
        for (std::uint32_t i = 0; i < 32; ++i)
        {
            if (((ack.bits >> i) & 1u) != 0 && ack.latest > i + 1)
            {
                confirm(ack.latest - 1 - i);
            }
        }
        return true;
    }

    std::size_t ServerReplicator::InFlightMessages(const ClientId client) const
    {
        const auto found = _clients.find(client);
        return found == _clients.end() ? 0 : found->second.records.size();
    }

    std::size_t ServerReplicator::BaselineCount(const ClientId client) const
    {
        const auto found = _clients.find(client);
        return found == _clients.end() ? 0 : found->second.baselines.size();
    }

    ClientReplicator::Result ClientReplicator::OnStateUpdate(const NetworkWorld &world,
                                                             const std::span<const std::uint8_t> message)
    {
        const auto decoded = DecodeStateUpdate(message);
        if (!decoded.has_value())
        {
            ++_stats.messagesMalformed;
            return Result::Malformed;
        }
        const StateUpdateView &view = *decoded;
        if (view.header.sequence <= _highestSeen)
        {
            ++_stats.messagesStale;
            return Result::Stale;
        }
        _highestSeen = view.header.sequence;

        N2Net::ByteReader reader(view.entries);
        const WorldReferences references(world);
        bool partial = false;
        for (std::uint16_t e = 0; e < view.header.entryCount; ++e)
        {
            std::uint64_t id = 0;
            if (!ReadVarint(reader, id) || id == 0 || id > std::numeric_limits<std::uint32_t>::max())
            {
                ++_stats.messagesMalformed;
                return Result::Malformed;
            }
            const std::uint16_t hash = reader.U16();
            std::uint64_t length = 0;
            if (!ReadVarint(reader, length) || reader.Failed() || length == 0 || length > kMaxReplicatedEntryBytes ||
                length > reader.Remaining())
            {
                ++_stats.messagesMalformed;
                return Result::Malformed;
            }
            const std::span<const std::uint8_t> payloadBytes = reader.Bytes(static_cast<std::size_t>(length));

            const NetworkId netId{static_cast<std::uint32_t>(id)};
            NetworkObject *const object = world.Find(netId);
            if (object == nullptr)
            {
                ++_stats.entriesUnknownObject;
                partial = true;
                continue;
            }
            GameObject &gameObject = object->GetGameObject();
            const ObjectLayout &layout = _layouts.Get(netId, gameObject);
            if (!layout.usable || layout.hash != hash)
            {
                // Counted and skipped, but not a reason to withhold the acknowledgement: a layout that differs stays
                // different (a component added on the server at runtime, another Lua script), and an unacknowledged message
                // would keep every object that shares it from ever getting a baseline
                ++_stats.entriesLayoutMismatch;
                continue;
            }
            N2Net::ByteReader payload(payloadBytes);
            std::size_t refused = 0;
            const EntryResult result = ApplyEntry(layout, gameObject, payload, references, refused);
            _stats.valuesRefused += refused;
            switch (result)
            {
            case EntryResult::Applied:
                ++_stats.entriesApplied;
                break;
            case EntryResult::Skipped:
                ++_stats.entriesUnknownReference;
                partial = true;
                break;
            case EntryResult::Malformed:
                ++_stats.entriesMalformed;
                partial = true;
                break;
            }
        }
        if (reader.Remaining() != 0)
        {
            ++_stats.messagesMalformed;
            return Result::Malformed;
        }
        if (partial)
        {
            ++_stats.messagesPartial;
            return Result::Partial;
        }
        MarkApplied(view.header.sequence);
        ++_stats.messagesApplied;
        return Result::Applied;
    }

    void ClientReplicator::MarkApplied(const std::uint32_t sequence) noexcept
    {
        if (_latest == 0)
        {
            _latest = sequence;
            _bits = 0;
        }
        else if (sequence > _latest)
        {
            const std::uint32_t delta = sequence - _latest;
            if (delta >= 33)
            {
                _bits = 0;
            }
            else
            {
                const std::uint64_t widened = (static_cast<std::uint64_t>(_bits) << delta) | (std::uint64_t{1} << (delta - 1));
                _bits = static_cast<std::uint32_t>(widened & 0xFFFFFFFFull);
            }
            _latest = sequence;
        }
        else
        {
            return;
        }
        _ackPending = true;
    }
}
