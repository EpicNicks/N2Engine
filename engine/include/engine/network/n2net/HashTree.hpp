#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace N2Engine::N2Net
{
    /**
     * The state-hash tree (netcode C3, design section 4.5): tick -> per object -> per field, 64-bit FNV-1a, leaves over
     * the ENCODED bytes of a field (so what is hashed is what the replication codec would send, and two values that
     * encode alike hash alike, NaN payloads included). Peers hash the confirmed sim state every N ticks and exchange
     * only the root; on a mismatch Bisect narrows the difference to the first divergent object and field by exchanging
     * subtree hashes, in at most about 70 small requests (two per level of a 32-level id bisection, plus the leaf lists) whatever the size of the world.
     *
     * The shape is fixed by the data, not by the history: objects are ordered by id and fields by field id (the builder
     * sorts, so the order the caller walks its world in does not matter), and an object id is a net id (deterministic
     * across peers), not an address. Hashes are not security features.
     *
     * Everything a remote peer can influence is bounded before it is stored: a tree has at most kMaxTreeObjects objects
     * and kMaxFieldsPerObject fields each (the builder refuses more), a bisection reply never carries more than
     * kMaxReplyObjects objects, and a reply that is larger than asked is Malformed rather than copied.
     */
    inline constexpr std::size_t kMaxTreeObjects = 65536;
    inline constexpr std::size_t kMaxFieldsPerObject = 256;
    /// Most (object, field) entries one builder holds
    inline constexpr std::size_t kMaxTreeLeaves = 1u << 20;
    /// A range with at most this many objects (on both sides) is answered with the objects themselves
    inline constexpr std::size_t kMaxReplyObjects = 8;
    /// Checkpoint trees kept for a desync report
    inline constexpr std::size_t kMaxHistory = 64;

    struct HashLeaf
    {
        std::uint16_t fieldId = 0;
        std::uint64_t hash = 0;

        friend bool operator==(const HashLeaf &, const HashLeaf &) = default;
    };

    struct ObjectDigest
    {
        std::uint32_t id = 0;
        std::uint64_t hash = 0;

        friend bool operator==(const ObjectDigest &, const ObjectDigest &) = default;
    };

    struct RangeSummary
    {
        std::uint32_t count = 0;
        std::uint64_t hash = 0;

        friend bool operator==(const RangeSummary &, const RangeSummary &) = default;
    };

    /// What one side of a bisection can be asked. HashTree implements it locally; a networked peer implements it by
    /// sending the request and waiting for the reply (each call is one round trip).
    class IHashTreePeer
    {
    public:
        virtual ~IHashTreePeer() = default;
        /// Count and combined hash of the objects with id in [lo, hi]
        virtual bool Range(std::uint32_t lo, std::uint32_t hi, RangeSummary &out) = 0;
        /// The objects with id in [lo, hi]; false when there are more than kMaxReplyObjects
        virtual bool Objects(std::uint32_t lo, std::uint32_t hi, std::vector<ObjectDigest> &out) = 0;
        /// The fields of one object; false when it does not exist
        virtual bool Fields(std::uint32_t objectId, std::vector<HashLeaf> &out) = 0;
    };

    class HashTree final : public IHashTreePeer
    {
    public:
        struct Object
        {
            std::uint32_t id = 0;
            std::uint64_t hash = 0;
            std::vector<HashLeaf> fields;
        };

        HashTree() = default;

        [[nodiscard]] std::uint32_t Tick() const noexcept { return tick_; }
        [[nodiscard]] const std::vector<Object> &Objects() const noexcept { return objects_; }
        /// The root: tick, object count and the combined hash of every object. This is what peers exchange.
        [[nodiscard]] std::uint64_t Root() const noexcept;

        bool Range(std::uint32_t lo, std::uint32_t hi, RangeSummary &out) override;
        bool Objects(std::uint32_t lo, std::uint32_t hi, std::vector<ObjectDigest> &out) override;
        bool Fields(std::uint32_t objectId, std::vector<HashLeaf> &out) override;

        /// The leaf hash of an encoded field value
        [[nodiscard]] static std::uint64_t LeafHash(std::uint16_t fieldId, std::span<const std::uint8_t> encoded) noexcept;

    private:
        friend class HashTreeBuilder;
        std::uint32_t tick_ = 0;
        std::vector<Object> objects_;
    };

    class HashTreeBuilder
    {
    public:
        explicit HashTreeBuilder(std::uint32_t tick) noexcept : tick_(tick) {}

        /// One field of one object, as its encoded bytes. False (and the builder is failed) beyond the bounds.
        bool AddField(std::uint32_t objectId, std::uint16_t fieldId, std::span<const std::uint8_t> encoded);
        /// An object with no fields still counts (its existence is state)
        bool AddObject(std::uint32_t objectId);

        /// nullopt when a bound was exceeded or the same (object, field) was added twice
        [[nodiscard]] std::optional<HashTree> Build();

    private:
        struct Pending
        {
            std::uint32_t objectId = 0;
            bool hasField = false;
            std::uint16_t fieldId = 0;
            std::uint64_t leaf = 0;
        };

        std::uint32_t tick_;
        bool failed_ = false;
        std::vector<Pending> pending_;
    };

    /// The last K checkpoint trees (a desync report writes them). Pushing past K drops the oldest.
    class HashTreeHistory
    {
    public:
        explicit HashTreeHistory(std::size_t capacity = 8) noexcept
            : capacity_(capacity < 1 ? 1 : (capacity > kMaxHistory ? kMaxHistory : capacity))
        {
        }

        void Push(HashTree tree);
        [[nodiscard]] const HashTree *Find(std::uint32_t tick) const noexcept;
        /// The same, mutable (the IHashTreePeer questions are non-const); a responder answers from it
        [[nodiscard]] HashTree *Find(std::uint32_t tick) noexcept;
        [[nodiscard]] const std::vector<HashTree> &Trees() const noexcept { return trees_; }
        [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

    private:
        std::size_t capacity_;
        std::vector<HashTree> trees_;
    };

    struct Divergence
    {
        enum class Kind : std::uint8_t
        {
            /// The trees agree
            None,
            /// The remote has an object the local tree lacks
            ObjectOnlyRemote,
            /// The local tree has an object the remote lacks
            ObjectOnlyLocal,
            /// Both have the field and it differs
            FieldDiffers,
            FieldOnlyRemote,
            FieldOnlyLocal,
            /// Both have the object, the object hashes differ, but no field does (the objects differ in something
            /// the leaves do not cover: a hash collision, or a peer that lies)
            ObjectDiffersNoField,
            /// The peer's answers were inconsistent or over the bounds; no conclusion
            Malformed,
        };

        Kind kind = Kind::None;
        std::uint32_t objectId = 0;
        std::uint16_t fieldId = 0;
        std::uint64_t localHash = 0;
        std::uint64_t remoteHash = 0;
        /// Requests made to the peer
        int rounds = 0;
    };

    /// The first (lowest object id, then lowest field id) difference between `local` and the peer's tree. The caller
    /// has already seen the roots differ (or not: equal trees give Kind::None after one round).
    [[nodiscard]] Divergence Bisect(HashTree &local, IHashTreePeer &remote);

    /// The root exchange: "my tree at `tick` has this root". Peers send their latest few every so often (a lost one is
    /// covered by the next); the receiver compares it with its own tree of that tick, if it has reached it yet.
    struct RootMessage
    {
        std::uint32_t tick = 0;
        std::uint64_t root = 0;

        friend bool operator==(const RootMessage &, const RootMessage &) = default;
    };

    inline constexpr std::size_t kRootMessageSize = 1 + 4 + 8;

    /// Bytes written (kRootMessageSize), or 0 when `out` is too small
    [[nodiscard]] std::size_t EncodeRootMessage(const RootMessage &message, std::span<std::uint8_t> out) noexcept;
    /// nullopt for a wrong kind byte or any length but kRootMessageSize
    [[nodiscard]] std::optional<RootMessage> DecodeRootMessage(std::span<const std::uint8_t> bytes) noexcept;
}
