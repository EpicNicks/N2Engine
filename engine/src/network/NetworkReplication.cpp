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
        _options.maxMessagesPerTick = std::clamp<std::uint32_t>(_options.maxMessagesPerTick, 1, 256);
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

    std::set<std::uint32_t> ServerReplicator::VisibleObjects(ClientId client) const
    {
        const auto found = _clients.find(client);
        return found == _clients.end() ? std::set<std::uint32_t>{} : _interest.Visible(client,found->second.observer);
    }

    bool ServerReplicator::SetObserver(ClientId client, NetworkId observer)
    {
        const auto found = _clients.find(client);
        if (found == _clients.end()) return false;
        found->second.observer = observer;
        return true;
    }

    void ServerReplicator::ForgetObject(const NetworkId id)
    {
        _layouts.Forget(id);
        _current.erase(id.value);
        for (auto &entry : _clients)
        {
            entry.second.baselines.erase(id.value);
            entry.second.priorities.erase(id.value);
        }
    }

    void ServerReplicator::BeginTick(const NetworkWorld &world)
    {
        _world = &world;
        _interest.Rebuild(world);
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
                                 const std::uint32_t serverTick, const std::optional<std::uint32_t> inputTick,
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
        header.tick = serverTick;
        header.hasInputTick = inputTick.has_value();
        header.inputTick = inputTick.value_or(0u);
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
                                                                            const std::size_t maxMessageBytes,
                                                                            const std::uint32_t serverTick,
                                                                            const std::optional<std::uint32_t> inputTick)
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

        const std::size_t limit = std::min(maxMessageBytes, _options.maxMessageBytes);
        if (limit < kStateUpdateHeaderBytes || !_world || !_world->HasScene()) return messages;
        const auto visible = _interest.Visible(client, state.observer);
        // Forget hidden baselines AND outstanding references to them. An old ack must not restore a hidden baseline.
        std::erase_if(state.baselines, [&](const auto &e){ return !visible.contains(e.first); });
        std::erase_if(state.priorities, [&](const auto &e){ return !visible.contains(e.first) || !_current.contains(e.first); });
        for (auto &record : state.records)
            std::erase_if(record.entries, [&](const auto &e){ return !visible.contains(e.id); });
        class References final : public IReplicationReferences
        {
        public:
            References(const NetworkWorld &w, const std::set<std::uint32_t> &v) : all(w), visible(v) {}
            std::uint32_t NetIdOf(const std::string &uuid) const override
            { const auto id=all.NetIdOf(uuid); return visible.contains(id) ? id : 0; }
            std::optional<std::string> UuidOf(std::uint32_t id) const override
            { return visible.contains(id) ? all.UuidOf(id) : std::nullopt; }
            WorldReferences all;
            const std::set<std::uint32_t> &visible;
        } references(*_world,visible);
        std::vector<std::uint32_t> order;
        for (const auto &[id, object] : _current)
        {
            if (!visible.contains(id)) { ++_stats.entriesNotRelevant; continue; }
            auto &score = state.priorities[id];
            score = std::min(score + _interest.Priority(id), 1e12);
            order.push_back(id);
        }
        std::sort(order.begin(),order.end(),[&](auto a, auto b) {
            const auto x=state.priorities.at(a), y=state.priorities.at(b);
            return x == y ? a < b : x > y;
        });
        std::size_t budget = _options.maxBytesPerTick == 0 ? limit * _options.maxMessagesPerTick :
            std::min(_options.maxBytesPerTick, limit * _options.maxMessagesPerTick);
        PendingMessage pending;
        auto flush = [&] {
            const auto size = pending.entries.empty() ? 0 : kStateUpdateHeaderBytes + pending.entries.size();
            Flush(state,pending,nowMicros,serverTick,inputTick,messages);
            budget -= size;
        };
        for (auto id : order)
        {
            auto *networkObject = _world->Find(NetworkId{id});
            if (!networkObject) continue;
            auto &go = networkObject->GetGameObject();
            const auto object = EncodeObjectState(_layouts.Get(NetworkId{id},go),go,&references);
            std::vector<std::uint8_t> bytes;
            SentEntry sent;
            bool full = false;
            if (!BuildEntry(state,id,object,bytes,sent,full)) continue;
            if (kStateUpdateHeaderBytes + bytes.size() > limit) { ++_stats.entriesTooLarge; continue; }
            if (!pending.sent.empty() && kStateUpdateHeaderBytes + pending.entries.size() + bytes.size() > limit)
                flush();
            if (messages.size() >= _options.maxMessagesPerTick ||
                kStateUpdateHeaderBytes + pending.entries.size() + bytes.size() > budget)
            { ++_stats.entriesBudgetDeferred; continue; }
            pending.entries.insert(pending.entries.end(),bytes.begin(),bytes.end());
            pending.sent.push_back(std::move(sent));
            state.priorities[id] = 0;
            if (full) ++_stats.fullEntriesSent;
        }
        flush();
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
        _currentInputTick = view.header.hasInputTick ? std::optional<std::uint32_t>(view.header.inputTick) : std::nullopt;
        if (_messageObserver)
        {
            _messageObserver(view.header.tick);
        }

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
                if (_entryObserver)
                {
                    _entryObserver(*object, view.header.tick);
                }
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
