#include "engine/network/n2net/HashTree.hpp"

#include <algorithm>

#include "engine/network/NetworkHash.hpp"
#include "engine/network/n2net/ByteIO.hpp"

namespace N2Engine::N2Net
{
    namespace
    {
        using Hasher = Network::Fnv1a64;

        std::uint64_t ObjectHash(const std::uint32_t id, const std::vector<HashLeaf> &fields)
        {
            Hasher hasher;
            hasher.U32(id);
            hasher.U32(static_cast<std::uint32_t>(fields.size()));
            for (const HashLeaf &leaf : fields)
            {
                hasher.U16(leaf.fieldId);
                hasher.U64(leaf.hash);
            }
            return hasher.Value();
        }

        // Objects are sorted by id: the first one with id >= lo, and the end of those <= hi
        template <typename Objects>
        auto RangeBegin(const Objects &objects, const std::uint32_t lo)
        {
            return std::lower_bound(objects.begin(), objects.end(), lo,
                                    [](const auto &object, const std::uint32_t value) { return object.id < value; });
        }

        template <typename Objects>
        auto RangeEnd(const Objects &objects, const std::uint32_t hi)
        {
            return std::upper_bound(objects.begin(), objects.end(), hi,
                                    [](const std::uint32_t value, const auto &object) { return value < object.id; });
        }
    }

    std::uint64_t HashTree::LeafHash(const std::uint16_t fieldId, const std::span<const std::uint8_t> encoded) noexcept
    {
        Hasher hasher;
        hasher.U16(fieldId);
        hasher.U32(static_cast<std::uint32_t>(encoded.size()));
        hasher.Bytes(encoded);
        return hasher.Value();
    }

    bool HashTree::Range(const std::uint32_t lo, const std::uint32_t hi, RangeSummary &out)
    {
        out = {};
        if (lo > hi)
        {
            return true;
        }
        Hasher hasher;
        for (auto it = RangeBegin(objects_, lo), end = RangeEnd(objects_, hi); it < end; ++it)
        {
            hasher.U32(it->id);
            hasher.U64(it->hash);
            ++out.count;
        }
        out.hash = hasher.Value();
        return true;
    }

    bool HashTree::Objects(const std::uint32_t lo, const std::uint32_t hi, std::vector<ObjectDigest> &out)
    {
        out.clear();
        if (lo > hi)
        {
            return true;
        }
        const auto begin = RangeBegin(objects_, lo);
        const auto end = RangeEnd(objects_, hi);
        if (static_cast<std::size_t>(end - begin) > kMaxReplyObjects)
        {
            return false;
        }
        for (auto it = begin; it < end; ++it)
        {
            out.push_back(ObjectDigest{it->id, it->hash});
        }
        return true;
    }

    bool HashTree::Fields(const std::uint32_t objectId, std::vector<HashLeaf> &out)
    {
        out.clear();
        const auto it = RangeBegin(objects_, objectId);
        if (it == objects_.end() || it->id != objectId)
        {
            return false;
        }
        out = it->fields;
        return true;
    }

    std::uint64_t HashTree::Root() const noexcept
    {
        Hasher hasher;
        hasher.U32(tick_);
        hasher.U32(static_cast<std::uint32_t>(objects_.size()));
        for (const Object &object : objects_)
        {
            hasher.U32(object.id);
            hasher.U64(object.hash);
        }
        return hasher.Value();
    }

    bool HashTreeBuilder::AddField(const std::uint32_t objectId, const std::uint16_t fieldId,
                                   const std::span<const std::uint8_t> encoded)
    {
        if (failed_ || pending_.size() >= kMaxTreeLeaves)
        {
            failed_ = true;
            return false;
        }
        pending_.push_back(Pending{objectId, true, fieldId, HashTree::LeafHash(fieldId, encoded)});
        return true;
    }

    bool HashTreeBuilder::AddObject(const std::uint32_t objectId)
    {
        if (failed_ || pending_.size() >= kMaxTreeLeaves)
        {
            failed_ = true;
            return false;
        }
        pending_.push_back(Pending{objectId, false, 0, 0});
        return true;
    }

    std::optional<HashTree> HashTreeBuilder::Build()
    {
        if (failed_)
        {
            return std::nullopt;
        }
        // Field-less markers sort first within an object (hasField false), then fields by id
        std::sort(pending_.begin(), pending_.end(),
                  [](const Pending &a, const Pending &b)
                  {
                      if (a.objectId != b.objectId)
                      {
                          return a.objectId < b.objectId;
                      }
                      if (a.hasField != b.hasField)
                      {
                          return !a.hasField;
                      }
                      return a.fieldId < b.fieldId;
                  });
        HashTree tree;
        tree.tick_ = tick_;
        for (std::size_t i = 0; i < pending_.size(); ++i)
        {
            const Pending &p = pending_[i];
            if (tree.objects_.empty() || tree.objects_.back().id != p.objectId)
            {
                if (tree.objects_.size() >= kMaxTreeObjects)
                {
                    return std::nullopt;
                }
                tree.objects_.push_back(HashTree::Object{p.objectId, 0, {}});
            }
            if (!p.hasField)
            {
                continue;
            }
            std::vector<HashLeaf> &fields = tree.objects_.back().fields;
            if (!fields.empty() && fields.back().fieldId == p.fieldId)
            {
                return std::nullopt; // the same (object, field) twice
            }
            if (fields.size() >= kMaxFieldsPerObject)
            {
                return std::nullopt;
            }
            fields.push_back(HashLeaf{p.fieldId, p.leaf});
        }
        for (HashTree::Object &object : tree.objects_)
        {
            object.hash = ObjectHash(object.id, object.fields);
        }
        return tree;
    }

    void HashTreeHistory::Push(HashTree tree)
    {
        if (trees_.size() >= capacity_)
        {
            trees_.erase(trees_.begin());
        }
        trees_.push_back(std::move(tree));
    }

    const HashTree *HashTreeHistory::Find(const std::uint32_t tick) const noexcept
    {
        for (const HashTree &tree : trees_)
        {
            if (tree.Tick() == tick)
            {
                return &tree;
            }
        }
        return nullptr;
    }

    namespace
    {
        Divergence Malformed(Divergence d)
        {
            d.kind = Divergence::Kind::Malformed;
            return d;
        }

        bool SortedWithin(const std::vector<ObjectDigest> &objects, const std::uint32_t lo, const std::uint32_t hi)
        {
            for (std::size_t i = 0; i < objects.size(); ++i)
            {
                if (objects[i].id < lo || objects[i].id > hi || (i > 0 && objects[i].id <= objects[i - 1].id))
                {
                    return false;
                }
            }
            return true;
        }

        bool SortedFields(const std::vector<HashLeaf> &fields)
        {
            if (fields.size() > kMaxFieldsPerObject)
            {
                return false;
            }
            for (std::size_t i = 1; i < fields.size(); ++i)
            {
                if (fields[i].fieldId <= fields[i - 1].fieldId)
                {
                    return false;
                }
            }
            return true;
        }

        Divergence ResolveFields(IHashTreePeer &local, IHashTreePeer &remote, const std::uint32_t id, Divergence d)
        {
            std::vector<HashLeaf> l;
            std::vector<HashLeaf> r;
            ++d.rounds;
            if (!local.Fields(id, l) || !remote.Fields(id, r) || !SortedFields(l) || !SortedFields(r))
            {
                return Malformed(d);
            }
            d.objectId = id;
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < l.size() || j < r.size())
            {
                if (j >= r.size() || (i < l.size() && l[i].fieldId < r[j].fieldId))
                {
                    d.kind = Divergence::Kind::FieldOnlyLocal;
                    d.fieldId = l[i].fieldId;
                    d.localHash = l[i].hash;
                    return d;
                }
                if (i >= l.size() || r[j].fieldId < l[i].fieldId)
                {
                    d.kind = Divergence::Kind::FieldOnlyRemote;
                    d.fieldId = r[j].fieldId;
                    d.remoteHash = r[j].hash;
                    return d;
                }
                if (l[i].hash != r[j].hash)
                {
                    d.kind = Divergence::Kind::FieldDiffers;
                    d.fieldId = l[i].fieldId;
                    d.localHash = l[i].hash;
                    d.remoteHash = r[j].hash;
                    return d;
                }
                ++i;
                ++j;
            }
            d.kind = Divergence::Kind::ObjectDiffersNoField;
            return d;
        }

        // Both sides hold at most kMaxReplyObjects objects in [lo, hi]: compare them one by one
        Divergence ResolveObjects(IHashTreePeer &local, IHashTreePeer &remote, const std::uint32_t lo, const std::uint32_t hi,
                                  const RangeSummary &l, const RangeSummary &r, Divergence d)
        {
            std::vector<ObjectDigest> lo_objects;
            std::vector<ObjectDigest> ro_objects;
            ++d.rounds;
            if (!local.Objects(lo, hi, lo_objects) || !remote.Objects(lo, hi, ro_objects) ||
                lo_objects.size() != l.count || ro_objects.size() != r.count || !SortedWithin(lo_objects, lo, hi) ||
                !SortedWithin(ro_objects, lo, hi))
            {
                return Malformed(d);
            }
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < lo_objects.size() || j < ro_objects.size())
            {
                if (j >= ro_objects.size() || (i < lo_objects.size() && lo_objects[i].id < ro_objects[j].id))
                {
                    d.kind = Divergence::Kind::ObjectOnlyLocal;
                    d.objectId = lo_objects[i].id;
                    d.localHash = lo_objects[i].hash;
                    return d;
                }
                if (i >= lo_objects.size() || ro_objects[j].id < lo_objects[i].id)
                {
                    d.kind = Divergence::Kind::ObjectOnlyRemote;
                    d.objectId = ro_objects[j].id;
                    d.remoteHash = ro_objects[j].hash;
                    return d;
                }
                if (lo_objects[i].hash != ro_objects[j].hash)
                {
                    return ResolveFields(local, remote, lo_objects[i].id, d);
                }
                ++i;
                ++j;
            }
            return Malformed(d); // the range hashes differed but the objects agree
        }
    }

    std::size_t EncodeRootMessage(const RootMessage &message, const std::span<std::uint8_t> out) noexcept
    {
        if (out.size() < kRootMessageSize)
        {
            return 0;
        }
        ByteWriter writer(out.data(), out.size());
        writer.U8(2); // message kind: root
        writer.U32(message.tick);
        writer.U32(static_cast<std::uint32_t>(message.root & 0xFFFFFFFFu));
        writer.U32(static_cast<std::uint32_t>(message.root >> 32));
        return writer.Size();
    }

    std::optional<RootMessage> DecodeRootMessage(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (bytes.size() != kRootMessageSize)
        {
            return std::nullopt;
        }
        ByteReader reader(bytes);
        if (reader.U8() != 2)
        {
            return std::nullopt;
        }
        RootMessage message;
        message.tick = reader.U32();
        const std::uint64_t low = reader.U32();
        const std::uint64_t high = reader.U32();
        message.root = low | (high << 32);
        return message;
    }

    Divergence Bisect(HashTree &local, IHashTreePeer &remote)
    {
        Divergence d;
        std::uint32_t lo = 0;
        std::uint32_t hi = 0xFFFFFFFFu;
        RangeSummary l;
        RangeSummary r;
        local.Range(lo, hi, l);
        ++d.rounds;
        if (!remote.Range(lo, hi, r))
        {
            return Malformed(d);
        }
        if (l == r)
        {
            return d;
        }
        for (;;)
        {
            if (l.count <= kMaxReplyObjects && r.count <= kMaxReplyObjects)
            {
                return ResolveObjects(local, remote, lo, hi, l, r, d);
            }
            if (lo == hi)
            {
                return Malformed(d); // one id cannot hold more than one object
            }
            const std::uint32_t mid = lo + (hi - lo) / 2;
            RangeSummary ll;
            RangeSummary lr;
            local.Range(lo, mid, ll);
            ++d.rounds;
            if (!remote.Range(lo, mid, lr))
            {
                return Malformed(d);
            }
            if (!(ll == lr))
            {
                hi = mid;
                l = ll;
                r = lr;
                continue;
            }
            // The left half agrees, so the difference is in the right half (the parent differed)
            RangeSummary rl;
            RangeSummary rr;
            local.Range(mid + 1, hi, rl);
            ++d.rounds;
            if (!remote.Range(mid + 1, hi, rr) || rl == rr)
            {
                return Malformed(d);
            }
            lo = mid + 1;
            l = rl;
            r = rr;
        }
    }
}
