#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/HashTree.hpp"

namespace N2Engine::N2Net
{
    /**
     * The wire protocol of hash-tree bisection (netcode C3, design section 4.5) and the session state around it.
     * It lives in the n2net message space (first byte: kind 1 = input bundle, 2 = root, 3 = bisect request, 4 =
     * bisect reply), not in the world protocol, and carries no wire version of its own: it rides a Connection whose
     * protocolId already names the build.
     *
     * A request names a tick and asks one question of that tick's hash tree (IHashTreePeer's three questions):
     *   Range   [lo, hi]  -> object count and combined hash
     *   Objects [lo, hi]  -> the objects themselves (at most kMaxReplyObjects)
     *   Fields  object    -> the fields of one object (at most kMaxFieldsPerObject)
     * Every reply has a hard maximum size (kMaxBisectReplySize), so nothing here asks for or allocates by a number a
     * peer chose: counts are checked against the caps and against the bytes that are actually there before anything
     * is reserved, and trailing bytes are rejected.
     *
     * All messages go on the reliable-ordered channel, so loss only delays; a peer that never answers ends the
     * exchange with BisectFailure::TimedOut, one that answers wrongly with Malformed.
     */
    inline constexpr std::uint8_t kBisectRequestKind = 3;
    inline constexpr std::uint8_t kBisectReplyKind = 4;

    enum class BisectOp : std::uint8_t
    {
        Range = 0,
        Objects = 1,
        Fields = 2,
    };

    enum class BisectStatus : std::uint8_t
    {
        Ok = 0,
        /// The responder holds no tree for that tick
        UnknownTick = 1,
        /// Objects: more than kMaxReplyObjects in the range
        TooMany = 2,
        /// Fields: no such object
        NoSuchObject = 3,
    };

    inline constexpr std::size_t kBisectRequestSize = 1 + 4 + 1 + 4 + 4 + 4;
    inline constexpr std::size_t kBisectReplyHeaderSize = 1 + 4 + 1 + 1;
    inline constexpr std::size_t kBisectObjectEntrySize = 4 + 8;
    inline constexpr std::size_t kBisectFieldEntrySize = 2 + 8;
    /// The largest reply there can be: a Fields reply with kMaxFieldsPerObject entries
    inline constexpr std::size_t kMaxBisectReplySize = kBisectReplyHeaderSize + 2 + kMaxFieldsPerObject * kBisectFieldEntrySize;

    struct BisectRequest
    {
        std::uint32_t requestId = 0;
        BisectOp op = BisectOp::Range;
        std::uint32_t tick = 0;
        /// Range and Objects: the id range, lo <= hi. Fields: the object id (hi is 0).
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;

        friend bool operator==(const BisectRequest &, const BisectRequest &) = default;
    };

    struct BisectReply
    {
        std::uint32_t requestId = 0;
        BisectOp op = BisectOp::Range;
        BisectStatus status = BisectStatus::Ok;
        RangeSummary range;
        std::vector<ObjectDigest> objects;
        std::vector<HashLeaf> fields;

        friend bool operator==(const BisectReply &, const BisectReply &) = default;
    };

    /// Bytes written, or 0 when `out` is too small or the request is not encodable (lo > hi, Fields with hi != 0)
    [[nodiscard]] std::size_t EncodeBisectRequest(const BisectRequest &request, std::span<std::uint8_t> out) noexcept;
    /// nullopt for a wrong kind, an unknown op, lo > hi, Fields with hi != 0, or any length but kBisectRequestSize
    [[nodiscard]] std::optional<BisectRequest> DecodeBisectRequest(std::span<const std::uint8_t> bytes) noexcept;

    /// Bytes written, or 0 when it does not fit, a list is over its cap, or op, status and payload do not agree
    /// (TooMany is only for Objects, NoSuchObject only for Fields; a non-Ok reply carries no payload)
    [[nodiscard]] std::size_t EncodeBisectReply(const BisectReply &reply, std::span<std::uint8_t> out) noexcept;
    /// nullopt on a wrong kind, unknown op or status, an op/status that cannot go together, a count over its cap, or
    /// a length that is not exactly what the header and the count say. Allocates only after those checks.
    [[nodiscard]] std::optional<BisectReply> DecodeBisectReply(std::span<const std::uint8_t> bytes);

    /// The first byte, to route a received n2net message (0 for an empty one)
    [[nodiscard]] inline std::uint8_t PeekMessageKind(std::span<const std::uint8_t> bytes) noexcept
    {
        return bytes.empty() ? 0 : bytes[0];
    }

    struct BisectResponderConfig
    {
        /// Requests answered back to back; the bucket then refills at one per refillMicros
        std::uint32_t burst = 16;
        std::uint64_t refillMicros = 2'000;
        /// Requests refused (rate or malformed) before the responder reports Abusive and stops answering
        std::uint32_t maxViolations = 64;
    };

    enum class ServeResult : std::uint8_t
    {
        /// A reply was written
        Replied,
        /// Over the rate limit: dropped without a reply
        RateLimited,
        /// Not a decodable request: dropped
        Malformed,
        /// Too many violations: the session should be ended; nothing is answered any more
        Abusive,
    };

    /// Answers requests from a peer out of the local checkpoint trees. Allocation-free (the reply is built in a fixed
    /// buffer from our own data), rate limited by a token bucket, and gives up on a peer that keeps breaking the
    /// rules. Single-threaded.
    class BisectResponder
    {
    public:
        BisectResponder(HashTreeHistory &history, const BisectResponderConfig &config, std::uint64_t nowMicros) noexcept;

        /// Handles one request message. On Replied, `reply` holds the encoded reply (valid until the next call).
        ServeResult Serve(std::uint64_t nowMicros, std::span<const std::uint8_t> request, std::span<const std::uint8_t> &reply);
        /// Serve, then queue the reply on `connection` (reliable)
        ServeResult ServeOn(std::uint64_t nowMicros, std::span<const std::uint8_t> request, Connection &connection);

        [[nodiscard]] bool Abusive() const noexcept { return violations_ >= config_.maxViolations; }
        [[nodiscard]] std::uint64_t Answered() const noexcept { return answered_; }
        [[nodiscard]] std::uint64_t RateLimited() const noexcept { return rateLimited_; }
        [[nodiscard]] std::uint64_t MalformedCount() const noexcept { return malformed_; }

    private:
        HashTreeHistory &history_;
        BisectResponderConfig config_;
        std::uint64_t lastRefill_;
        std::uint64_t refillRemainder_ = 0;
        std::uint32_t tokens_;
        std::uint32_t violations_ = 0;
        std::uint64_t answered_ = 0;
        std::uint64_t rateLimited_ = 0;
        std::uint64_t malformed_ = 0;
        std::array<std::uint8_t, kMaxBisectReplySize> buffer_{};
    };

    enum class BisectFailure : std::uint8_t
    {
        None,
        /// A request got no reply within requestTimeoutMicros
        TimedOut,
        /// A reply that does not decode, does not match its request, or breaks the caps or ordering; or too many
        /// replies nobody asked for
        Malformed,
        /// More than maxRequests requests in this session
        BudgetExceeded,
        /// The connection failed or refused the request
        ConnectionFailed,
        /// The peer has no tree for that tick
        UnknownTick,
    };

    struct BisectClientConfig
    {
        std::uint64_t requestTimeoutMicros = 2'000'000;
        /// Requests over the whole session (a bisection needs about 70)
        std::uint32_t maxRequests = 128;
        /// Requests awaiting a reply at once, at most kMaxBisectOutstanding
        std::uint32_t maxOutstanding = 4;
        /// Replies that match no outstanding request tolerated before the session is Malformed
        std::uint32_t maxUnsolicited = 8;
    };

    inline constexpr std::uint32_t kMaxBisectOutstanding = 8;

    enum class TakeResult : std::uint8_t
    {
        Pending,
        Ready,
        /// Failed (see Failure()); also an id that is not outstanding
        Failed,
    };

    /// The asking side: numbers requests, keeps a bounded table of outstanding ones, matches and validates replies,
    /// times requests out. Once failed it stays failed (the session is over). It sends on the Connection it was given
    /// and is fed the replies by the host (OnMessage).
    class BisectClient
    {
    public:
        BisectClient(Connection &connection, const BisectClientConfig &config) noexcept;

        /// Sends a request. nullopt (and failure set, unless it is only that the table is full: then None) when the
        /// budget or the outstanding limit is hit or the connection refuses.
        [[nodiscard]] std::optional<std::uint32_t> Submit(std::uint64_t nowMicros, BisectOp op, std::uint32_t tick,
                                                          std::uint32_t lo, std::uint32_t hi);

        /// A message of kind kBisectReplyKind from the peer. False when it ended the session (Malformed).
        bool OnMessage(std::uint64_t nowMicros, std::span<const std::uint8_t> message);

        /// Expires requests older than the timeout; sets TimedOut
        void Poll(std::uint64_t nowMicros);

        /// Ends the exchange with `failure` (the first failure sticks)
        void Abort(BisectFailure failure) noexcept { Fail(failure); }

        /// Moves the reply for `requestId` into `out` when it has arrived
        TakeResult Take(std::uint32_t requestId, BisectReply &out);

        [[nodiscard]] BisectFailure Failure() const noexcept { return failure_; }
        [[nodiscard]] std::uint32_t Outstanding() const noexcept;
        [[nodiscard]] std::uint32_t RequestsSent() const noexcept { return sent_; }
        [[nodiscard]] std::uint32_t Unsolicited() const noexcept { return unsolicited_; }

    private:
        struct Pending
        {
            bool used = false;
            bool ready = false;
            std::uint32_t id = 0;
            BisectOp op = BisectOp::Range;
            std::uint32_t lo = 0;
            std::uint32_t hi = 0;
            std::uint64_t sentAt = 0;
            BisectReply reply;
        };

        void Fail(BisectFailure failure) noexcept
        {
            if (failure_ == BisectFailure::None)
            {
                failure_ = failure;
            }
        }

        Connection &connection_;
        BisectClientConfig config_;
        BisectFailure failure_ = BisectFailure::None;
        std::uint32_t nextId_ = 1;
        std::uint32_t sent_ = 0;
        std::uint32_t unsolicited_ = 0;
        std::array<Pending, kMaxBisectOutstanding> table_{};
    };

    /// What a blocking peer needs from its host: a way to let the network run. Step() moves the world on (delivers
    /// datagrams, feeds BisectClient::OnMessage, lets the Connections send) and false means the session is gone.
    class IBisectDriver
    {
    public:
        virtual ~IBisectDriver() = default;
        [[nodiscard]] virtual std::uint64_t NowMicros() const = 0;
        virtual bool Step() = 0;
    };

    /// IHashTreePeer over a BisectClient: each question is one request, answered by stepping the driver until the
    /// reply arrives, the timeout passes or the session fails. Bisect(local, peer) then gives Malformed for any failed
    /// question; Failure() says why. One question at a time, so a bisection uses one outstanding request.
    ///
    /// Blocking is a property of the IHashTreePeer seam (Bisect is a synchronous loop). A host that cannot block (the
    /// web build has no threads) runs the same client from its own per-frame state machine instead.
    class NetworkedHashTreePeer final : public IHashTreePeer
    {
    public:
        NetworkedHashTreePeer(BisectClient &client, IBisectDriver &driver, std::uint32_t tick) noexcept
            : client_(client), driver_(driver), tick_(tick)
        {
        }

        bool Range(std::uint32_t lo, std::uint32_t hi, RangeSummary &out) override;
        bool Objects(std::uint32_t lo, std::uint32_t hi, std::vector<ObjectDigest> &out) override;
        bool Fields(std::uint32_t objectId, std::vector<HashLeaf> &out) override;

        [[nodiscard]] BisectFailure Failure() const noexcept { return client_.Failure(); }

    private:
        bool Ask(BisectOp op, std::uint32_t lo, std::uint32_t hi, BisectReply &reply);

        BisectClient &client_;
        IBisectDriver &driver_;
        std::uint32_t tick_;
    };
}
