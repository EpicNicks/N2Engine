#include "engine/network/n2net/SecureTransport.hpp"

#if N2NET_HAS_SECURE

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace N2Engine::N2Net
{
    namespace
    {
        using namespace SecureWire;
        using Bytes32 = std::array<std::uint8_t, 32>;

        // Domain-separation labels: every HMAC, hash and key derivation has its own, so no value made for one purpose
        // can be presented as another
        constexpr std::string_view kLabelRequestProof = "n2net v1 request proof";
        constexpr std::string_view kLabelCookie = "n2net v1 cookie";
        constexpr std::string_view kLabelCookieProof = "n2net v1 cookie proof";
        constexpr std::string_view kLabelTranscript = "n2net v1 transcript";
        constexpr std::string_view kLabelClientToServer = "n2net v1 client to server key";
        constexpr std::string_view kLabelServerToClient = "n2net v1 server to client key";
        constexpr std::string_view kLabelServerProof = "n2net v1 server proof";

        /// Each sealed packet's associated data: type, version, protocol id, sequence
        constexpr std::size_t kSealedAadSize = 1 + 1 + 4 + 8;
        /// The Accept's associated data: type, version, protocol id, the server's public key
        constexpr std::size_t kAcceptAadSize = 1 + 1 + 4 + kKeySize;
        /// A pending session answers a repeated ConnectResponse (its Accept was lost) at most this often
        constexpr std::uint32_t kMaxAcceptResends = 8;

        void PutU32(std::uint8_t *out, std::uint32_t value) noexcept
        {
            for (std::size_t i = 0; i < 4; ++i)
            {
                out[i] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        }

        void PutU64(std::uint8_t *out, std::uint64_t value) noexcept
        {
            for (std::size_t i = 0; i < 8; ++i)
            {
                out[i] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        }

        std::uint32_t GetU32(const std::uint8_t *in) noexcept
        {
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < 4; ++i)
            {
                value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
            }
            return value;
        }

        std::uint64_t GetU64(const std::uint8_t *in) noexcept
        {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < 8; ++i)
            {
                value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
            }
            return value;
        }

        /// The bytes an HMAC, hash or key derivation runs over, built up piece by piece and wiped when done
        class Message
        {
        public:
            Message() { bytes_.reserve(256); }
            ~Message() { Security::SecureZero(bytes_.data(), bytes_.size()); }
            Message(const Message &) = delete;
            Message &operator=(const Message &) = delete;

            Message &Label(std::string_view label)
            {
                bytes_.insert(bytes_.end(), label.begin(), label.end());
                return *this;
            }
            Message &Bytes(std::span<const std::uint8_t> data)
            {
                bytes_.insert(bytes_.end(), data.begin(), data.end());
                return *this;
            }
            Message &U8(std::uint8_t value)
            {
                bytes_.push_back(value);
                return *this;
            }
            Message &U16(std::uint16_t value)
            {
                bytes_.push_back(static_cast<std::uint8_t>(value & 0xFF));
                bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
                return *this;
            }
            Message &U64(std::uint64_t value)
            {
                std::uint8_t raw[8];
                PutU64(raw, value);
                bytes_.insert(bytes_.end(), raw, raw + 8);
                return *this;
            }
            [[nodiscard]] std::span<const std::uint8_t> View() const { return bytes_; }

        private:
            std::vector<std::uint8_t> bytes_;
        };

        bool AllZero(std::span<const std::uint8_t> bytes) noexcept
        {
            return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
        }

        Security::PeerAddress ToPeerAddress(const PeerEndpoint &endpoint)
        {
            if (endpoint.ipv6)
            {
                return Security::PeerAddress::IPv6(endpoint.bytes);
            }
            return Security::PeerAddress::IPv4(endpoint.bytes[0], endpoint.bytes[1], endpoint.bytes[2], endpoint.bytes[3]);
        }

        Security::ChaCha20Poly1305::Nonce MakeNonce(std::uint8_t direction, std::uint64_t sequence) noexcept
        {
            Security::ChaCha20Poly1305::Nonce nonce{};
            nonce[0] = direction;
            PutU64(nonce.data() + 4, sequence);
            return nonce;
        }

        std::uint64_t Elapsed(std::uint64_t now, std::uint64_t since) noexcept
        {
            return now > since ? now - since : 0;
        }

        /// SHA-256 of the transcript: every handshake field from the version to the cookie proof, then the server's key
        bool Transcript(std::span<const std::uint8_t> responseFields, const Security::X25519Key &serverPublic,
                        Bytes32 &transcript)
        {
            Message message;
            message.Label(kLabelTranscript).Bytes(responseFields).Bytes(serverPublic);
            return Security::Sha256(message.View(), transcript);
        }

        /// The two session keys from the shared secret, salted with the transcript; one per direction
        bool DeriveKeys(const Security::X25519Key &shared, const Bytes32 &transcript, Bytes32 &clientToServer,
                        Bytes32 &serverToClient)
        {
            const std::span<const std::uint8_t> c2sInfo(reinterpret_cast<const std::uint8_t *>(kLabelClientToServer.data()),
                                                        kLabelClientToServer.size());
            const std::span<const std::uint8_t> s2cInfo(reinterpret_cast<const std::uint8_t *>(kLabelServerToClient.data()),
                                                        kLabelServerToClient.size());
            const bool c2s = Security::HkdfSha256(transcript, shared, c2sInfo, clientToServer);
            const bool s2c = Security::HkdfSha256(transcript, shared, s2cInfo, serverToClient);
            return c2s && s2c;
        }

        bool ServerProof(const Security::TokenHash &tokenKey, const Bytes32 &transcript, Bytes32 &proof)
        {
            Message message;
            message.Label(kLabelServerProof).Bytes(transcript);
            return Security::HmacSha256(tokenKey, message.View(), proof);
        }

        void BuildSealedAad(std::uint8_t type, std::uint32_t protocolId, std::uint64_t sequence,
                            std::array<std::uint8_t, kSealedAadSize> &aad) noexcept
        {
            aad[0] = type;
            aad[1] = kVersion;
            PutU32(aad.data() + 2, protocolId);
            PutU64(aad.data() + 6, sequence);
        }

        void BuildAcceptAad(std::uint32_t protocolId, const Security::X25519Key &serverPublic,
                            std::array<std::uint8_t, kAcceptAadSize> &aad) noexcept
        {
            aad[0] = kAccept;
            aad[1] = kVersion;
            PutU32(aad.data() + 2, protocolId);
            std::copy(serverPublic.begin(), serverPublic.end(), aad.begin() + 6);
        }

        bool IsSealedType(std::uint8_t type) noexcept
        {
            return type == kData || type == kKeepAlive || type == kDisconnect;
        }
    } // namespace

    // ==================== ReplayWindow ====================

    bool ReplayWindow::Bit(std::uint64_t back) const noexcept
    {
        return ((bits_[back / 64] >> (back % 64)) & 1u) != 0;
    }

    void ReplayWindow::SetBit(std::uint64_t back) noexcept
    {
        bits_[back / 64] |= std::uint64_t{1} << (back % 64);
    }

    void ReplayWindow::ShiftBy(std::uint64_t count) noexcept
    {
        if (count >= kReplayWindow)
        {
            bits_.fill(0);
            return;
        }
        // Bit b moves to bit b + count (the 256-bit map shifted towards "older")
        const std::size_t wordShift = static_cast<std::size_t>(count / 64);
        const unsigned bitShift = static_cast<unsigned>(count % 64);
        std::array<std::uint64_t, kWords> shifted{};
        for (std::size_t i = wordShift; i < kWords; ++i)
        {
            const std::size_t from = i - wordShift;
            std::uint64_t value = bits_[from] << bitShift;
            if (bitShift != 0 && from > 0)
            {
                value |= bits_[from - 1] >> (64 - bitShift);
            }
            shifted[i] = value;
        }
        bits_ = shifted;
    }

    ReplayWindow::Verdict ReplayWindow::Check(std::uint64_t sequence) const noexcept
    {
        if (!any_ || sequence > newest_)
        {
            return Verdict::Fresh;
        }
        const std::uint64_t back = newest_ - sequence;
        if (back >= kReplayWindow)
        {
            return Verdict::TooOld;
        }
        return Bit(back) ? Verdict::Replay : Verdict::Fresh;
    }

    void ReplayWindow::Mark(std::uint64_t sequence) noexcept
    {
        if (!any_)
        {
            any_ = true;
            newest_ = sequence;
            bits_.fill(0);
            SetBit(0);
            return;
        }
        if (sequence > newest_)
        {
            ShiftBy(sequence - newest_);
            newest_ = sequence;
            SetBit(0);
            return;
        }
        const std::uint64_t back = newest_ - sequence;
        if (back < kReplayWindow)
        {
            SetBit(back);
        }
    }

    // ==================== Session ====================

    struct SecureTransport::Session
    {
        /// Server: false from the Accept until the client's first authenticated packet. A client's session is made
        /// established.
        bool established = false;
        PeerEndpoint endpoint{};
        Security::PeerAddress address;
        /// Server, while pending: the pre-auth slot (released when established or dropped)
        Security::PreAuthSlots::Handle slot;
        Security::ChaCha20Poly1305 sendCipher;
        Security::ChaCha20Poly1305 receiveCipher;
        std::uint8_t sendNonceTag = 0;
        std::uint8_t receiveNonceTag = 0;
        /// The next sequence (and nonce) this side seals with; only ever increases
        std::uint64_t nextSendSequence = 0;
        ReplayWindow window;
        std::uint64_t createdAt = 0;
        std::uint64_t lastReceiveAt = 0;
        std::uint64_t lastSendAt = 0;
        /// Server, while pending: the Accept, sent again (byte for byte) for a repeated ConnectResponse
        std::array<std::uint8_t, kAcceptSize> accept{};
        std::uint32_t acceptResends = 0;
        Security::X25519Key clientPublic{};
        SecurePeerIdentity identity;
    };

    // ==================== SecureTransport ====================

    SecureTransport::SecureTransport(ITransport &inner, SecureTransportOptions options)
        : inner_(&inner), options_(std::move(options))
    {
        Initialise();
    }

    SecureTransport::SecureTransport(std::unique_ptr<ITransport> inner, SecureTransportOptions options)
        : owned_(std::move(inner)), inner_(owned_.get()), options_(std::move(options))
    {
        Initialise();
    }

    SecureTransport::~SecureTransport()
    {
        // Sessions hold pre-auth slot handles: give them back before the slots go
        sessions_.clear();
        Security::SecureZero(cookieSecret_.data(), cookieSecret_.size());
        Security::SecureZero(previousCookieSecret_.data(), previousCookieSecret_.size());
        Security::SecureZero(tokenKey_.data(), tokenKey_.size());
        Security::SecureZero(clientPrivate_.data(), clientPrivate_.size());
    }

    void SecureTransport::Initialise()
    {
        options_.cookieLifetimeMicros = std::clamp<std::uint64_t>(options_.cookieLifetimeMicros, 100'000, 60'000'000);
        options_.cookieSecretRotationMicros =
            std::max(options_.cookieSecretRotationMicros, options_.cookieLifetimeMicros * 2);
        options_.handshakeTimeoutMicros = std::max<std::uint64_t>(options_.handshakeTimeoutMicros, 1'000);
        options_.keepAliveMicros = std::max<std::uint64_t>(options_.keepAliveMicros, 1'000);
        options_.idleTimeoutMicros = std::max<std::uint64_t>(options_.idleTimeoutMicros, 1'000);
        options_.resendIntervalMicros = std::max<std::uint64_t>(options_.resendIntervalMicros, 1'000);
        options_.maxSessions = std::max<std::uint32_t>(options_.maxSessions, 1);

        if (inner_ == nullptr || !Security::InitializeCrypto())
        {
            healthy_ = false;
            Security::SecureZero(options_.joinToken.data(), options_.joinToken.size());
            options_.joinToken.clear();
            return;
        }

        // The admission policy reads the same clock as everything else here
        const Security::PreAuthSlots::Clock clock = [this]
        { return std::chrono::steady_clock::time_point(std::chrono::microseconds(NowMicros())); };

        if (options_.role == SecureRole::Server)
        {
            tokens_ = options_.tokens;
            slots_ = std::make_unique<Security::PreAuthSlots>(options_.preAuthSlots, clock);
            challengeBucket_ = std::make_unique<Security::TokenBucket>(options_.challengeRate, clock);
            connectBucket_ = std::make_unique<Security::TokenBucket>(options_.connectRate, clock);
            limiter_ = std::make_unique<Security::RateLimiter>(options_.failures, clock);
            const std::uint64_t nowMillis = NowMicros() / 1000;
            if (!RotateCookieSecret(nowMillis))
            {
                healthy_ = false;
            }
        }
        else
        {
            if (!options_.joinToken.empty())
            {
                const std::optional<Security::TokenHash> hash = HashJoinToken(options_.joinToken);
                if (hash)
                {
                    tokenKey_ = *hash;
                    haveTokenKey_ = true;
                }
            }
        }
        // Keep only the hash: the token's text is wiped from our copy of the options
        Security::SecureZero(options_.joinToken.data(), options_.joinToken.size());
        options_.joinToken.clear();
        options_.joinToken.shrink_to_fit();
    }

    std::uint64_t SecureTransport::NowMicros() const
    {
        if (options_.clockMicros)
        {
            return options_.clockMicros();
        }
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    SecureUdpOpenResult SecureTransport::OpenUdp(const UdpTransportOptions &udpOptions, SecureTransportOptions options)
    {
        SecureUdpOpenResult result;
        // The secure layer goes over every datagram this transport carries: encryption is available
        UdpOpenResult opened = UdpTransport::Open(udpOptions, true);
        if (!opened.transport)
        {
            result.error = opened.error;
            result.message = std::move(opened.message);
            return result;
        }
        UdpTransport *udp = opened.transport.get();
        auto secure = std::make_unique<SecureTransport>(std::unique_ptr<ITransport>(std::move(opened.transport)),
                                                        std::move(options));
        if (!secure->Healthy())
        {
            result.message = "the secure layer could not start (no random generator or crypto library)";
            return result;
        }
        result.udp = udp;
        result.transport = std::move(secure);
        return result;
    }

    std::size_t SecureTransport::MaxPacketSize() const noexcept
    {
        const std::size_t inner = inner_ != nullptr ? inner_->MaxPacketSize() : 0;
        return inner > kSealedOverhead ? inner - kSealedOverhead : 0;
    }

    bool SecureTransport::IsReliableStream() const noexcept
    {
        return inner_ != nullptr && inner_->IsReliableStream();
    }

    bool SecureTransport::RotateCookieSecret(std::uint64_t nowMillis)
    {
        const std::uint64_t rotationMillis = options_.cookieSecretRotationMicros / 1000;
        const std::uint64_t epoch = nowMillis / rotationMillis;
        if (haveCookieSecret_ && epoch == cookieEpoch_)
        {
            return true;
        }
        Bytes32 fresh{};
        if (!Security::RandomBytes(fresh))
        {
            ++stats_.cryptoFailures;
            return false;
        }
        if (haveCookieSecret_ && epoch == cookieEpoch_ + 1)
        {
            // The secret of the epoch just ended still verifies the cookies made near its end
            previousCookieSecret_ = cookieSecret_;
            havePreviousCookieSecret_ = true;
        }
        else
        {
            Security::SecureZero(previousCookieSecret_.data(), previousCookieSecret_.size());
            havePreviousCookieSecret_ = false;
        }
        cookieSecret_ = fresh;
        Security::SecureZero(fresh.data(), fresh.size());
        cookieEpoch_ = epoch;
        haveCookieSecret_ = true;
        return true;
    }

    bool SecureTransport::ComputeCookie(const PeerEndpoint &endpoint, std::uint64_t cookieTime,
                                        std::span<const std::uint8_t> requestFields, const Bytes32 &secret,
                                        Bytes32 &cookie) const
    {
        Message message;
        message.Label(kLabelCookie)
            .U8(endpoint.ipv6 ? 6 : 4)
            .Bytes(endpoint.bytes)
            .U16(endpoint.port)
            .U64(cookieTime)
            .Bytes(requestFields);
        return Security::HmacSha256(secret, message.View(), cookie);
    }

    bool SecureTransport::SendPreAuthReply(PeerId peer, std::span<const std::uint8_t> reply, std::size_t requestSize)
    {
        // The amplification bound: never more bytes to an unvetted source than it sent. True of every reply by
        // construction (the static_asserts in SecureTransport.hpp); checked here as well.
        if (reply.size() > requestSize)
        {
            return false;
        }
        if (inner_->Send(peer, reply) != TransportSendResult::Sent)
        {
            return false;
        }
        stats_.preAuthBytesSent += reply.size();
        stats_.largestPreAuthReply = std::max<std::uint64_t>(stats_.largestPreAuthReply, reply.size());
        stats_.smallestPreAuthRequestAnswered = stats_.smallestPreAuthRequestAnswered == 0
                                                    ? requestSize
                                                    : std::min<std::uint64_t>(stats_.smallestPreAuthRequestAnswered,
                                                                              requestSize);
        return true;
    }

    TransportSendResult SecureTransport::SendSealed(PeerId peer, Session &session, std::uint8_t type,
                                                    std::span<const std::uint8_t> payload, std::uint64_t now)
    {
        const std::size_t size = kSealedOverhead + payload.size();
        if (size > inner_->MaxPacketSize())
        {
            return TransportSendResult::PacketTooLarge;
        }
        if (session.nextSendSequence == std::numeric_limits<std::uint64_t>::max())
        {
            // The last nonce: refuse rather than ever wrap
            return TransportSendResult::UnknownPeer;
        }
        // The sequence is used up before sealing, whatever happens next: a nonce is never sealed with twice
        const std::uint64_t sequence = session.nextSendSequence;
        session.nextSendSequence = sequence + 1;

        std::array<std::uint8_t, kMaxPacketSize> packet{};
        packet[0] = type;
        PutU64(packet.data() + 1, sequence);
        std::array<std::uint8_t, kSealedAadSize> aad{};
        BuildSealedAad(type, options_.protocolId, sequence, aad);
        const Security::ChaCha20Poly1305::Nonce nonce = MakeNonce(session.sendNonceTag, sequence);
        if (!session.sendCipher.Seal(nonce, aad, payload,
                                     std::span<std::uint8_t>(packet.data() + kSealedHeaderSize, payload.size() + kTagSize)))
        {
            ++stats_.cryptoFailures;
            return TransportSendResult::UnknownPeer;
        }
        const TransportSendResult sent = inner_->Send(peer, std::span<const std::uint8_t>(packet.data(), size));
        if (sent == TransportSendResult::Sent)
        {
            ++stats_.sealedSent;
            session.lastSendAt = now;
        }
        return sent;
    }

    TransportSendResult SecureTransport::Send(PeerId peer, std::span<const std::uint8_t> datagram)
    {
        if (datagram.empty())
        {
            return TransportSendResult::EmptyPacket;
        }
        if (datagram.size() > MaxPacketSize())
        {
            return TransportSendResult::PacketTooLarge;
        }
        const auto found = sessions_.find(peer);
        if (!healthy_ || found == sessions_.end() || !found->second->established)
        {
            return TransportSendResult::UnknownPeer;
        }
        const std::uint64_t now = NowMicros();
        return SendSealed(peer, *found->second, kData, datagram, now);
    }

    void SecureTransport::Poll(EventHandler handler)
    {
        if (inPoll_ || inner_ == nullptr)
        {
            return;
        }
        inPoll_ = true;
        const std::uint64_t now = NowMicros();
        // Drain first, then sweep, as UdpTransport::Poll does: a caller that stalled for longer than the idle timeout
        // must not lose sessions whose authenticated packets are already queued. Both steps read the time the Poll
        // began, so a packet that authenticates here is stamped with it and its session is not idle against it.
        inner_->Poll([this, now, handler](const TransportEvent &event) { OnInnerEvent(event, now, handler); });
        RunTimers(now, handler);
        inPoll_ = false;

        // The inner transport forbids Disconnect from inside its Poll: the sessions that ended are let go here
        std::vector<PeerId> disconnects;
        disconnects.swap(deferredInnerDisconnects_);
        for (const PeerId peer : disconnects)
        {
            inner_->Disconnect(peer);
        }
    }

    void SecureTransport::Disconnect(PeerId peer)
    {
        if (inPoll_ || inner_ == nullptr)
        {
            return;
        }
        const auto found = sessions_.find(peer);
        if (found != sessions_.end())
        {
            if (found->second->established)
            {
                // Best effort: the other side otherwise finds out by its idle timeout
                const std::uint64_t now = NowMicros();
                (void)SendSealed(peer, *found->second, kDisconnect, {}, now);
            }
            sessions_.erase(found);
        }
        if (options_.role == SecureRole::Client && peer == serverPeer_)
        {
            clientState_ = SecureClientState::Idle;
            clientFailure_ = SecureClientFailure::None;
            grantedRole_.reset();
            haveCookie_ = false;
            Security::SecureZero(clientPrivate_.data(), clientPrivate_.size());
            serverPeer_ = kInvalidPeer;
        }
        inner_->Disconnect(peer);
    }

    bool SecureTransport::GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const
    {
        const auto found = sessions_.find(peer);
        if (found == sessions_.end() || !found->second->established)
        {
            return false;
        }
        return inner_->GetPeerEndpoint(peer, endpoint);
    }

    bool SecureTransport::Connect(PeerId serverPeer)
    {
        if (!healthy_ || options_.role != SecureRole::Client || !haveTokenKey_ || serverPeer == kInvalidPeer)
        {
            return false;
        }
        if (clientState_ == SecureClientState::Requesting || clientState_ == SecureClientState::Responding ||
            clientState_ == SecureClientState::Connected)
        {
            return false;
        }
        const std::uint64_t now = NowMicros();
        grantedRole_.reset();
        haveCookie_ = false;
        if (!Security::X25519GenerateKeyPair(clientPrivate_, clientPublic_))
        {
            ++stats_.cryptoFailures;
            FailClient(SecureClientFailure::CryptoFailure);
            return false;
        }
        Message message;
        std::uint8_t protocolId[4];
        PutU32(protocolId, options_.protocolId);
        message.Label(kLabelRequestProof).U8(kVersion).Bytes(protocolId).Bytes(clientPublic_);
        if (!Security::HmacSha256(tokenKey_, message.View(), requestProof_))
        {
            ++stats_.cryptoFailures;
            FailClient(SecureClientFailure::CryptoFailure);
            return false;
        }
        serverPeer_ = serverPeer;
        clientFailure_ = SecureClientFailure::None;
        clientState_ = SecureClientState::Requesting;
        handshakeStartedAt_ = now;
        SendClientHandshake(now);
        return true;
    }

    void SecureTransport::SendClientHandshake(std::uint64_t now)
    {
        std::array<std::uint8_t, kHandshakeRequestSize> packet{};
        packet[kOffsetVersion] = kVersion;
        PutU32(packet.data() + kOffsetProtocolId, options_.protocolId);
        std::copy(clientPublic_.begin(), clientPublic_.end(), packet.begin() + kOffsetClientPublic);
        std::copy(requestProof_.begin(), requestProof_.end(), packet.begin() + kOffsetRequestProof);
        if (clientState_ == SecureClientState::Responding)
        {
            packet[0] = kConnectResponse;
            PutU64(packet.data() + kOffsetCookieTime, cookieTime_);
            std::copy(cookie_.begin(), cookie_.end(), packet.begin() + kOffsetCookie);
            std::copy(cookieProof_.begin(), cookieProof_.end(), packet.begin() + kOffsetCookieProof);
        }
        else
        {
            packet[0] = kConnectRequest;
        }
        (void)inner_->Send(serverPeer_, packet);
        lastHandshakeSendAt_ = now;
    }

    void SecureTransport::FailClient(SecureClientFailure failure)
    {
        clientState_ = SecureClientState::Failed;
        clientFailure_ = failure;
        haveCookie_ = false;
        Security::SecureZero(clientPrivate_.data(), clientPrivate_.size());
    }

    void SecureTransport::EndSession(PeerId peer, bool notifyUpper, EventHandler *handler)
    {
        const auto found = sessions_.find(peer);
        if (found == sessions_.end())
        {
            return;
        }
        const bool wasEstablished = found->second->established;
        sessions_.erase(found);
        deferredInnerDisconnects_.push_back(peer);
        if (notifyUpper && wasEstablished && handler != nullptr)
        {
            TransportEvent event;
            event.type = TransportEventType::PeerDisconnected;
            event.peer = peer;
            (*handler)(event);
        }
    }

    void SecureTransport::RunTimers(std::uint64_t now, EventHandler handler)
    {
        if (options_.role == SecureRole::Client &&
            (clientState_ == SecureClientState::Requesting || clientState_ == SecureClientState::Responding))
        {
            if (Elapsed(now, handshakeStartedAt_) >= options_.handshakeTimeoutMicros)
            {
                FailClient(SecureClientFailure::HandshakeTimeout);
            }
            else if (clientState_ == SecureClientState::Responding &&
                     Elapsed(now, respondingSince_) >= options_.cookieLifetimeMicros)
            {
                // The cookie has expired by now: ask for a new one (a late Accept for the old one is still taken)
                clientState_ = SecureClientState::Requesting;
                SendClientHandshake(now);
            }
            else if (Elapsed(now, lastHandshakeSendAt_) >= options_.resendIntervalMicros)
            {
                SendClientHandshake(now);
            }
        }

        std::vector<PeerId> expired;
        std::vector<PeerId> idle;
        std::vector<PeerId> keepAlive;
        for (const auto &[peer, session] : sessions_)
        {
            if (!session->established)
            {
                if (Elapsed(now, session->createdAt) >= options_.handshakeTimeoutMicros)
                {
                    expired.push_back(peer);
                }
            }
            else if (Elapsed(now, session->lastReceiveAt) >= options_.idleTimeoutMicros)
            {
                idle.push_back(peer);
            }
            else if (Elapsed(now, session->lastSendAt) >= options_.keepAliveMicros)
            {
                keepAlive.push_back(peer);
            }
        }
        for (const PeerId peer : expired)
        {
            // An Accept that was never answered with a sealed packet: a failure of that address, like the editor
            // host's unfinished Hello
            ++stats_.handshakeTimeouts;
            if (limiter_)
            {
                (void)limiter_->RecordUnfinishedHello(sessions_.at(peer)->address);
            }
            EndSession(peer, false, nullptr);
        }
        for (const PeerId peer : idle)
        {
            ++stats_.idleTimeouts;
            EndSession(peer, true, &handler);
            if (options_.role == SecureRole::Client && peer == serverPeer_)
            {
                FailClient(SecureClientFailure::IdleTimeout);
            }
        }
        for (const PeerId peer : keepAlive)
        {
            const auto found = sessions_.find(peer);
            if (found == sessions_.end())
            {
                continue;
            }
            if (found->second->nextSendSequence == std::numeric_limits<std::uint64_t>::max())
            {
                EndSession(peer, true, &handler);
                if (options_.role == SecureRole::Client && peer == serverPeer_)
                {
                    FailClient(SecureClientFailure::SequenceExhausted);
                }
                continue;
            }
            (void)SendSealed(peer, *found->second, kKeepAlive, {}, now);
        }
    }

    void SecureTransport::OnInnerEvent(const TransportEvent &event, std::uint64_t now, EventHandler handler)
    {
        if (event.type == TransportEventType::PeerConnected)
        {
            // A link below is not a peer above: only a completed handshake is
            return;
        }
        if (event.type == TransportEventType::PeerDisconnected)
        {
            const auto found = sessions_.find(event.peer);
            if (found != sessions_.end())
            {
                const bool wasEstablished = found->second->established;
                if (!wasEstablished && limiter_)
                {
                    (void)limiter_->RecordUnfinishedHello(found->second->address);
                }
                sessions_.erase(found);
                if (wasEstablished)
                {
                    TransportEvent gone;
                    gone.type = TransportEventType::PeerDisconnected;
                    gone.peer = event.peer;
                    handler(gone);
                }
            }
            if (options_.role == SecureRole::Client && event.peer == serverPeer_ &&
                clientState_ != SecureClientState::Idle && clientState_ != SecureClientState::Failed)
            {
                FailClient(SecureClientFailure::TransportClosed);
            }
            return;
        }

        const std::span<const std::uint8_t> data = event.data;
        if (!healthy_ || data.empty())
        {
            ++stats_.droppedMalformed;
            return;
        }
        const std::uint8_t type = data[0];
        if (IsSealedType(type))
        {
            OnSealed(event.peer, data, now, handler);
            return;
        }
        if (options_.role == SecureRole::Server)
        {
            if (type == kConnectRequest)
            {
                OnConnectRequest(event.peer, data, now);
            }
            else if (type == kConnectResponse)
            {
                OnConnectResponse(event.peer, data, now);
            }
            else
            {
                ++stats_.droppedMalformed;
            }
            return;
        }
        if (event.peer != serverPeer_)
        {
            ++stats_.droppedMalformed;
            return;
        }
        if (type == kChallenge)
        {
            OnChallenge(event.peer, data, now);
        }
        else if (type == kAccept)
        {
            OnAccept(event.peer, data, now, handler);
        }
        else
        {
            ++stats_.droppedMalformed;
        }
    }

    void SecureTransport::OnConnectRequest(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now)
    {
        ++stats_.requestsReceived;
        stats_.preAuthBytesReceived += data.size();
        if (data.size() != kHandshakeRequestSize || !AllZero(data.subspan(kRequestUsed)))
        {
            ++stats_.droppedMalformed;
            return;
        }
        if (data[kOffsetVersion] != kVersion || GetU32(data.data() + kOffsetProtocolId) != options_.protocolId)
        {
            ++stats_.droppedWrongProtocol;
            return;
        }
        if (sessions_.find(peer) != sessions_.end())
        {
            ++stats_.droppedDuplicate;
            return;
        }
        PeerEndpoint endpoint;
        if (!inner_->GetPeerEndpoint(peer, endpoint))
        {
            ++stats_.droppedNoAddress;
            return;
        }
        const Security::PeerAddress address = ToPeerAddress(endpoint);
        if (!limiter_->AdmitConnection(address).allowed)
        {
            ++stats_.droppedBackedOff;
            return;
        }
        if (!challengeBucket_->TryTake(address))
        {
            ++stats_.droppedRateLimited;
            return;
        }
        const std::uint64_t nowMillis = now / 1000;
        if (!RotateCookieSecret(nowMillis))
        {
            return;
        }
        // Stateless: the cookie is a MAC of everything the server will need to check the echo, so nothing is kept
        Bytes32 cookie{};
        if (!ComputeCookie(endpoint, nowMillis, data.subspan(kOffsetVersion, kRequestUsed - kOffsetVersion),
                           cookieSecret_, cookie))
        {
            ++stats_.cryptoFailures;
            return;
        }
        std::array<std::uint8_t, kChallengeSize> reply{};
        reply[0] = kChallenge;
        PutU64(reply.data() + 1, nowMillis);
        std::copy(cookie.begin(), cookie.end(), reply.begin() + 9);
        if (SendPreAuthReply(peer, reply, data.size()))
        {
            ++stats_.challengesSent;
        }
    }

    void SecureTransport::OnConnectResponse(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now)
    {
        ++stats_.responsesReceived;
        stats_.preAuthBytesReceived += data.size();
        if (data.size() != kHandshakeRequestSize || !AllZero(data.subspan(kResponseUsed)))
        {
            ++stats_.droppedMalformed;
            return;
        }
        if (data[kOffsetVersion] != kVersion || GetU32(data.data() + kOffsetProtocolId) != options_.protocolId)
        {
            ++stats_.droppedWrongProtocol;
            return;
        }
        Security::X25519Key clientPublic{};
        std::copy_n(data.begin() + kOffsetClientPublic, kKeySize, clientPublic.begin());

        if (const auto found = sessions_.find(peer); found != sessions_.end())
        {
            Session &session = *found->second;
            // A repeated ConnectResponse while pending: its Accept was lost. Answer with the same bytes (no new keys,
            // no new nonce), a few times at most, never more than the request's size.
            if (!session.established && session.clientPublic == clientPublic && session.acceptResends < kMaxAcceptResends &&
                SendPreAuthReply(peer, session.accept, data.size()))
            {
                ++session.acceptResends;
                ++stats_.acceptsResent;
                return;
            }
            ++stats_.droppedDuplicate;
            return;
        }

        PeerEndpoint endpoint;
        if (!inner_->GetPeerEndpoint(peer, endpoint))
        {
            ++stats_.droppedNoAddress;
            return;
        }
        const Security::PeerAddress address = ToPeerAddress(endpoint);
        if (!limiter_->AdmitConnection(address).allowed)
        {
            ++stats_.droppedBackedOff;
            return;
        }
        // 1. The cookie: made by this process (a secret it still holds), for this address and port and these request
        //    fields, and recent. Nothing has been allocated for the peer up to here. It is checked BEFORE the connect
        //    rate is charged, so echoes from spoofed sources (which cannot hold a valid cookie) cost one HMAC and
        //    never use up the budget meant for address-validated peers.
        const std::uint64_t nowMillis = now / 1000;
        if (!RotateCookieSecret(nowMillis))
        {
            return;
        }
        const std::uint64_t cookieTime = GetU64(data.data() + kOffsetCookieTime);
        const std::uint64_t rotationMillis = options_.cookieSecretRotationMicros / 1000;
        const std::uint64_t cookieEpoch = cookieTime / rotationMillis;
        const Bytes32 *secret = nullptr;
        if (cookieEpoch == cookieEpoch_)
        {
            secret = &cookieSecret_;
        }
        else if (havePreviousCookieSecret_ && cookieEpoch_ > 0 && cookieEpoch == cookieEpoch_ - 1)
        {
            secret = &previousCookieSecret_;
        }
        if (secret == nullptr)
        {
            ++stats_.droppedBadCookie;
            return;
        }
        Bytes32 expected{};
        if (!ComputeCookie(endpoint, cookieTime, data.subspan(kOffsetVersion, kRequestUsed - kOffsetVersion), *secret,
                           expected))
        {
            ++stats_.cryptoFailures;
            return;
        }
        if (!Security::ConstantTimeEqual(expected, data.subspan(kOffsetCookie, kMacSize)))
        {
            ++stats_.droppedBadCookie;
            return;
        }
        if (cookieTime > nowMillis || nowMillis - cookieTime > options_.cookieLifetimeMicros / 1000)
        {
            ++stats_.droppedExpiredCookie;
            return;
        }
        if (!connectBucket_->TryTake(address))
        {
            ++stats_.droppedRateLimited;
            return;
        }

        // 2. Only now does the peer cost state: a session slot and a pre-auth slot
        if (sessions_.size() >= options_.maxSessions)
        {
            ++stats_.droppedSessionLimit;
            return;
        }
        Security::PreAuthSlots::Handle slot = slots_->TryAcquire(address);
        if (!slot)
        {
            ++stats_.droppedNoSlot;
            return;
        }

        // 3. The token: both proofs against EVERY entry, in full, with no early exit; the matching entry is selected
        //    with a mask, so the time depends on the table's size, not on which entry matched or how close a wrong
        //    proof came
        const std::shared_ptr<const JoinTokenTable> tokens = tokens_;
        Message requestMessage;
        requestMessage.Label(kLabelRequestProof)
            .Bytes(data.subspan(kOffsetVersion, kOffsetRequestProof - kOffsetVersion));
        Message cookieMessage;
        cookieMessage.Label(kLabelCookieProof).U64(cookieTime).Bytes(data.subspan(kOffsetCookie, kMacSize));
        const std::span<const std::uint8_t> requestProof = data.subspan(kOffsetRequestProof, kMacSize);
        const std::span<const std::uint8_t> cookieProof = data.subspan(kOffsetCookieProof, kMacSize);

        constexpr std::size_t kNone = static_cast<std::size_t>(-1);
        std::size_t match = kNone;
        bool macFailed = false;
        const std::size_t entryCount = tokens ? tokens->Size() : 0;
        for (std::size_t i = 0; i < entryCount; ++i)
        {
            const Security::TokenHash &key = tokens->Entries()[i].hash;
            Bytes32 requestMac{};
            Bytes32 cookieMac{};
            const bool requestOk = Security::HmacSha256(key, requestMessage.View(), requestMac);
            const bool cookieOk = Security::HmacSha256(key, cookieMessage.View(), cookieMac);
            const bool requestMatches = Security::ConstantTimeEqual(requestMac, requestProof);
            const bool cookieMatches = Security::ConstantTimeEqual(cookieMac, cookieProof);
            Security::SecureZero(requestMac.data(), requestMac.size());
            Security::SecureZero(cookieMac.data(), cookieMac.size());
            macFailed = macFailed || !requestOk || !cookieOk;
            const bool both = requestOk & cookieOk & requestMatches & cookieMatches;
            const std::size_t mask = static_cast<std::size_t>(0) - static_cast<std::size_t>(both);
            match = (mask & i) | (~mask & match);
        }
        if (macFailed)
        {
            ++stats_.cryptoFailures;
        }
        if (match == kNone)
        {
            ++stats_.droppedBadToken;
            (void)limiter_->RecordFailure(address, std::nullopt);
            return; // the slot goes back with `slot`
        }
        JoinTokenEntry entry = tokens->Entries()[match];

        // 4. The key exchange: our ephemeral X25519 key, the shared secret, the transcript, one key per direction
        Security::X25519Key serverPrivate{};
        Security::X25519Key serverPublic{};
        Security::X25519Key shared{};
        Bytes32 transcript{};
        Bytes32 clientToServer{};
        Bytes32 serverToClient{};
        Bytes32 serverProof{};
        const auto wipe = [&]
        {
            Security::SecureZero(serverPrivate.data(), serverPrivate.size());
            Security::SecureZero(shared.data(), shared.size());
            Security::SecureZero(clientToServer.data(), clientToServer.size());
            Security::SecureZero(serverToClient.data(), serverToClient.size());
            Security::SecureZero(serverProof.data(), serverProof.size());
            Security::SecureZero(entry.hash.data(), entry.hash.size());
        };
        const bool keysOk = Security::X25519GenerateKeyPair(serverPrivate, serverPublic) &&
                            Security::X25519SharedSecret(serverPrivate, clientPublic, shared) &&
                            Transcript(data.subspan(kOffsetVersion, kResponseUsed - kOffsetVersion), serverPublic,
                                       transcript) &&
                            DeriveKeys(shared, transcript, clientToServer, serverToClient) &&
                            ServerProof(entry.hash, transcript, serverProof);
        if (!keysOk)
        {
            // A refused client key (low order) lands here too
            ++stats_.cryptoFailures;
            wipe();
            return;
        }

        auto session = std::make_unique<Session>();
        session->endpoint = endpoint;
        session->address = address;
        session->slot = std::move(slot);
        const bool sendKeyOk = session->sendCipher.SetKey(serverToClient);
        const bool receiveKeyOk = session->receiveCipher.SetKey(clientToServer);
        session->sendNonceTag = kNonceServerToClient;
        session->receiveNonceTag = kNonceClientToServer;
        // Sequence 0 of the server-to-client key seals the Accept; data starts at 1
        session->nextSendSequence = 1;
        session->createdAt = now;
        session->lastReceiveAt = now;
        session->lastSendAt = now;
        session->clientPublic = clientPublic;
        session->identity.user = entry.user;
        session->identity.role = entry.role;

        std::array<std::uint8_t, kAcceptPlainSize> plain{};
        plain[0] = static_cast<std::uint8_t>(entry.role);
        std::copy(serverProof.begin(), serverProof.end(), plain.begin() + 1);
        std::array<std::uint8_t, kAcceptAadSize> aad{};
        BuildAcceptAad(options_.protocolId, serverPublic, aad);
        session->accept[0] = kAccept;
        std::copy(serverPublic.begin(), serverPublic.end(), session->accept.begin() + 1);
        const bool sealed =
            sendKeyOk && receiveKeyOk &&
            session->sendCipher.Seal(MakeNonce(kNonceServerToClient, 0), aad, plain,
                                     std::span<std::uint8_t>(session->accept.data() + 1 + kKeySize,
                                                             kAcceptPlainSize + kTagSize));
        Security::SecureZero(plain.data(), plain.size());
        wipe();
        if (!sealed)
        {
            ++stats_.cryptoFailures;
            return;
        }

        Session &stored = *session;
        sessions_[peer] = std::move(session);
        if (SendPreAuthReply(peer, stored.accept, data.size()))
        {
            ++stats_.acceptsSent;
        }
    }

    void SecureTransport::OnChallenge(PeerId /*peer*/, std::span<const std::uint8_t> data, std::uint64_t now)
    {
        // Only while Requesting. Once Responding the client keeps the cookie it echoed: a later Challenge (the answer
        // to a resent ConnectRequest when the round trip exceeds resendIntervalMicros) carries another cookie, and
        // taking it would make the client's transcript differ from the one the server built its session (and its
        // Accept) from. RunTimers goes back to Requesting when the cookie has expired.
        if (clientState_ != SecureClientState::Requesting)
        {
            ++stats_.droppedDuplicate;
            return;
        }
        if (data.size() != kChallengeSize)
        {
            ++stats_.droppedMalformed;
            return;
        }
        const std::uint64_t cookieTime = GetU64(data.data() + 1);
        Bytes32 cookie{};
        std::copy_n(data.begin() + 9, kMacSize, cookie.begin());
        Message message;
        message.Label(kLabelCookieProof).U64(cookieTime).Bytes(cookie);
        Bytes32 proof{};
        if (!Security::HmacSha256(tokenKey_, message.View(), proof))
        {
            ++stats_.cryptoFailures;
            return;
        }
        cookieTime_ = cookieTime;
        cookie_ = cookie;
        cookieProof_ = proof;
        haveCookie_ = true;
        clientState_ = SecureClientState::Responding;
        respondingSince_ = now;
        SendClientHandshake(now);
    }

    void SecureTransport::OnAccept(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now,
                                   EventHandler handler)
    {
        const bool expecting = clientState_ == SecureClientState::Responding ||
                               (clientState_ == SecureClientState::Requesting && haveCookie_);
        if (!expecting)
        {
            ++stats_.droppedDuplicate;
            return;
        }
        if (data.size() != kAcceptSize)
        {
            ++stats_.droppedMalformed;
            return;
        }
        Security::X25519Key serverPublic{};
        std::copy_n(data.begin() + 1, kKeySize, serverPublic.begin());

        // The response fields exactly as sent: the transcript must be the server's byte for byte
        std::array<std::uint8_t, kResponseUsed - kOffsetVersion> fields{};
        fields[0] = kVersion;
        PutU32(fields.data() + (kOffsetProtocolId - kOffsetVersion), options_.protocolId);
        std::copy(clientPublic_.begin(), clientPublic_.end(), fields.begin() + (kOffsetClientPublic - kOffsetVersion));
        std::copy(requestProof_.begin(), requestProof_.end(), fields.begin() + (kOffsetRequestProof - kOffsetVersion));
        PutU64(fields.data() + (kOffsetCookieTime - kOffsetVersion), cookieTime_);
        std::copy(cookie_.begin(), cookie_.end(), fields.begin() + (kOffsetCookie - kOffsetVersion));
        std::copy(cookieProof_.begin(), cookieProof_.end(), fields.begin() + (kOffsetCookieProof - kOffsetVersion));

        Security::X25519Key shared{};
        Bytes32 transcript{};
        Bytes32 clientToServer{};
        Bytes32 serverToClient{};
        Bytes32 expectedProof{};
        std::array<std::uint8_t, kAcceptPlainSize> plain{};
        const auto wipe = [&]
        {
            Security::SecureZero(shared.data(), shared.size());
            Security::SecureZero(clientToServer.data(), clientToServer.size());
            Security::SecureZero(serverToClient.data(), serverToClient.size());
            Security::SecureZero(expectedProof.data(), expectedProof.size());
            Security::SecureZero(plain.data(), plain.size());
        };
        const bool keysOk = Security::X25519SharedSecret(clientPrivate_, serverPublic, shared) &&
                            Transcript(fields, serverPublic, transcript) &&
                            DeriveKeys(shared, transcript, clientToServer, serverToClient) &&
                            ServerProof(tokenKey_, transcript, expectedProof);
        if (!keysOk)
        {
            ++stats_.droppedBadAccept;
            wipe();
            return;
        }

        auto session = std::make_unique<Session>();
        const bool sendKeyOk = session->sendCipher.SetKey(clientToServer);
        const bool receiveKeyOk = session->receiveCipher.SetKey(serverToClient);
        std::array<std::uint8_t, kAcceptAadSize> aad{};
        BuildAcceptAad(options_.protocolId, serverPublic, aad);
        const bool opened = sendKeyOk && receiveKeyOk &&
                            session->receiveCipher.Open(MakeNonce(kNonceServerToClient, 0), aad,
                                                        data.subspan(1 + kKeySize, kAcceptPlainSize + kTagSize), plain);
        // The role byte and the server's proof that it holds our token's hash (compared in constant time)
        const bool proofOk = opened && plain[0] <= static_cast<std::uint8_t>(JoinRole::Admin) &&
                             Security::ConstantTimeEqual(expectedProof, std::span<const std::uint8_t>(plain).subspan(1));
        if (!proofOk)
        {
            ++stats_.droppedBadAccept;
            wipe();
            return;
        }
        const JoinRole role = static_cast<JoinRole>(plain[0]);
        wipe();

        session->established = true;
        (void)inner_->GetPeerEndpoint(peer, session->endpoint);
        session->sendNonceTag = kNonceClientToServer;
        session->receiveNonceTag = kNonceServerToClient;
        session->nextSendSequence = 0;
        session->window.Mark(0); // the Accept was sequence 0 of the server's key
        session->createdAt = now;
        session->lastReceiveAt = now;
        session->lastSendAt = now;
        session->clientPublic = clientPublic_;

        // Forward secrecy: the ephemeral private key is not needed again
        Security::SecureZero(clientPrivate_.data(), clientPrivate_.size());
        haveCookie_ = false;
        clientState_ = SecureClientState::Connected;
        grantedRole_ = role;
        ++stats_.sessionsEstablished;

        Session &stored = *session;
        sessions_[peer] = std::move(session);
        TransportEvent connected;
        connected.type = TransportEventType::PeerConnected;
        connected.peer = peer;
        handler(connected);
        // Key confirmation for the server: our first sealed packet, at once
        const auto still = sessions_.find(peer);
        if (still != sessions_.end() && still->second.get() == &stored)
        {
            (void)SendSealed(peer, stored, kKeepAlive, {}, now);
        }
    }

    void SecureTransport::OnSealed(PeerId peer, std::span<const std::uint8_t> data, std::uint64_t now,
                                   EventHandler handler)
    {
        if (data.size() < kSealedOverhead || data.size() > kMaxPacketSize)
        {
            ++stats_.droppedMalformed;
            return;
        }
        const auto found = sessions_.find(peer);
        if (found == sessions_.end())
        {
            ++stats_.droppedNoSession;
            return;
        }
        Session &session = *found->second;
        const std::uint8_t type = data[0];
        const std::uint64_t sequence = GetU64(data.data() + 1);
        const ReplayWindow::Verdict verdict = session.window.Check(sequence);
        if (verdict == ReplayWindow::Verdict::Replay)
        {
            ++stats_.droppedReplay;
            return;
        }
        if (verdict == ReplayWindow::Verdict::TooOld)
        {
            ++stats_.droppedTooOld;
            return;
        }
        std::array<std::uint8_t, kSealedAadSize> aad{};
        BuildSealedAad(type, options_.protocolId, sequence, aad);
        const std::size_t length = data.size() - kSealedOverhead;
        std::array<std::uint8_t, kMaxPacketSize> plain{};
        if (!session.receiveCipher.Open(MakeNonce(session.receiveNonceTag, sequence), aad,
                                        data.subspan(kSealedHeaderSize), std::span<std::uint8_t>(plain.data(), length)))
        {
            ++stats_.droppedAuthFailed;
            return;
        }
        // Authenticated: only now does the packet count, move the window or refresh the session
        session.window.Mark(sequence);
        session.lastReceiveAt = now;
        ++stats_.sealedReceived;

        if (!session.established)
        {
            // The client has proved it holds the keys: the handshake is over, the pre-auth slot goes back
            session.established = true;
            session.slot.Release();
            if (limiter_)
            {
                limiter_->RecordSuccess(session.address, std::nullopt);
            }
            if (slots_)
            {
                slots_->RecordAuthenticated(session.address);
            }
            ++stats_.sessionsEstablished;
            TransportEvent connected;
            connected.type = TransportEventType::PeerConnected;
            connected.peer = peer;
            handler(connected);
        }

        if (type == kData && length > 0)
        {
            TransportEvent datagram;
            datagram.type = TransportEventType::Datagram;
            datagram.peer = peer;
            datagram.data = std::span<const std::uint8_t>(plain.data(), length);
            handler(datagram);
        }
        else if (type == kDisconnect)
        {
            EndSession(peer, true, &handler);
            if (options_.role == SecureRole::Client && peer == serverPeer_)
            {
                FailClient(SecureClientFailure::ClosedByServer);
            }
        }
        Security::SecureZero(plain.data(), length);
    }

    void SecureTransport::SetTokens(std::shared_ptr<const JoinTokenTable> tokens)
    {
        tokens_ = std::move(tokens);
    }

    bool SecureTransport::GetPeerIdentity(PeerId peer, SecurePeerIdentity &identity) const
    {
        const auto found = sessions_.find(peer);
        if (options_.role != SecureRole::Server || found == sessions_.end() || !found->second->established)
        {
            return false;
        }
        identity = found->second->identity;
        return true;
    }

    std::size_t SecureTransport::PendingSessionCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(sessions_.begin(), sessions_.end(),
                                                       [](const auto &item) { return !item.second->established; }));
    }

    std::size_t SecureTransport::EstablishedSessionCount() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(sessions_.begin(), sessions_.end(),
                                                       [](const auto &item) { return item.second->established; }));
    }

    std::size_t SecureTransport::PreAuthSlotsInUse() const
    {
        return slots_ ? slots_->InUse() : 0;
    }
} // namespace N2Engine::N2Net

#endif // N2NET_HAS_SECURE
