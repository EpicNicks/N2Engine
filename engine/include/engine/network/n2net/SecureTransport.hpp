#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/network/n2net/JoinTokens.hpp"
#include "engine/network/n2net/Transport.hpp"
#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/security/Crypto.hpp"
#include "engine/security/PreAuthSlots.hpp"
#include "engine/security/RateLimiter.hpp"
#include "engine/security/TokenBucket.hpp"

// SecureTransport needs the crypto library (n2security over Mbed TLS), which only native builds have. On the web build
// N2NET_HAS_SECURE is 0 and the class does not exist: a browser transport gets its security from the browser.
#if defined(N2SECURITY_HAS_CRYPTO) && N2SECURITY_HAS_CRYPTO
#define N2NET_HAS_SECURE 1
#else
#define N2NET_HAS_SECURE 0
#endif

#if N2NET_HAS_SECURE

namespace N2Engine::N2Net
{
    /// The wire format of the handshake and the sealed packets (docs/netcode.html, "The secure layer"). Little-endian.
    namespace SecureWire
    {
        inline constexpr std::uint8_t kVersion = 1;

        inline constexpr std::uint8_t kConnectRequest = 0x01;
        inline constexpr std::uint8_t kChallenge = 0x02;
        inline constexpr std::uint8_t kConnectResponse = 0x03;
        inline constexpr std::uint8_t kAccept = 0x04;
        inline constexpr std::uint8_t kData = 0x10;
        inline constexpr std::uint8_t kKeepAlive = 0x11;
        inline constexpr std::uint8_t kDisconnect = 0x12;

        inline constexpr std::size_t kKeySize = 32;
        inline constexpr std::size_t kMacSize = 32;
        inline constexpr std::size_t kTagSize = 16;

        /// A ConnectRequest and a ConnectResponse are exactly this long (zero padded). Every reply the server sends
        /// before a session exists (Challenge, Accept) is shorter, so the handshake never amplifies.
        inline constexpr std::size_t kHandshakeRequestSize = 256;

        // ConnectRequest: type, version, u32 protocolId, client X25519 public key, request proof, zeros
        inline constexpr std::size_t kOffsetVersion = 1;
        inline constexpr std::size_t kOffsetProtocolId = 2;
        inline constexpr std::size_t kOffsetClientPublic = 6;
        inline constexpr std::size_t kOffsetRequestProof = kOffsetClientPublic + kKeySize;   // 38
        inline constexpr std::size_t kRequestUsed = kOffsetRequestProof + kMacSize;          // 70
        // ConnectResponse: the request's first 70 bytes, then u64 cookie time, the cookie, the cookie proof, zeros
        inline constexpr std::size_t kOffsetCookieTime = kRequestUsed;                       // 70
        inline constexpr std::size_t kOffsetCookie = kOffsetCookieTime + 8;                  // 78
        inline constexpr std::size_t kOffsetCookieProof = kOffsetCookie + kMacSize;          // 110
        inline constexpr std::size_t kResponseUsed = kOffsetCookieProof + kMacSize;          // 142

        /// Challenge: type, u64 cookie time, cookie
        inline constexpr std::size_t kChallengeSize = 1 + 8 + kMacSize;
        /// Accept: type, server X25519 public key, then sealed (server->client key, sequence 0): role, server proof
        inline constexpr std::size_t kAcceptPlainSize = 1 + kMacSize;
        inline constexpr std::size_t kAcceptSize = 1 + kKeySize + kAcceptPlainSize + kTagSize;

        /// A sealed packet: type, u64 sequence, ciphertext, tag
        inline constexpr std::size_t kSealedHeaderSize = 1 + 8;
        inline constexpr std::size_t kSealedOverhead = kSealedHeaderSize + kTagSize;

        /// The nonce's first byte, per direction (the keys differ too: this is belt and braces)
        inline constexpr std::uint8_t kNonceClientToServer = 0x43;
        inline constexpr std::uint8_t kNonceServerToClient = 0x53;

        /// Sequences this far behind the newest authenticated one are still accepted once
        inline constexpr std::uint64_t kReplayWindow = 256;

        static_assert(kResponseUsed <= kHandshakeRequestSize);
        static_assert(kChallengeSize <= kHandshakeRequestSize && kAcceptSize <= kHandshakeRequestSize,
                      "a pre-auth reply must never be larger than the request it answers");
    } // namespace SecureWire

    /// Receive-side replay protection for one direction: the newest sequence authenticated so far and a 256-bit map of
    /// the ones before it. A sequence is checked (Check) before the packet is opened and recorded (Mark) only after its
    /// tag verified, so a forged packet can never move the window.
    class ReplayWindow
    {
    public:
        enum class Verdict : std::uint8_t
        {
            Fresh,
            /// Seen (authenticated) before
            Replay,
            /// kReplayWindow or more behind the newest: can no longer be told apart from a replay
            TooOld,
        };

        [[nodiscard]] Verdict Check(std::uint64_t sequence) const noexcept;
        /// Records an authenticated sequence (Check must have said Fresh)
        void Mark(std::uint64_t sequence) noexcept;
        [[nodiscard]] bool Any() const noexcept { return any_; }
        [[nodiscard]] std::uint64_t Newest() const noexcept { return newest_; }

    private:
        static constexpr std::size_t kWords = SecureWire::kReplayWindow / 64;
        [[nodiscard]] bool Bit(std::uint64_t back) const noexcept;
        void SetBit(std::uint64_t back) noexcept;
        void ShiftBy(std::uint64_t count) noexcept;

        bool any_ = false;
        std::uint64_t newest_ = 0;
        /// bit b set: newest_ - b was authenticated (bit 0 is newest_ itself)
        std::array<std::uint64_t, kWords> bits_{};
    };

    enum class SecureRole : std::uint8_t
    {
        Server,
        Client,
    };

    struct SecureTransportOptions
    {
        SecureRole role = SecureRole::Server;
        /// Identifies the game and its build: a handshake with any other id is dropped. Not a secret.
        std::uint32_t protocolId = 0;

        // ---- Server ----
        /// Who may join (JoinTokenTable::Load). A server without a table admits nobody. Replace it with SetTokens.
        std::shared_ptr<const JoinTokenTable> tokens;
        /// Handshakes holding a slot at once (from a valid cookie echo until the client's first sealed packet). The
        /// editor host's PreAuthSlots: total clamped to 1-16, per non-loopback address; loopback may use every slot.
        Security::PreAuthSlotsOptions preAuthSlots{.totalSlots = 16, .perAddressSlots = 2};
        /// The rate of Challenges (answers to ConnectRequest), globally and per non-loopback address
        Security::TokenBucketOptions challengeRate{.globalPerSecond = 200, .perAddressPerSecond = 10};
        /// The rate of ConnectResponses the server does work for (slot, token check, X25519), charged only once the cookie
        /// verified, so echoes from spoofed sources cannot spend it
        Security::TokenBucketOptions connectRate{.globalPerSecond = 50, .perAddressPerSecond = 5};
        /// Backoff of addresses whose handshakes failed (a wrong token, or no sealed packet after the Accept)
        Security::RateLimitPolicy failures{};
        /// A cookie is valid this long after the server made it (clamped to 100 ms - 60 s)
        std::uint64_t cookieLifetimeMicros = 5'000'000;
        /// The cookie secret is replaced this often (at least twice the lifetime); the previous one still verifies
        std::uint64_t cookieSecretRotationMicros = 60'000'000;
        /// The most sessions (pending and established); a handshake beyond it is dropped
        std::uint32_t maxSessions = 64;

        // ---- Client ----
        /// The join token. Hashed (SHA-256) when the transport is made; the transport keeps only the hash, never the
        /// text, and never logs either. Clear your own copy when you no longer need it.
        std::string joinToken;
        /// How often an unanswered ConnectRequest or ConnectResponse is sent again
        std::uint64_t resendIntervalMicros = 250'000;

        // ---- Both ----
        /// The client gives up a handshake after this long; the server drops a pending session after this long
        std::uint64_t handshakeTimeoutMicros = 5'000'000;
        /// A sealed keep-alive goes out when nothing was sent for this long
        std::uint64_t keepAliveMicros = 1'000'000;
        /// A session that authenticates nothing for this long is dropped
        std::uint64_t idleTimeoutMicros = 10'000'000;
        /// The time source (any monotonic microsecond clock); empty: std::chrono::steady_clock. Also drives the cookie
        /// times and the admission policy's clocks, so a test can move time without sleeping.
        std::function<std::uint64_t()> clockMicros;
    };

    /// Counters since construction. Nothing here logs; these are the only record of what was dropped and why.
    struct SecureTransportStats
    {
        // Server, before a session (every drop is silent: no reply)
        std::uint64_t requestsReceived = 0;
        std::uint64_t challengesSent = 0;
        std::uint64_t responsesReceived = 0;
        std::uint64_t acceptsSent = 0;
        /// An Accept sent again for a repeated ConnectResponse (its Accept was lost)
        std::uint64_t acceptsResent = 0;
        /// Wrong size, nonzero padding, an unknown type or a type this side never receives
        std::uint64_t droppedMalformed = 0;
        /// Another version or protocol id
        std::uint64_t droppedWrongProtocol = 0;
        /// The transport had no address for the peer
        std::uint64_t droppedNoAddress = 0;
        /// The address is backed off for failed handshakes (RateLimiter)
        std::uint64_t droppedBackedOff = 0;
        /// Over the challenge or connect rate (TokenBucket)
        std::uint64_t droppedRateLimited = 0;
        /// The cookie did not verify: forged, for another address or request, made with a secret no longer kept
        std::uint64_t droppedBadCookie = 0;
        /// The cookie verified but is older than cookieLifetimeMicros (or from the future)
        std::uint64_t droppedExpiredCookie = 0;
        /// No pre-auth slot free for this address (PreAuthSlots)
        std::uint64_t droppedNoSlot = 0;
        /// No token in the table matches the proofs
        std::uint64_t droppedBadToken = 0;
        /// maxSessions reached
        std::uint64_t droppedSessionLimit = 0;
        /// A handshake packet from a peer that already has a session (other than a repeated ConnectResponse)
        std::uint64_t droppedDuplicate = 0;
        /// X25519, HKDF, HMAC or the random generator failed (or the peer's key was refused)
        std::uint64_t cryptoFailures = 0;
        std::uint64_t handshakeTimeouts = 0;
        std::uint64_t sessionsEstablished = 0;
        std::uint64_t idleTimeouts = 0;
        /// Bytes of handshake packets received before a session existed, and of the replies sent to them
        std::uint64_t preAuthBytesReceived = 0;
        std::uint64_t preAuthBytesSent = 0;
        /// The largest reply sent before a session, and the smallest request answered: the amplification bound is
        /// largestPreAuthReply <= smallestPreAuthRequestAnswered (both 0 until something was answered)
        std::uint64_t largestPreAuthReply = 0;
        std::uint64_t smallestPreAuthRequestAnswered = 0;

        // Client
        /// An Accept whose seal, key or server proof did not verify (an impostor, or a server without our token)
        std::uint64_t droppedBadAccept = 0;

        // Sealed packets, both sides
        std::uint64_t sealedSent = 0;
        std::uint64_t sealedReceived = 0;
        /// The tag did not verify: tampered, forged or for another session. Dropped silently.
        std::uint64_t droppedAuthFailed = 0;
        /// A sequence authenticated before
        std::uint64_t droppedReplay = 0;
        /// A sequence too far behind the newest to be checked
        std::uint64_t droppedTooOld = 0;
        /// A sealed packet from a peer with no keys
        std::uint64_t droppedNoSession = 0;
    };

    enum class SecureClientState : std::uint8_t
    {
        /// Connect has not been called (or the session ended through Disconnect)
        Idle,
        /// Sending ConnectRequest, waiting for a Challenge
        Requesting,
        /// Sending ConnectResponse, waiting for an Accept
        Responding,
        /// The Accept verified: Send works, PeerConnected was delivered
        Connected,
        /// The handshake timed out or the session ended; see ClientFailure
        Failed,
    };

    enum class SecureClientFailure : std::uint8_t
    {
        None,
        /// No verified Accept within handshakeTimeoutMicros (a wrong token looks exactly like this: the server says nothing)
        HandshakeTimeout,
        /// The session authenticated nothing for idleTimeoutMicros
        IdleTimeout,
        /// The server sent a sealed Disconnect
        ClosedByServer,
        /// The transport below dropped the server peer
        TransportClosed,
        /// The random generator or X25519 failed on this side
        CryptoFailure,
        /// The 2^64th packet: the session can send no more (it would reuse a nonce)
        SequenceExhausted,
    };

    /// Who a server's peer is: the tokens file entry its handshake proved
    struct SecurePeerIdentity
    {
        std::string user;
        JoinRole role = JoinRole::Player;
    };

    class SecureTransport;

    struct SecureUdpOpenResult
    {
        /// Null when the open failed
        std::unique_ptr<SecureTransport> transport;
        /// The UDP transport inside `transport` (owned by it), for Connect, LocalPort and its stats. Null on failure.
        UdpTransport *udp = nullptr;
        UdpError error = UdpError::None;
        std::string message;
    };

    /**
     * The secure layer: an ITransport over another ITransport that adds the connection handshake and seals every
     * datagram (docs/netcode.html, "The secure layer"). The layers above (Connection, the channels) see only
     * authenticated peers: PeerConnected after a handshake completes, Datagram only for packets whose tag verified,
     * PeerDisconnected when a session ends. MaxPacketSize() is the inner transport's less SecureWire::kSealedOverhead;
     * give it to ConnectionConfig::maxPacketSize.
     *
     * The handshake (client C, server S):
     *  1. C -> S ConnectRequest (256 bytes): version, protocol id, C's ephemeral X25519 public key, and a proof of the
     *     join token: HMAC-SHA-256 keyed with the token's SHA-256 over the version, the protocol id and that key.
     *  2. S -> C Challenge (41 bytes): a cookie, HMAC-SHA-256 with a per-process random secret over C's address and
     *     port, S's time in milliseconds and the request's fields, and that time. S keeps NOTHING: no session, no slot,
     *     no table entry (the inner UDP transport's own peer table is A4's, bounded and idle-timed).
     *  3. C -> S ConnectResponse (256 bytes): the request's fields, the cookie and its time, and a second proof
     *     (HMAC keyed with the token's hash over the cookie time and the cookie). S checks the cookie (same address,
     *     not expired, a secret it still has) BEFORE anything else costs it state; only then it takes a pre-auth slot,
     *     matches both proofs against every token in constant time, makes its own ephemeral key and derives the
     *     session keys (X25519, then HKDF-SHA-256 over the transcript, one key per direction).
     *  4. S -> C Accept (82 bytes): S's public key, then sealed with the server-to-client key: the role and a server
     *     proof (HMAC keyed with the token's hash over the transcript), so C knows S holds its token's hash.
     *  5. C's first sealed packet (a keep-alive at once) proves it holds the keys: S then frees the slot and the session
     *     is established.
     * Sealed packets are ChaCha20-Poly1305 with a 64-bit per-direction sequence as the nonce (never reused: each key is
     * fresh per session and the counter only rises), the header as associated data, a 256-packet replay window, and
     * anything that fails dropped silently and counted.
     *
     * Single-threaded like every ITransport: call everything from one thread. Never throws, never logs.
     */
    class SecureTransport final : public ITransport
    {
    public:
        /// Over `inner`, which must outlive this object (and is driven only through it from now on)
        SecureTransport(ITransport &inner, SecureTransportOptions options);
        /// Over `inner`, owned
        SecureTransport(std::unique_ptr<ITransport> inner, SecureTransportOptions options);
        ~SecureTransport() override;
        SecureTransport(const SecureTransport &) = delete;
        SecureTransport &operator=(const SecureTransport &) = delete;

        /**
         * A UdpTransport with this layer over it. This is the only way to open a UdpTransport with encryption counted
         * as available: a non-loopback bind (and Connect) is then allowed without udpOptions.insecureBind (the A4
         * fail-closed rule: "non-loopback only when encrypted, or by explicit override").
         */
        [[nodiscard]] static SecureUdpOpenResult OpenUdp(const UdpTransportOptions &udpOptions,
                                                         SecureTransportOptions options);

        [[nodiscard]] std::size_t MaxPacketSize() const noexcept override;
        [[nodiscard]] bool IsReliableStream() const noexcept override;
        /// Seals and sends to an established session; UnknownPeer for any other peer
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> datagram) override;
        void Poll(EventHandler handler) override;
        /// Sends a sealed Disconnect, forgets the session and disconnects the inner peer. A no-op inside Poll's handler.
        void Disconnect(PeerId peer) override;
        [[nodiscard]] bool GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const override;

        /// Client: starts the handshake with the inner transport's `serverPeer` (from UdpTransport::Connect, or a
        /// SimNetwork link). False if this is a server, the role's token could not be hashed, or a handshake or session
        /// is already under way.
        bool Connect(PeerId serverPeer);
        [[nodiscard]] SecureClientState ClientState() const noexcept { return clientState_; }
        [[nodiscard]] SecureClientFailure ClientFailure() const noexcept { return clientFailure_; }
        /// Client: the role the server granted (nullopt until Connected)
        [[nodiscard]] std::optional<JoinRole> GrantedRole() const noexcept { return grantedRole_; }

        /// Server: replaces the token table (a reloaded file); handshakes from now on use it, sessions are kept
        void SetTokens(std::shared_ptr<const JoinTokenTable> tokens);
        /// Server: who an established peer is; false for any other peer
        [[nodiscard]] bool GetPeerIdentity(PeerId peer, SecurePeerIdentity &identity) const;

        [[nodiscard]] const SecureTransportStats &Stats() const noexcept { return stats_; }
        [[nodiscard]] const SecureTransportOptions &Options() const noexcept { return options_; }
        /// Sessions held: pending (between Accept and the first sealed packet) and established
        [[nodiscard]] std::size_t PendingSessionCount() const noexcept;
        [[nodiscard]] std::size_t EstablishedSessionCount() const noexcept;
        /// Pre-auth slots held now (pending sessions only)
        [[nodiscard]] std::size_t PreAuthSlotsInUse() const;
        /// False when the layer could not start (no random generator for the cookie secret or the client's keys): it
        /// then drops everything
        [[nodiscard]] bool Healthy() const noexcept { return healthy_; }

    private:
        struct Session;

        void Initialise();
        [[nodiscard]] std::uint64_t NowMicros() const;

        void RunTimers(std::uint64_t now, EventHandler handler);
        void OnInnerEvent(const TransportEvent &event, std::uint64_t now, EventHandler handler);
        void OnConnectRequest(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now);
        void OnConnectResponse(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now);
        void OnChallenge(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now);
        void OnAccept(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now, EventHandler handler);
        void OnSealed(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now, EventHandler handler);

        /// Sends a reply to a peer with no session: only when it is no larger than the request it answers
        bool SendPreAuthReply(PeerId peer, std::span<const std::uint8_t> reply, std::size_t requestSize);
        TransportSendResult SendSealed(PeerId peer, Session &session, std::uint8_t type,
                                       std::span<const std::uint8_t> payload, std::uint64_t now);
        void SendClientHandshake(std::uint64_t now);
        void EndSession(PeerId peer, bool notifyUpper, EventHandler *handler);
        void FailClient(SecureClientFailure failure);

        [[nodiscard]] bool RotateCookieSecret(std::uint64_t nowMillis);
        [[nodiscard]] bool ComputeCookie(const PeerEndpoint &endpoint, std::uint64_t cookieTime,
                                         std::span<const std::uint8_t> requestFields,
                                         const std::array<std::uint8_t, 32> &secret, std::array<std::uint8_t, 32> &cookie) const;

        std::unique_ptr<ITransport> owned_;
        ITransport *inner_ = nullptr;
        SecureTransportOptions options_;
        SecureTransportStats stats_;
        bool healthy_ = true;
        bool inPoll_ = false;
        std::vector<PeerId> deferredInnerDisconnects_;

        // Server
        std::shared_ptr<const JoinTokenTable> tokens_;
        std::unique_ptr<Security::PreAuthSlots> slots_;
        std::unique_ptr<Security::TokenBucket> challengeBucket_;
        std::unique_ptr<Security::TokenBucket> connectBucket_;
        std::unique_ptr<Security::RateLimiter> limiter_;
        std::array<std::uint8_t, 32> cookieSecret_{};
        std::array<std::uint8_t, 32> previousCookieSecret_{};
        std::uint64_t cookieEpoch_ = 0;
        bool haveCookieSecret_ = false;
        bool havePreviousCookieSecret_ = false;

        std::map<PeerId, std::unique_ptr<Session>> sessions_;

        // Client
        Security::TokenHash tokenKey_{};
        bool haveTokenKey_ = false;
        PeerId serverPeer_ = kInvalidPeer;
        SecureClientState clientState_ = SecureClientState::Idle;
        SecureClientFailure clientFailure_ = SecureClientFailure::None;
        std::optional<JoinRole> grantedRole_;
        Security::X25519Key clientPrivate_{};
        Security::X25519Key clientPublic_{};
        std::array<std::uint8_t, 32> requestProof_{};
        bool haveCookie_ = false;
        std::uint64_t cookieTime_ = 0;
        std::array<std::uint8_t, 32> cookie_{};
        std::array<std::uint8_t, 32> cookieProof_{};
        std::uint64_t handshakeStartedAt_ = 0;
        std::uint64_t lastHandshakeSendAt_ = 0;
        std::uint64_t respondingSince_ = 0;
    };
} // namespace N2Engine::N2Net

#endif // N2NET_HAS_SECURE
