#include "engine/network/n2net/BisectProtocol.hpp"

#include <utility>

#include "engine/network/n2net/ByteIO.hpp"

namespace N2Engine::N2Net
{
    namespace
    {
        void U64(ByteWriter &writer, const std::uint64_t value) noexcept
        {
            writer.U32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
            writer.U32(static_cast<std::uint32_t>(value >> 32));
        }

        std::uint64_t ReadU64(ByteReader &reader) noexcept
        {
            const std::uint64_t low = reader.U32();
            const std::uint64_t high = reader.U32();
            return low | (high << 32);
        }

        bool ValidOp(const std::uint8_t op) noexcept
        {
            return op <= static_cast<std::uint8_t>(BisectOp::Fields);
        }

        bool StatusFitsOp(const BisectStatus status, const BisectOp op) noexcept
        {
            switch (status)
            {
            case BisectStatus::Ok:
            case BisectStatus::UnknownTick:
                return true;
            case BisectStatus::TooMany:
                return op == BisectOp::Objects;
            case BisectStatus::NoSuchObject:
                return op == BisectOp::Fields;
            }
            return false;
        }
    }

    std::size_t EncodeBisectRequest(const BisectRequest &request, const std::span<std::uint8_t> out) noexcept
    {
        if (out.size() < kBisectRequestSize || static_cast<std::uint8_t>(request.op) > static_cast<std::uint8_t>(BisectOp::Fields))
        {
            return 0;
        }
        if (request.op == BisectOp::Fields ? request.hi != 0 : request.lo > request.hi)
        {
            return 0;
        }
        ByteWriter writer(out.data(), out.size());
        writer.U8(kBisectRequestKind);
        writer.U32(request.requestId);
        writer.U8(static_cast<std::uint8_t>(request.op));
        writer.U32(request.tick);
        writer.U32(request.lo);
        writer.U32(request.hi);
        return writer.Size();
    }

    std::optional<BisectRequest> DecodeBisectRequest(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (bytes.size() != kBisectRequestSize)
        {
            return std::nullopt;
        }
        ByteReader reader(bytes);
        if (reader.U8() != kBisectRequestKind)
        {
            return std::nullopt;
        }
        BisectRequest request;
        request.requestId = reader.U32();
        const std::uint8_t op = reader.U8();
        if (!ValidOp(op))
        {
            return std::nullopt;
        }
        request.op = static_cast<BisectOp>(op);
        request.tick = reader.U32();
        request.lo = reader.U32();
        request.hi = reader.U32();
        if (request.op == BisectOp::Fields ? request.hi != 0 : request.lo > request.hi)
        {
            return std::nullopt;
        }
        return request;
    }

    std::size_t EncodeBisectReply(const BisectReply &reply, const std::span<std::uint8_t> out) noexcept
    {
        if (!ValidOp(static_cast<std::uint8_t>(reply.op)) || !StatusFitsOp(reply.status, reply.op))
        {
            return 0;
        }
        const bool ok = reply.status == BisectStatus::Ok;
        std::size_t size = kBisectReplyHeaderSize;
        if (ok)
        {
            switch (reply.op)
            {
            case BisectOp::Range:
                size += 4 + 8;
                break;
            case BisectOp::Objects:
                if (reply.objects.size() > kMaxReplyObjects)
                {
                    return 0;
                }
                size += 1 + reply.objects.size() * kBisectObjectEntrySize;
                break;
            case BisectOp::Fields:
                if (reply.fields.size() > kMaxFieldsPerObject)
                {
                    return 0;
                }
                size += 2 + reply.fields.size() * kBisectFieldEntrySize;
                break;
            }
        }
        if (out.size() < size)
        {
            return 0;
        }
        ByteWriter writer(out.data(), out.size());
        writer.U8(kBisectReplyKind);
        writer.U32(reply.requestId);
        writer.U8(static_cast<std::uint8_t>(reply.op));
        writer.U8(static_cast<std::uint8_t>(reply.status));
        if (ok)
        {
            switch (reply.op)
            {
            case BisectOp::Range:
                writer.U32(reply.range.count);
                U64(writer, reply.range.hash);
                break;
            case BisectOp::Objects:
                writer.U8(static_cast<std::uint8_t>(reply.objects.size()));
                for (const ObjectDigest &object : reply.objects)
                {
                    writer.U32(object.id);
                    U64(writer, object.hash);
                }
                break;
            case BisectOp::Fields:
                writer.U16(static_cast<std::uint16_t>(reply.fields.size()));
                for (const HashLeaf &leaf : reply.fields)
                {
                    writer.U16(leaf.fieldId);
                    U64(writer, leaf.hash);
                }
                break;
            }
        }
        return writer.Size();
    }

    std::optional<BisectReply> DecodeBisectReply(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() < kBisectReplyHeaderSize || bytes.size() > kMaxBisectReplySize)
        {
            return std::nullopt;
        }
        ByteReader reader(bytes);
        if (reader.U8() != kBisectReplyKind)
        {
            return std::nullopt;
        }
        BisectReply reply;
        reply.requestId = reader.U32();
        const std::uint8_t op = reader.U8();
        const std::uint8_t status = reader.U8();
        if (!ValidOp(op) || status > static_cast<std::uint8_t>(BisectStatus::NoSuchObject))
        {
            return std::nullopt;
        }
        reply.op = static_cast<BisectOp>(op);
        reply.status = static_cast<BisectStatus>(status);
        if (!StatusFitsOp(reply.status, reply.op))
        {
            return std::nullopt;
        }
        if (reply.status != BisectStatus::Ok)
        {
            return reader.Remaining() == 0 ? std::optional<BisectReply>(std::move(reply)) : std::nullopt;
        }
        switch (reply.op)
        {
        case BisectOp::Range:
            if (reader.Remaining() != 12)
            {
                return std::nullopt;
            }
            reply.range.count = reader.U32();
            reply.range.hash = ReadU64(reader);
            break;
        case BisectOp::Objects:
        {
            const std::size_t count = reader.U8();
            if (reader.Failed() || count > kMaxReplyObjects || reader.Remaining() != count * kBisectObjectEntrySize)
            {
                return std::nullopt;
            }
            reply.objects.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                ObjectDigest object;
                object.id = reader.U32();
                object.hash = ReadU64(reader);
                reply.objects.push_back(object);
            }
            break;
        }
        case BisectOp::Fields:
        {
            const std::size_t count = reader.U16();
            if (reader.Failed() || count > kMaxFieldsPerObject || reader.Remaining() != count * kBisectFieldEntrySize)
            {
                return std::nullopt;
            }
            reply.fields.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                HashLeaf leaf;
                leaf.fieldId = reader.U16();
                leaf.hash = ReadU64(reader);
                reply.fields.push_back(leaf);
            }
            break;
        }
        }
        if (reader.Failed() || reader.Remaining() != 0)
        {
            return std::nullopt;
        }
        return reply;
    }

    // ---- responder ----

    BisectResponder::BisectResponder(HashTreeHistory &history, const BisectResponderConfig &config,
                                     const std::uint64_t nowMicros) noexcept
        : history_(history), config_(config), lastRefill_(nowMicros)
    {
        if (config_.burst < 1)
        {
            config_.burst = 1;
        }
        if (config_.refillMicros < 1)
        {
            config_.refillMicros = 1;
        }
        tokens_ = config_.burst;
    }

    ServeResult BisectResponder::Serve(const std::uint64_t nowMicros, const std::span<const std::uint8_t> request,
                                       std::span<const std::uint8_t> &reply)
    {
        reply = {};
        if (Abusive())
        {
            return ServeResult::Abusive;
        }
        // Refill: one token per refillMicros since the last refill, kept to the burst
        if (nowMicros > lastRefill_)
        {
            const std::uint64_t elapsed = nowMicros - lastRefill_ + refillRemainder_;
            const std::uint64_t gained = elapsed / config_.refillMicros;
            refillRemainder_ = elapsed % config_.refillMicros;
            lastRefill_ = nowMicros;
            const std::uint64_t total = tokens_ + gained;
            tokens_ = total > config_.burst ? config_.burst : static_cast<std::uint32_t>(total);
            if (tokens_ == config_.burst)
            {
                refillRemainder_ = 0;
            }
        }
        if (tokens_ == 0)
        {
            ++rateLimited_;
            ++violations_;
            return Abusive() ? ServeResult::Abusive : ServeResult::RateLimited;
        }
        --tokens_;
        const std::optional<BisectRequest> decoded = DecodeBisectRequest(request);
        if (!decoded)
        {
            ++malformed_;
            ++violations_;
            return Abusive() ? ServeResult::Abusive : ServeResult::Malformed;
        }

        BisectReply out;
        out.requestId = decoded->requestId;
        out.op = decoded->op;
        HashTree *tree = history_.Find(decoded->tick);
        if (tree == nullptr)
        {
            out.status = BisectStatus::UnknownTick;
        }
        else
        {
            switch (decoded->op)
            {
            case BisectOp::Range:
                tree->Range(decoded->lo, decoded->hi, out.range);
                break;
            case BisectOp::Objects:
                if (!tree->Objects(decoded->lo, decoded->hi, out.objects))
                {
                    out.objects.clear();
                    out.status = BisectStatus::TooMany;
                }
                break;
            case BisectOp::Fields:
                if (!tree->Fields(decoded->lo, out.fields))
                {
                    out.fields.clear();
                    out.status = BisectStatus::NoSuchObject;
                }
                break;
            }
        }
        const std::size_t size = EncodeBisectReply(out, buffer_);
        if (size == 0)
        {
            // Our own tree broke a cap (cannot happen for trees the builder made): refuse rather than send garbage
            ++malformed_;
            return ServeResult::Malformed;
        }
        ++answered_;
        reply = std::span<const std::uint8_t>(buffer_.data(), size);
        return ServeResult::Replied;
    }

    ServeResult BisectResponder::ServeOn(const std::uint64_t nowMicros, const std::span<const std::uint8_t> request,
                                         Connection &connection)
    {
        std::span<const std::uint8_t> reply;
        const ServeResult result = Serve(nowMicros, request, reply);
        if (result == ServeResult::Replied)
        {
            connection.SendReliable(reply);
        }
        return result;
    }

    // ---- client ----

    BisectClient::BisectClient(Connection &connection, const BisectClientConfig &config) noexcept
        : connection_(connection), config_(config)
    {
        if (config_.maxOutstanding < 1)
        {
            config_.maxOutstanding = 1;
        }
        if (config_.maxOutstanding > kMaxBisectOutstanding)
        {
            config_.maxOutstanding = kMaxBisectOutstanding;
        }
    }

    std::uint32_t BisectClient::Outstanding() const noexcept
    {
        std::uint32_t count = 0;
        for (const Pending &pending : table_)
        {
            count += pending.used ? 1u : 0u;
        }
        return count;
    }

    std::optional<std::uint32_t> BisectClient::Submit(const std::uint64_t nowMicros, const BisectOp op,
                                                      const std::uint32_t tick, const std::uint32_t lo,
                                                      const std::uint32_t hi)
    {
        if (failure_ != BisectFailure::None)
        {
            return std::nullopt;
        }
        if (sent_ >= config_.maxRequests)
        {
            Fail(BisectFailure::BudgetExceeded);
            return std::nullopt;
        }
        if (Outstanding() >= config_.maxOutstanding)
        {
            return std::nullopt;
        }
        Pending *slot = nullptr;
        for (Pending &pending : table_)
        {
            if (!pending.used)
            {
                slot = &pending;
                break;
            }
        }
        if (slot == nullptr)
        {
            return std::nullopt;
        }
        BisectRequest request;
        request.requestId = nextId_;
        request.op = op;
        request.tick = tick;
        request.lo = lo;
        request.hi = op == BisectOp::Fields ? 0 : hi;
        std::array<std::uint8_t, kBisectRequestSize> buffer{};
        const std::size_t size = EncodeBisectRequest(request, buffer);
        if (size == 0 || connection_.SendReliable(std::span<const std::uint8_t>(buffer.data(), size)) != SendResult::Ok)
        {
            Fail(BisectFailure::ConnectionFailed);
            return std::nullopt;
        }
        ++nextId_;
        ++sent_;
        *slot = Pending{};
        slot->used = true;
        slot->id = request.requestId;
        slot->op = op;
        slot->lo = request.lo;
        slot->hi = request.hi;
        slot->sentAt = nowMicros;
        return request.requestId;
    }

    bool BisectClient::OnMessage(const std::uint64_t, const std::span<const std::uint8_t> message)
    {
        if (failure_ != BisectFailure::None)
        {
            return false;
        }
        // Match on the id before decoding the payload: a reply nobody asked for costs nothing
        if (message.size() < kBisectReplyHeaderSize || message[0] != kBisectReplyKind)
        {
            Fail(BisectFailure::Malformed);
            return false;
        }
        ByteReader header(message);
        header.U8();
        const std::uint32_t id = header.U32();
        Pending *match = nullptr;
        for (Pending &pending : table_)
        {
            if (pending.used && !pending.ready && pending.id == id)
            {
                match = &pending;
                break;
            }
        }
        if (match == nullptr)
        {
            if (++unsolicited_ > config_.maxUnsolicited)
            {
                Fail(BisectFailure::Malformed);
                return false;
            }
            return true;
        }
        std::optional<BisectReply> reply = DecodeBisectReply(message);
        if (!reply || reply->op != match->op)
        {
            Fail(BisectFailure::Malformed);
            return false;
        }
        if (reply->status == BisectStatus::Ok)
        {
            if (match->op == BisectOp::Objects)
            {
                for (std::size_t i = 0; i < reply->objects.size(); ++i)
                {
                    const ObjectDigest &object = reply->objects[i];
                    if (object.id < match->lo || object.id > match->hi || (i > 0 && object.id <= reply->objects[i - 1].id))
                    {
                        Fail(BisectFailure::Malformed);
                        return false;
                    }
                }
            }
            else if (match->op == BisectOp::Fields)
            {
                for (std::size_t i = 1; i < reply->fields.size(); ++i)
                {
                    if (reply->fields[i].fieldId <= reply->fields[i - 1].fieldId)
                    {
                        Fail(BisectFailure::Malformed);
                        return false;
                    }
                }
            }
        }
        else if (reply->status == BisectStatus::UnknownTick)
        {
            Fail(BisectFailure::UnknownTick);
            return false;
        }
        match->reply = std::move(*reply);
        match->ready = true;
        return true;
    }

    void BisectClient::Poll(const std::uint64_t nowMicros)
    {
        if (failure_ != BisectFailure::None)
        {
            return;
        }
        for (const Pending &pending : table_)
        {
            if (pending.used && !pending.ready && nowMicros >= pending.sentAt &&
                nowMicros - pending.sentAt >= config_.requestTimeoutMicros)
            {
                Fail(BisectFailure::TimedOut);
                return;
            }
        }
    }

    TakeResult BisectClient::Take(const std::uint32_t requestId, BisectReply &out)
    {
        if (failure_ != BisectFailure::None)
        {
            return TakeResult::Failed;
        }
        for (Pending &pending : table_)
        {
            if (pending.used && pending.id == requestId)
            {
                if (!pending.ready)
                {
                    return TakeResult::Pending;
                }
                out = std::move(pending.reply);
                pending = Pending{};
                return TakeResult::Ready;
            }
        }
        return TakeResult::Failed;
    }

    // ---- networked peer ----

    bool NetworkedHashTreePeer::Ask(const BisectOp op, const std::uint32_t lo, const std::uint32_t hi, BisectReply &reply)
    {
        const std::optional<std::uint32_t> id = client_.Submit(driver_.NowMicros(), op, tick_, lo, hi);
        if (!id)
        {
            return false;
        }
        // The loop ends by the timeout in virtual or real time; the step cap makes it end even if the driver's clock
        // is frozen
        constexpr std::uint32_t kMaxWaitSteps = 1'000'000;
        for (std::uint32_t steps = 0; steps < kMaxWaitSteps; ++steps)
        {
            const TakeResult taken = client_.Take(*id, reply);
            if (taken == TakeResult::Ready)
            {
                return true;
            }
            if (taken == TakeResult::Failed)
            {
                return false;
            }
            client_.Poll(driver_.NowMicros());
            if (client_.Failure() != BisectFailure::None)
            {
                return false;
            }
            if (!driver_.Step())
            {
                client_.Abort(BisectFailure::ConnectionFailed);
                return false;
            }
        }
        client_.Abort(BisectFailure::TimedOut);
        return false;
    }

    bool NetworkedHashTreePeer::Range(const std::uint32_t lo, const std::uint32_t hi, RangeSummary &out)
    {
        BisectReply reply;
        if (!Ask(BisectOp::Range, lo, hi, reply) || reply.status != BisectStatus::Ok)
        {
            return false;
        }
        out = reply.range;
        return true;
    }

    bool NetworkedHashTreePeer::Objects(const std::uint32_t lo, const std::uint32_t hi, std::vector<ObjectDigest> &out)
    {
        BisectReply reply;
        if (!Ask(BisectOp::Objects, lo, hi, reply) || reply.status != BisectStatus::Ok)
        {
            return false;
        }
        out = std::move(reply.objects);
        return true;
    }

    bool NetworkedHashTreePeer::Fields(const std::uint32_t objectId, std::vector<HashLeaf> &out)
    {
        BisectReply reply;
        if (!Ask(BisectOp::Fields, objectId, 0, reply) || reply.status != BisectStatus::Ok)
        {
            return false;
        }
        out = std::move(reply.fields);
        return true;
    }
}
