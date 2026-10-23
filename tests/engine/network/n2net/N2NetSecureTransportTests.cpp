#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/SecureTransport.hpp"

// The secure layer (net-a5): the cookie handshake, the sealed packets and their replay window, the amplification bound,
// the rate limits and the join tokens, over the deterministic SimNetwork with a virtual clock (no sleeps), and once end to
// end over loopback UDP with the A3 channels on top. Native only for the parts that need the crypto library: the web
// build has none, so there only the Connection packet-size test below runs.

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

// ==================== ConnectionConfig::maxPacketSize (no crypto needed) ====================

TEST(N2NetConnectionPacketSizeTest, PacketsNeverExceedTheConfiguredSizeAndLargeMessagesStillGetThrough)
{
    ConnectionConfig config = TestConfig();
    config.maxPacketSize = 1175;
    Connection sender(config, 0);
    Connection receiver(config, 0);
    EXPECT_EQ(sender.Config().maxPacketSize, 1175u);
    EXPECT_EQ(sender.MaxUnreliableMessageSize(), 1175u - kHeaderSize - kUnreliableHeaderSize);

    // An unreliable message one byte over the configured room is refused; one that fits is taken
    EXPECT_EQ(sender.SendUnreliable(MakeMessage(1, sender.MaxUnreliableMessageSize() + 1)), SendResult::TooLarge);
    EXPECT_EQ(sender.SendUnreliable(MakeMessage(2, sender.MaxUnreliableMessageSize())), SendResult::Ok);
    ASSERT_EQ(sender.SendReliable(MakeMessage(3, 10'000)), SendResult::Ok);

    std::vector<std::vector<std::uint8_t>> delivered;
    std::size_t largest = 0;
    std::uint64_t now = 0;
    for (int round = 0; round < 50 && delivered.size() < 2; ++round)
    {
        now += 1'000;
        std::vector<std::vector<std::uint8_t>> packets;
        sender.Update(now, [&](std::span<const std::uint8_t> packet) { packets.emplace_back(packet.begin(), packet.end()); });
        for (const auto &packet : packets)
        {
            largest = std::max(largest, packet.size());
            receiver.OnPacket(now, packet, [&](MessageChannel, std::span<const std::uint8_t> message)
                              { delivered.emplace_back(message.begin(), message.end()); });
        }
        std::vector<std::vector<std::uint8_t>> acks;
        receiver.Update(now, [&](std::span<const std::uint8_t> packet) { acks.emplace_back(packet.begin(), packet.end()); });
        for (const auto &ack : acks)
        {
            sender.OnPacket(now, ack, [](MessageChannel, std::span<const std::uint8_t>) {});
        }
    }
    EXPECT_LE(largest, 1175u);
    EXPECT_GT(largest, 1000u) << "units fill the smaller packets";
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_TRUE(std::find(delivered.begin(), delivered.end(), MakeMessage(3, 10'000)) != delivered.end());

    // Clamped: never below the minimum, never above kMaxPacketSize
    ConnectionConfig tiny = TestConfig();
    tiny.maxPacketSize = 10;
    EXPECT_EQ(Connection(tiny, 0).Config().maxPacketSize, kMinConfiguredPacketSize);
    ConnectionConfig huge = TestConfig();
    huge.maxPacketSize = 100'000;
    EXPECT_EQ(Connection(huge, 0).Config().maxPacketSize, kMaxPacketSize);
}

#if N2NET_HAS_SECURE

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "engine/security/Crypto.hpp"

namespace
{
    namespace Wire = SecureWire;

    constexpr std::uint32_t kProtocol = 0x4E32'5345; // "N2SE"
    constexpr std::uint64_t kStartMicros = 1'000'000'000; // 1000 s: away from 0, inside one cookie-secret epoch
    const std::string kToken = "test-join-token-5f1c0a77e2b94d6c8a3e1b2f9d0c4e6a";

    std::string Hex(std::span<const std::uint8_t> bytes)
    {
        static const char *digits = "0123456789abcdef";
        std::string text;
        for (const std::uint8_t byte : bytes)
        {
            text += digits[byte >> 4];
            text += digits[byte & 0x0F];
        }
        return text;
    }

    std::vector<std::uint8_t> FromHex(std::string_view text)
    {
        std::vector<std::uint8_t> bytes;
        for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        {
            bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
        }
        return bytes;
    }

    std::span<const std::uint8_t> AsBytes(std::string_view text)
    {
        return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
    }

    /// A one-entry table for `token`
    std::shared_ptr<const JoinTokenTable> TableFor(const std::string &token, const std::string &user, const char *role)
    {
        const auto hash = HashJoinToken(token);
        EXPECT_TRUE(hash.has_value());
        std::string text = "[{\"user\":\"" + user + "\",\"role\":\"" + role + "\",\"hash\":\"" +
                           N2Engine::Security::FormatTokenHash(*hash) + "\"}]";
        auto table = JoinTokenTable::Parse(text);
        EXPECT_TRUE(table.has_value()) << (table ? "" : table.error());
        return std::make_shared<const JoinTokenTable>(std::move(*table));
    }

    struct Event
    {
        TransportEventType type = TransportEventType::Datagram;
        PeerId peer = kInvalidPeer;
        std::vector<std::uint8_t> data;
    };

    void PollInto(ITransport &transport, std::vector<Event> &events)
    {
        transport.Poll(
            [&](const TransportEvent &event)
            {
                Event copy;
                copy.type = event.type;
                copy.peer = event.peer;
                copy.data.assign(event.data.begin(), event.data.end());
                events.push_back(std::move(copy));
            });
    }

    std::size_t CountOf(const std::vector<Event> &events, TransportEventType type)
    {
        return static_cast<std::size_t>(
            std::count_if(events.begin(), events.end(), [type](const Event &event) { return event.type == type; }));
    }

    /// Between a SecureTransport and its transport: records every datagram sent and can send raw bytes, the stand-in
    /// for an attacker on the path who captures, replays and alters packets
    class TapTransport final : public ITransport
    {
    public:
        explicit TapTransport(ITransport &inner) : inner_(inner) {}

        [[nodiscard]] std::size_t MaxPacketSize() const noexcept override { return inner_.MaxPacketSize(); }
        [[nodiscard]] bool IsReliableStream() const noexcept override { return inner_.IsReliableStream(); }
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> datagram) override
        {
            sent.emplace_back(datagram.begin(), datagram.end());
            return inner_.Send(peer, datagram);
        }
        void Poll(EventHandler handler) override { inner_.Poll(handler); }
        void Disconnect(PeerId peer) override { inner_.Disconnect(peer); }
        [[nodiscard]] bool GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const override
        {
            return inner_.GetPeerEndpoint(peer, endpoint);
        }

        /// Sends bytes the secure layer never made
        TransportSendResult Inject(PeerId peer, std::span<const std::uint8_t> datagram) { return inner_.Send(peer, datagram); }

        std::vector<std::vector<std::uint8_t>> sent;

    private:
        ITransport &inner_;
    };

    /// A server and a client SecureTransport over a perfect SimNetwork link, both reading one virtual clock
    struct SecurePair
    {
        SimNetwork net{0x5EC0'0001};
        std::uint64_t clock = kStartMicros;
        SimTransport &serverEnd;
        SimTransport &clientEnd;
        TapTransport tap;
        std::unique_ptr<SecureTransport> server;
        std::unique_ptr<SecureTransport> client;
        std::vector<Event> serverEvents;
        std::vector<Event> clientEvents;

        explicit SecurePair(const std::string &clientToken = kToken, const char *role = "player",
                            std::function<void(SecureTransportOptions &)> tweakServer = {})
            : serverEnd(net.AddEndpoint()), clientEnd(net.AddEndpoint()), tap(clientEnd)
        {
            net.Link(serverEnd.Id(), clientEnd.Id(), SimLinkConfig{});

            SecureTransportOptions serverOptions;
            serverOptions.role = SecureRole::Server;
            serverOptions.protocolId = kProtocol;
            serverOptions.tokens = TableFor(kToken, "alice", role);
            serverOptions.clockMicros = [this] { return clock; };
            if (tweakServer)
            {
                tweakServer(serverOptions);
            }
            server = std::make_unique<SecureTransport>(serverEnd, std::move(serverOptions));

            SecureTransportOptions clientOptions;
            clientOptions.role = SecureRole::Client;
            clientOptions.protocolId = kProtocol;
            clientOptions.joinToken = clientToken;
            clientOptions.clockMicros = [this] { return clock; };
            client = std::make_unique<SecureTransport>(tap, std::move(clientOptions));
        }

        /// Moves time (both the network's and the transports' clock) and polls both sides
        void Step(std::uint64_t micros = 1'000)
        {
            clock += micros;
            net.Advance(micros);
            PollInto(*server, serverEvents);
            PollInto(*client, clientEvents);
        }

        bool RunUntil(const std::function<bool()> &done, int maxSteps = 2'000, std::uint64_t micros = 1'000)
        {
            for (int i = 0; i < maxSteps; ++i)
            {
                if (done())
                {
                    return true;
                }
                Step(micros);
            }
            return done();
        }

        bool Handshake()
        {
            if (!client->Connect(serverEnd.Id()))
            {
                return false;
            }
            return RunUntil(
                [this]
                {
                    return client->ClientState() == SecureClientState::Connected &&
                           CountOf(serverEvents, TransportEventType::PeerConnected) == 1;
                });
        }

        /// The client's sealed datagrams so far (type byte 0x10-0x12), in send order
        std::vector<std::vector<std::uint8_t>> SealedFromClient(std::uint8_t type) const
        {
            std::vector<std::vector<std::uint8_t>> result;
            for (const auto &datagram : tap.sent)
            {
                if (!datagram.empty() && datagram[0] == type)
                {
                    result.push_back(datagram);
                }
            }
            return result;
        }
    };

    std::uint64_t SequenceOf(const std::vector<std::uint8_t> &sealed)
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i)
        {
            value |= static_cast<std::uint64_t>(sealed[1 + i]) << (8 * i);
        }
        return value;
    }

    /// A well-formed ConnectRequest with a made-up key and no valid proof (the server can only tell at the echo)
    std::vector<std::uint8_t> RawRequest(std::uint8_t keyFill, std::uint32_t protocolId = kProtocol)
    {
        std::vector<std::uint8_t> packet(Wire::kHandshakeRequestSize, 0);
        packet[0] = Wire::kConnectRequest;
        packet[Wire::kOffsetVersion] = Wire::kVersion;
        for (std::size_t i = 0; i < 4; ++i)
        {
            packet[Wire::kOffsetProtocolId + i] = static_cast<std::uint8_t>(protocolId >> (8 * i));
        }
        std::fill_n(packet.begin() + Wire::kOffsetClientPublic, Wire::kKeySize, keyFill);
        std::fill_n(packet.begin() + Wire::kOffsetRequestProof, Wire::kMacSize, static_cast<std::uint8_t>(keyFill ^ 0x5A));
        return packet;
    }

    /// The echo of `request` with the cookie and time from `challenge`
    std::vector<std::uint8_t> RawResponse(const std::vector<std::uint8_t> &request, const std::vector<std::uint8_t> &challenge)
    {
        std::vector<std::uint8_t> packet = request;
        packet[0] = Wire::kConnectResponse;
        std::copy_n(challenge.begin() + 1, 8, packet.begin() + Wire::kOffsetCookieTime);
        std::copy_n(challenge.begin() + 9, Wire::kMacSize, packet.begin() + Wire::kOffsetCookie);
        return packet;
    }

    /// An endpoint of the pair's network that speaks raw bytes to the server
    struct Raw
    {
        SimTransport &end;
        std::vector<Event> events;

        explicit Raw(SecurePair &pair) : end(pair.net.AddEndpoint())
        {
            pair.net.Link(pair.serverEnd.Id(), end.Id(), SimLinkConfig{});
        }

        void Send(SecurePair &pair, const std::vector<std::uint8_t> &bytes) { end.Send(pair.serverEnd.Id(), bytes); }

        std::vector<std::vector<std::uint8_t>> Datagrams()
        {
            PollInto(end, events);
            std::vector<std::vector<std::uint8_t>> result;
            for (const Event &event : events)
            {
                if (event.type == TransportEventType::Datagram)
                {
                    result.push_back(event.data);
                }
            }
            return result;
        }
    };
} // namespace

// ==================== Crypto primitives: known-answer vectors ====================

TEST(N2SecurityCryptoTest, KnownAnswerVectors)
{
    ASSERT_TRUE(N2Engine::Security::CryptoAvailable());
    namespace S = N2Engine::Security;

    // SHA-256 (FIPS 180-2 "abc")
    S::Sha256Digest digest{};
    ASSERT_TRUE(S::Sha256(AsBytes("abc"), digest));
    EXPECT_EQ(Hex(digest), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // HMAC-SHA-256 (RFC 4231 test case 2)
    S::Sha256Digest mac{};
    ASSERT_TRUE(S::HmacSha256(AsBytes("Jefe"), AsBytes("what do ya want for nothing?"), mac));
    EXPECT_EQ(Hex(mac), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // HKDF-SHA-256 (RFC 5869 test case 1)
    const std::vector<std::uint8_t> ikm(22, 0x0B);
    const std::vector<std::uint8_t> salt = FromHex("000102030405060708090a0b0c");
    const std::vector<std::uint8_t> info = FromHex("f0f1f2f3f4f5f6f7f8f9");
    std::vector<std::uint8_t> okm(42, 0);
    ASSERT_TRUE(S::HkdfSha256(salt, ikm, info, okm));
    EXPECT_EQ(Hex(okm), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    // X25519 (RFC 7748 section 6.1): Alice's scalar, clamped as X25519 does, with Bob's public key
    std::vector<std::uint8_t> alice = FromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    alice[0] &= 248;
    alice[31] &= 127;
    alice[31] |= 64;
    S::X25519Key alicePrivate{};
    std::copy(alice.begin(), alice.end(), alicePrivate.begin());
    S::X25519Key bobPublic{};
    const std::vector<std::uint8_t> bob = FromHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    std::copy(bob.begin(), bob.end(), bobPublic.begin());
    S::X25519Key shared{};
    ASSERT_TRUE(S::X25519SharedSecret(alicePrivate, bobPublic, shared));
    EXPECT_EQ(Hex(shared), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

    // A low-order public key (0) gives the all-zero secret, which is refused
    S::X25519Key zero{};
    S::X25519Key refused{};
    EXPECT_FALSE(S::X25519SharedSecret(alicePrivate, zero, refused));

    // Two fresh key pairs agree
    S::X25519Key aPriv{};
    S::X25519Key aPub{};
    S::X25519Key bPriv{};
    S::X25519Key bPub{};
    ASSERT_TRUE(S::X25519GenerateKeyPair(aPriv, aPub));
    ASSERT_TRUE(S::X25519GenerateKeyPair(bPriv, bPub));
    S::X25519Key ab{};
    S::X25519Key ba{};
    ASSERT_TRUE(S::X25519SharedSecret(aPriv, bPub, ab));
    ASSERT_TRUE(S::X25519SharedSecret(bPriv, aPub, ba));
    EXPECT_EQ(ab, ba);
    EXPECT_NE(aPub, bPub);

    // ChaCha20-Poly1305 (RFC 8439 section 2.8.2)
    S::ChaCha20Poly1305 aead;
    S::ChaCha20Poly1305::Key key{};
    for (std::size_t i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::uint8_t>(0x80 + i);
    }
    ASSERT_TRUE(aead.SetKey(key));
    const std::vector<std::uint8_t> nonceBytes = FromHex("070000004041424344454647");
    S::ChaCha20Poly1305::Nonce nonce{};
    std::copy(nonceBytes.begin(), nonceBytes.end(), nonce.begin());
    const std::vector<std::uint8_t> aad = FromHex("50515253c0c1c2c3c4c5c6c7");
    const std::string_view plaintext = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for "
                                       "the future, sunscreen would be it.";
    std::vector<std::uint8_t> sealed(plaintext.size() + S::ChaCha20Poly1305::kTagSize, 0);
    ASSERT_TRUE(aead.Seal(nonce, aad, AsBytes(plaintext), sealed));
    EXPECT_EQ(Hex(std::span<const std::uint8_t>(sealed).subspan(plaintext.size())), "1ae10b594f09e26a7e902ecbd0600691");
    EXPECT_EQ(Hex(std::span<const std::uint8_t>(sealed).first(16)), "d31a8d34648e60db7b86afbc53ef7ec2");
    std::vector<std::uint8_t> opened(plaintext.size(), 0);
    ASSERT_TRUE(aead.Open(nonce, aad, sealed, opened));
    EXPECT_EQ(std::string(opened.begin(), opened.end()), plaintext);
    sealed[5] ^= 0x01;
    EXPECT_FALSE(aead.Open(nonce, aad, sealed, opened)) << "a flipped ciphertext bit fails the tag";

    // Constant-time comparison: equal, unequal, different lengths
    EXPECT_TRUE(S::ConstantTimeEqual(AsBytes("abcd"), AsBytes("abcd")));
    EXPECT_FALSE(S::ConstantTimeEqual(AsBytes("abcd"), AsBytes("abce")));
    EXPECT_FALSE(S::ConstantTimeEqual(AsBytes("abcd"), AsBytes("abc")));

    // The generator: different bytes each time, and more than one request's worth
    std::vector<std::uint8_t> first(32, 0);
    std::vector<std::uint8_t> second(32, 0);
    ASSERT_TRUE(S::RandomBytes(first));
    ASSERT_TRUE(S::RandomBytes(second));
    EXPECT_NE(first, second);
    std::vector<std::uint8_t> many(3000, 0);
    ASSERT_TRUE(S::RandomBytes(many));
    EXPECT_NE(std::count(many.begin() + 2000, many.end(), 0), 1000);
}

// ==================== Join tokens ====================

TEST(N2NetJoinTokensTest, RolesHashesAndTheFileFormat)
{
    EXPECT_EQ(JoinRoleName(JoinRole::Player), "player");
    EXPECT_EQ(JoinRoleName(JoinRole::Spectator), "spectator");
    EXPECT_EQ(JoinRoleName(JoinRole::Admin), "admin");
    EXPECT_EQ(ParseJoinRole("spectator"), JoinRole::Spectator);
    EXPECT_FALSE(ParseJoinRole("Player").has_value());
    EXPECT_FALSE(ParseJoinRole("editor").has_value());

    const auto abc = HashJoinToken("abc");
    ASSERT_TRUE(abc.has_value());
    EXPECT_EQ(N2Engine::Security::FormatTokenHash(*abc),
              "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::string hashA = N2Engine::Security::FormatTokenHash(*HashJoinToken("token-a"));
    const std::string hashB = N2Engine::Security::FormatTokenHash(*HashJoinToken("token-b"));
    const std::string hashC = N2Engine::Security::FormatTokenHash(*HashJoinToken("token-c"));
    const auto table = JoinTokenTable::Parse("[{\"user\":\"p1\",\"role\":\"player\",\"hash\":\"" + hashA +
                                             "\"},{\"user\":\"s1\",\"role\":\"spectator\",\"hash\":\"" + hashB +
                                             "\"},{\"user\":\"root\",\"role\":\"admin\",\"hash\":\"" + hashC + "\"}]");
    ASSERT_TRUE(table.has_value()) << table.error();
    ASSERT_EQ(table->Size(), 3u);
    EXPECT_EQ(table->Entries()[0].role, JoinRole::Player);
    EXPECT_EQ(table->Entries()[1].role, JoinRole::Spectator);
    EXPECT_EQ(table->Entries()[2].role, JoinRole::Admin);
    const auto match = table->Authenticate(*HashJoinToken("token-b"), std::nullopt);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->user, "s1");
    EXPECT_FALSE(table->Authenticate(*HashJoinToken("token-d"), std::nullopt).has_value());

    // An editor role is not a game role; the error names the allowed ones and never repeats the hash
    const auto wrongRole = JoinTokenTable::Parse("[{\"user\":\"e\",\"role\":\"editor\",\"hash\":\"" + hashA + "\"}]");
    ASSERT_FALSE(wrongRole.has_value());
    EXPECT_NE(wrongRole.error().find("admin, player or spectator"), std::string::npos) << wrongRole.error();
    EXPECT_EQ(wrongRole.error().find(hashA.substr(7)), std::string::npos) << wrongRole.error();
    const auto badHash = JoinTokenTable::Parse("[{\"user\":\"e\",\"role\":\"player\",\"hash\":\"sha256:00\"}]");
    ASSERT_FALSE(badHash.has_value());
    EXPECT_NE(badHash.error().find("SHA-256 of the token"), std::string::npos) << badHash.error();
}

// ==================== ReplayWindow ====================

TEST(N2NetReplayWindowTest, FreshReplayAndTooOld)
{
    ReplayWindow window;
    EXPECT_EQ(window.Check(0), ReplayWindow::Verdict::Fresh);
    window.Mark(0);
    EXPECT_EQ(window.Check(0), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Check(1), ReplayWindow::Verdict::Fresh);

    // Out of order inside the window: each once
    window.Mark(10);
    EXPECT_EQ(window.Check(5), ReplayWindow::Verdict::Fresh);
    window.Mark(5);
    EXPECT_EQ(window.Check(5), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Check(10), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Check(0), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Check(9), ReplayWindow::Verdict::Fresh);

    // Shifts across the 64-bit words keep what was seen
    window.Mark(100);
    EXPECT_EQ(window.Check(10), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Check(5), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Check(6), ReplayWindow::Verdict::Fresh);
    window.Mark(250);
    EXPECT_EQ(window.Check(5), ReplayWindow::Verdict::Replay) << "245 back: still in the 256 window";
    EXPECT_EQ(window.Check(100), ReplayWindow::Verdict::Replay);
    window.Mark(262);
    EXPECT_EQ(window.Check(6), ReplayWindow::Verdict::TooOld) << "256 back";
    EXPECT_EQ(window.Check(7), ReplayWindow::Verdict::Fresh) << "255 back, never seen";
    EXPECT_EQ(window.Check(10), ReplayWindow::Verdict::Replay);

    // A jump past the whole window forgets it all (everything behind is too old or fresh)
    window.Mark(100'000);
    EXPECT_EQ(window.Check(262), ReplayWindow::Verdict::TooOld);
    EXPECT_EQ(window.Check(99'999), ReplayWindow::Verdict::Fresh);
    EXPECT_EQ(window.Check(100'000), ReplayWindow::Verdict::Replay);
    EXPECT_EQ(window.Newest(), 100'000u);
}

// ==================== The handshake ====================

TEST(N2NetSecureTransportTest, HandshakeSucceedsAndCarriesSealedDatagramsBothWays)
{
    SecurePair pair(kToken, "spectator");
    ASSERT_TRUE(pair.server->Healthy());
    ASSERT_TRUE(pair.client->Healthy());
    EXPECT_TRUE(pair.client->Options().joinToken.empty()) << "the transport keeps the token's hash, not its text";
    EXPECT_EQ(pair.client->MaxPacketSize(), kMaxPacketSize - Wire::kSealedOverhead);

    ASSERT_TRUE(pair.Handshake());
    EXPECT_EQ(pair.client->GrantedRole(), JoinRole::Spectator);
    EXPECT_EQ(CountOf(pair.clientEvents, TransportEventType::PeerConnected), 1u);
    const PeerId clientOnServer = pair.clientEnd.Id();
    SecurePeerIdentity identity;
    ASSERT_TRUE(pair.server->GetPeerIdentity(clientOnServer, identity));
    EXPECT_EQ(identity.user, "alice");
    EXPECT_EQ(identity.role, JoinRole::Spectator);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u) << "the slot goes back once the client's first sealed packet verified";
    PeerEndpoint endpoint;
    EXPECT_TRUE(pair.server->GetPeerEndpoint(clientOnServer, endpoint));

    const std::vector<std::uint8_t> hello = MakeMessage(7, 600);
    ASSERT_EQ(pair.client->Send(pair.serverEnd.Id(), hello), TransportSendResult::Sent);
    const std::vector<std::uint8_t> reply = MakeMessage(8, pair.server->MaxPacketSize());
    ASSERT_EQ(pair.server->Send(clientOnServer, reply), TransportSendResult::Sent);
    pair.Step();
    pair.Step();
    std::vector<std::vector<std::uint8_t>> atServer;
    for (const Event &event : pair.serverEvents)
    {
        if (event.type == TransportEventType::Datagram)
        {
            atServer.push_back(event.data);
        }
    }
    ASSERT_EQ(atServer.size(), 1u);
    EXPECT_EQ(atServer[0], hello);
    std::vector<std::vector<std::uint8_t>> atClient;
    for (const Event &event : pair.clientEvents)
    {
        if (event.type == TransportEventType::Datagram)
        {
            atClient.push_back(event.data);
        }
    }
    ASSERT_EQ(atClient.size(), 1u);
    EXPECT_EQ(atClient[0], reply);

    // The plaintext never appears on the wire
    for (const auto &datagram : pair.tap.sent)
    {
        EXPECT_EQ(std::search(datagram.begin(), datagram.end(), hello.begin(), hello.begin() + 32), datagram.end());
    }
    // Too large, empty, and not a session
    EXPECT_EQ(pair.client->Send(pair.serverEnd.Id(), MakeMessage(1, pair.client->MaxPacketSize() + 1)),
              TransportSendResult::PacketTooLarge);
    EXPECT_EQ(pair.client->Send(pair.serverEnd.Id(), {}), TransportSendResult::EmptyPacket);
    EXPECT_EQ(pair.server->Send(999, hello), TransportSendResult::UnknownPeer);

    // Keep-alives hold an idle session open; a Disconnect reaches the other side as PeerDisconnected
    ASSERT_TRUE(pair.RunUntil([] { return false; }, 300, 50'000) == false);
    EXPECT_EQ(pair.client->ClientState(), SecureClientState::Connected) << "15 s of keep-alives";
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
    pair.client->Disconnect(pair.serverEnd.Id());
    EXPECT_EQ(pair.client->ClientState(), SecureClientState::Idle);
    pair.Step();
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::PeerDisconnected), 1u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 0u);
}

TEST(N2NetSecureTransportTest, AWrongTokenIsRefusedSilentlyAndTheClientTimesOut)
{
    SecurePair pair("not-the-token");
    ASSERT_TRUE(pair.client->Connect(pair.serverEnd.Id()));
    ASSERT_TRUE(pair.RunUntil([&] { return pair.client->ClientState() == SecureClientState::Failed; }, 1'000, 10'000));
    EXPECT_EQ(pair.client->ClientFailure(), SecureClientFailure::HandshakeTimeout);
    const SecureTransportStats &stats = pair.server->Stats();
    EXPECT_GE(stats.droppedBadToken, 1u);
    EXPECT_EQ(stats.acceptsSent, 0u);
    EXPECT_EQ(stats.sessionsEstablished, 0u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u) << "the slot taken for the token check went back";
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::PeerConnected), 0u);
    EXPECT_EQ(CountOf(pair.clientEvents, TransportEventType::PeerConnected), 0u);
    // Repeated failures back the address off (the editor host's RateLimiter): later responses are not even checked
    EXPECT_GE(stats.droppedBackedOff, 1u);

    // A server with no table admits nobody either
    SecurePair empty(kToken, "player", [](SecureTransportOptions &options) { options.tokens.reset(); });
    ASSERT_TRUE(empty.client->Connect(empty.serverEnd.Id()));
    empty.RunUntil([&] { return empty.client->ClientState() == SecureClientState::Failed; }, 1'000, 10'000);
    EXPECT_EQ(empty.client->ClientFailure(), SecureClientFailure::HandshakeTimeout);
    EXPECT_GE(empty.server->Stats().droppedBadToken, 1u);
}

TEST(N2NetSecureTransportTest, NoSlotOrSessionExistsBeforeTheCookieEcho)
{
    SecurePair pair;
    ASSERT_TRUE(pair.client->Connect(pair.serverEnd.Id()));

    // ConnectRequest -> Challenge: the server answers and keeps nothing
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().challengesSent, 1u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 0u);

    // A flood of requests from another address: every one answered, still nothing held
    Raw attacker(pair);
    for (std::uint8_t i = 0; i < 8; ++i)
    {
        attacker.Send(pair, RawRequest(static_cast<std::uint8_t>(i + 1)));
    }
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(attacker.Datagrams().size(), 8u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);

    // Challenge -> ConnectResponse -> Accept: the slot is taken only now
    PollInto(*pair.client, pair.clientEvents);
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().acceptsSent, 1u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 1u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 1u);
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::PeerConnected), 0u) << "not until the client proves its keys";

    // Accept -> the client's first sealed packet: established, the slot given back
    PollInto(*pair.client, pair.clientEvents);
    EXPECT_EQ(pair.client->ClientState(), SecureClientState::Connected);
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::PeerConnected), 1u);
}

TEST(N2NetSecureTransportTest, APendingSessionThatNeverAnswersGivesItsSlotBack)
{
    SecurePair pair;
    ASSERT_TRUE(pair.client->Connect(pair.serverEnd.Id()));
    PollInto(*pair.server, pair.serverEvents); // Challenge
    PollInto(*pair.client, pair.clientEvents); // ConnectResponse
    PollInto(*pair.server, pair.serverEvents); // Accept, slot taken
    ASSERT_EQ(pair.server->PreAuthSlotsInUse(), 1u);
    // The client vanishes: the Accept is never answered
    pair.net.Unlink(pair.serverEnd.Id(), pair.clientEnd.Id());
    for (int i = 0; i < 60; ++i)
    {
        pair.clock += 100'000;
        pair.net.Advance(100'000);
        PollInto(*pair.server, pair.serverEvents);
    }
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);
    EXPECT_EQ(pair.server->Stats().handshakeTimeouts, 1u);
}

TEST(N2NetSecureTransportTest, AStallLongerThanTheIdleTimeoutKeepsASessionWhosePacketsAreQueued)
{
    // Poll drains the inner transport before it sweeps idle sessions: a caller that stalls between Polls for longer
    // than idleTimeoutMicros keeps a session whose authenticated packet is queued, and still loses one that sent nothing
    SecurePair pair;
    ASSERT_TRUE(pair.Handshake());
    ASSERT_EQ(pair.server->EstablishedSessionCount(), 1u);
    const std::uint64_t idle = pair.server->Options().idleTimeoutMicros;
    const std::uint64_t stall = 3 * idle;
    pair.serverEvents.clear();

    // The stall: the client sends one datagram, then neither side is polled for three idle timeouts
    const std::vector<std::uint8_t> message = MakeMessage(5, 200);
    ASSERT_EQ(pair.client->Send(pair.serverEnd.Id(), message), TransportSendResult::Sent);
    pair.clock += stall;
    pair.net.Advance(stall);

    // One Poll: the datagram is delivered and the session is not ended
    PollInto(*pair.server, pair.serverEvents);
    ASSERT_EQ(CountOf(pair.serverEvents, TransportEventType::Datagram), 1u);
    EXPECT_EQ(pair.serverEvents[0].data, message);
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::PeerDisconnected), 0u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
    EXPECT_EQ(pair.server->Stats().idleTimeouts, 0u);

    // The client sends nothing more and is not polled, so no keep-alive comes: after one more idle timeout the
    // server ends the session
    pair.clock += idle + 1'000;
    pair.net.Advance(idle + 1'000);
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::PeerDisconnected), 1u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 0u);
    EXPECT_EQ(pair.server->Stats().idleTimeouts, 1u);
}

TEST(N2NetSecureTransportTest, ForgedExpiredAndBorrowedCookiesAreRefused)
{
    SecurePair pair;
    Raw a(pair);
    Raw b(pair);

    // A forged cookie (never made by the server)
    const std::vector<std::uint8_t> request = RawRequest(0x11);
    std::vector<std::uint8_t> forgedChallenge(Wire::kChallengeSize, 0x42);
    forgedChallenge[0] = Wire::kChallenge;
    a.Send(pair, RawResponse(request, forgedChallenge));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie, 1u);

    // A real cookie for A, echoed by B (another address): refused
    a.Send(pair, request);
    PollInto(*pair.server, pair.serverEvents);
    const auto challenges = a.Datagrams();
    ASSERT_EQ(challenges.size(), 1u);
    ASSERT_EQ(challenges[0].size(), Wire::kChallengeSize);
    b.Send(pair, RawResponse(request, challenges[0]));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie, 2u);

    // A's cookie with other request fields (another key): refused
    a.Send(pair, RawResponse(RawRequest(0x12), challenges[0]));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie, 3u);

    // The cookie with a changed time (the MAC covers it): refused
    std::vector<std::uint8_t> shifted = RawResponse(request, challenges[0]);
    shifted[Wire::kOffsetCookieTime] ^= 0x01;
    a.Send(pair, shifted);
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie, 4u);

    // A's own echo after the cookie's lifetime (5 s): expired
    pair.clock += 6'000'000;
    pair.net.Advance(6'000'000);
    a.Send(pair, RawResponse(request, challenges[0]));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedExpiredCookie, 1u);

    // None of these reached the token check or took a slot
    EXPECT_EQ(pair.server->Stats().droppedBadToken, 0u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);
    EXPECT_EQ(pair.server->PendingSessionCount(), 0u);

    // A fresh, valid cookie with a proof that matches no token: the token check refuses it and the slot goes back
    a.Send(pair, request);
    PollInto(*pair.server, pair.serverEvents);
    const auto fresh = a.Datagrams();
    ASSERT_EQ(fresh.size(), 2u);
    a.Send(pair, RawResponse(request, fresh[1]));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadToken, 1u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);
    EXPECT_EQ(a.Datagrams().size(), 2u) << "no reply to a wrong token";
}

TEST(N2NetSecureTransportTest, ACookieMadeWithTheRotatedOutSecretStillVerifiesUntilTheNextRotation)
{
    SecurePair pair(kToken, "player", [](SecureTransportOptions &options)
                    {
                        options.cookieLifetimeMicros = 5'000'000;
                        options.cookieSecretRotationMicros = 10'000'000;
                    });
    Raw a(pair);
    const std::vector<std::uint8_t> request = RawRequest(0x21);
    // Move to 1 s before a rotation boundary (epochs of 10 s), get a cookie there
    const std::uint64_t boundary = (pair.clock / 10'000'000 + 1) * 10'000'000;
    pair.clock = boundary - 1'000'000;
    a.Send(pair, request);
    PollInto(*pair.server, pair.serverEvents);
    const auto challenges = a.Datagrams();
    ASSERT_EQ(challenges.size(), 1u);
    // 2 s later the secret has rotated; the cookie is 2 s old and made with the previous secret: the token check runs
    pair.clock += 2'000'000;
    a.Send(pair, RawResponse(request, challenges[0]));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie, 0u);
    EXPECT_EQ(pair.server->Stats().droppedBadToken, 1u);
    // Two rotations later the old secret is gone
    pair.clock += 20'000'000;
    a.Send(pair, RawResponse(request, challenges[0]));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie + pair.server->Stats().droppedBackedOff, 1u);
}

TEST(N2NetSecureTransportTest, NoPreAuthReplyIsEverLargerThanTheRequestItAnswers)
{
    SecurePair pair;
    Raw attacker(pair);
    std::size_t requestBytes = 0;
    std::size_t requests = 0;
    // Requests, echoes with good and bad cookies, malformed and short packets of every handshake type
    for (std::uint8_t i = 0; i < 6; ++i)
    {
        const std::vector<std::uint8_t> request = RawRequest(static_cast<std::uint8_t>(0x30 + i));
        attacker.Send(pair, request);
        requestBytes += request.size();
        ++requests;
    }
    PollInto(*pair.server, pair.serverEvents);
    const auto challenges = attacker.Datagrams();
    ASSERT_EQ(challenges.size(), 6u);
    for (std::uint8_t i = 0; i < 4; ++i)
    {
        const std::vector<std::uint8_t> response = RawResponse(RawRequest(static_cast<std::uint8_t>(0x30 + i)), challenges[i]);
        attacker.Send(pair, response);
        requestBytes += response.size();
        ++requests;
    }
    const std::vector<std::vector<std::uint8_t>> junk = {
        std::vector<std::uint8_t>(70, Wire::kConnectRequest), std::vector<std::uint8_t>(1, Wire::kConnectRequest),
        std::vector<std::uint8_t>(255, Wire::kConnectResponse), std::vector<std::uint8_t>(1200, Wire::kConnectRequest),
        RawRequest(0x40, kProtocol + 1), std::vector<std::uint8_t>(41, Wire::kChallenge),
        std::vector<std::uint8_t>(82, Wire::kAccept), std::vector<std::uint8_t>(40, Wire::kData)};
    for (const auto &packet : junk)
    {
        attacker.Send(pair, packet);
        requestBytes += packet.size();
        ++requests;
    }
    pair.clock += 1'000;
    pair.net.Advance(1'000);
    PollInto(*pair.server, pair.serverEvents);

    const auto replies = attacker.Datagrams();
    std::size_t replyBytes = 0;
    for (const auto &reply : replies)
    {
        EXPECT_LE(reply.size(), Wire::kHandshakeRequestSize);
        replyBytes += reply.size();
    }
    EXPECT_LE(replies.size(), requests);
    EXPECT_LE(replyBytes, requestBytes);
    EXPECT_EQ(replies.size(), 6u) << "only the six full-size requests were answered (the echoes had no valid token)";

    // And for a complete, successful handshake
    ASSERT_TRUE(pair.Handshake());
    const SecureTransportStats &stats = pair.server->Stats();
    EXPECT_GT(stats.largestPreAuthReply, 0u);
    EXPECT_LE(stats.largestPreAuthReply, stats.smallestPreAuthRequestAnswered);
    EXPECT_LE(stats.preAuthBytesSent, stats.preAuthBytesReceived);
    EXPECT_EQ(stats.smallestPreAuthRequestAnswered, Wire::kHandshakeRequestSize);
    EXPECT_EQ(stats.largestPreAuthReply, Wire::kAcceptSize);
}

TEST(N2NetSecureTransportTest, ALostAcceptIsSentAgainByteForByte)
{
    SecurePair pair;
    ASSERT_TRUE(pair.client->Connect(pair.serverEnd.Id()));
    PollInto(*pair.server, pair.serverEvents); // Challenge
    PollInto(*pair.client, pair.clientEvents); // ConnectResponse
    // The Accept is lost: the link drops everything from the server for a moment
    SimLinkConfig lossy;
    lossy.lossPpm = 1'000'000;
    pair.net.SetLinkConfig(pair.serverEnd.Id(), pair.clientEnd.Id(), lossy);
    PollInto(*pair.server, pair.serverEvents); // Accept (lost)
    pair.net.SetLinkConfig(pair.serverEnd.Id(), pair.clientEnd.Id(), SimLinkConfig{});
    ASSERT_TRUE(pair.RunUntil([&] { return pair.client->ClientState() == SecureClientState::Connected; }, 200, 10'000));
    EXPECT_GE(pair.server->Stats().acceptsResent, 1u);
    EXPECT_EQ(pair.server->Stats().acceptsSent, 1u) << "one key exchange, the same Accept again";
    pair.Step();
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
}

TEST(N2NetSecureTransportTest, AHandshakeCompletesWhenTheRoundTripExceedsTheResendInterval)
{
    SecurePair pair;
    // A 300 ms round trip, over the 250 ms resend interval: the ConnectRequest goes out twice and two Challenges (two
    // different cookies) come back; the client must keep the cookie it echoed first, which the server's Accept is for
    SimLinkConfig slow;
    slow.latencyMicros = 150'000;
    pair.net.SetLinkConfig(pair.serverEnd.Id(), pair.clientEnd.Id(), slow);
    pair.net.SetLinkConfig(pair.clientEnd.Id(), pair.serverEnd.Id(), slow);
    ASSERT_TRUE(pair.client->Connect(pair.serverEnd.Id()));
    ASSERT_TRUE(pair.RunUntil(
        [&]
        {
            return pair.client->ClientState() == SecureClientState::Connected &&
                   CountOf(pair.serverEvents, TransportEventType::PeerConnected) == 1;
        },
        4'000));
    EXPECT_GE(pair.server->Stats().challengesSent, 2u) << "the request was sent again before the first Challenge came back";
    EXPECT_EQ(pair.server->Stats().acceptsSent, 1u);
    EXPECT_EQ(pair.client->Stats().droppedBadAccept, 0u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
}

// ==================== Sealed packets ====================

TEST(N2NetSecureTransportTest, AReplayedSealedPacketIsDropped)
{
    SecurePair pair;
    ASSERT_TRUE(pair.Handshake());
    const std::vector<std::uint8_t> message = MakeMessage(3, 100);
    ASSERT_EQ(pair.client->Send(pair.serverEnd.Id(), message), TransportSendResult::Sent);
    pair.Step();
    ASSERT_EQ(CountOf(pair.serverEvents, TransportEventType::Datagram), 1u);
    const auto data = pair.SealedFromClient(Wire::kData);
    ASSERT_EQ(data.size(), 1u);

    // The same bytes again, twice, and an old keep-alive: all dropped, nothing delivered
    pair.tap.Inject(pair.serverEnd.Id(), data[0]);
    pair.tap.Inject(pair.serverEnd.Id(), data[0]);
    pair.tap.Inject(pair.serverEnd.Id(), pair.SealedFromClient(Wire::kKeepAlive).at(0));
    pair.Step();
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::Datagram), 1u);
    EXPECT_EQ(pair.server->Stats().droppedReplay, 3u);

    // Long after, behind the window: still dropped (as too old)
    for (int i = 0; i < 300; ++i)
    {
        ASSERT_EQ(pair.client->Send(pair.serverEnd.Id(), message), TransportSendResult::Sent);
    }
    pair.Step();
    pair.tap.Inject(pair.serverEnd.Id(), data[0]);
    pair.Step();
    EXPECT_EQ(pair.server->Stats().droppedTooOld, 1u);
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::Datagram), 301u);
}

TEST(N2NetSecureTransportTest, ATamperedPacketIsDroppedSilently)
{
    SecurePair pair;
    ASSERT_TRUE(pair.Handshake());
    // The original is captured on its way out and lost, so the altered copies below arrive with a fresh sequence (a
    // copy of a packet already delivered would be dropped earlier, as a replay)
    SimLinkConfig lossy;
    lossy.lossPpm = 1'000'000;
    pair.net.SetLinkConfig(pair.clientEnd.Id(), pair.serverEnd.Id(), lossy);
    ASSERT_EQ(pair.client->Send(pair.serverEnd.Id(), MakeMessage(4, 64)), TransportSendResult::Sent);
    pair.net.SetLinkConfig(pair.clientEnd.Id(), pair.serverEnd.Id(), SimLinkConfig{});
    const std::vector<std::uint8_t> original = pair.SealedFromClient(Wire::kData).at(0);
    // Altered copies of it, each changing one thing, then the original itself
    std::vector<std::vector<std::uint8_t>> altered;
    for (const std::size_t offset : {std::size_t{9}, std::size_t{40}, original.size() - 1})
    {
        std::vector<std::uint8_t> copy = original;
        copy[offset] ^= 0x80; // the ciphertext, and the tag
        altered.push_back(copy);
    }
    std::vector<std::uint8_t> retyped = original;
    retyped[0] = Wire::kKeepAlive; // the type is in the associated data
    altered.push_back(retyped);
    std::vector<std::uint8_t> renumbered = original;
    renumbered[1] ^= 0x04; // the sequence is the nonce and in the associated data
    altered.push_back(renumbered);
    std::vector<std::uint8_t> truncated(original.begin(), original.end() - 1);
    altered.push_back(truncated);
    for (const auto &packet : altered)
    {
        pair.tap.Inject(pair.serverEnd.Id(), packet);
    }
    pair.tap.Inject(pair.serverEnd.Id(), original);
    pair.Step();
    EXPECT_EQ(pair.server->Stats().droppedAuthFailed, altered.size());
    EXPECT_EQ(CountOf(pair.serverEvents, TransportEventType::Datagram), 1u) << "only the original";
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u) << "a forgery does not end the session";

    // A sealed packet from a peer with no session
    Raw stranger(pair);
    stranger.Send(pair, original);
    pair.Step();
    EXPECT_EQ(pair.server->Stats().droppedNoSession, 1u);
}

TEST(N2NetSecureTransportTest, EverySealedPacketUsesANewSequence)
{
    SecurePair pair;
    ASSERT_TRUE(pair.Handshake());
    for (int i = 0; i < 50; ++i)
    {
        ASSERT_EQ(pair.client->Send(pair.serverEnd.Id(), MakeMessage(static_cast<std::uint32_t>(i), 32)),
                  TransportSendResult::Sent);
        if (i % 10 == 0)
        {
            pair.Step(1'200'000); // keep-alives in between
        }
    }
    std::vector<std::uint64_t> sequences;
    for (const auto &datagram : pair.tap.sent)
    {
        if (!datagram.empty() && datagram[0] >= Wire::kData && datagram[0] <= Wire::kDisconnect)
        {
            sequences.push_back(SequenceOf(datagram));
        }
    }
    ASSERT_GT(sequences.size(), 50u);
    for (std::size_t i = 0; i < sequences.size(); ++i)
    {
        EXPECT_EQ(sequences[i], i) << "the client's sequence starts at 0 and rises by one per sealed packet";
    }
}

TEST(N2NetSecureTransportTest, ConnectFloodsAreRateLimited)
{
    SecurePair pair;
    Raw attacker(pair);
    for (std::uint8_t i = 0; i < 100; ++i)
    {
        attacker.Send(pair, RawRequest(static_cast<std::uint8_t>(i)));
    }
    PollInto(*pair.server, pair.serverEvents);
    const SecureTransportStats &stats = pair.server->Stats();
    EXPECT_EQ(stats.challengesSent, 10u) << "one second's burst for one address (10/s)";
    EXPECT_EQ(stats.droppedRateLimited, 90u);
    EXPECT_EQ(attacker.Datagrams().size(), 10u);

    // The legitimate client from its own address is unaffected
    ASSERT_TRUE(pair.Handshake());

    // A second later the attacker's bucket has refilled
    pair.clock += 1'000'000;
    pair.net.Advance(1'000'000);
    attacker.Send(pair, RawRequest(0xEE));
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(stats.challengesSent, 12u) << "the client's one, then the attacker's";
}

TEST(N2NetSecureTransportTest, SpoofedEchoesDoNotSpendTheConnectRate)
{
    SecurePair pair;
    // 80 echoes with forged cookies (a current time, so the cookie MAC is computed), each from its own address: more
    // than the connect rate's global burst (50). The cookie is checked before the rate is charged, so none of them
    // takes a connect token and the legitimate client still gets in.
    std::vector<std::uint8_t> forgedChallenge(Wire::kChallengeSize, 0x42);
    forgedChallenge[0] = Wire::kChallenge;
    const std::uint64_t nowMillis = pair.clock / 1000;
    for (std::size_t i = 0; i < 8; ++i)
    {
        forgedChallenge[1 + i] = static_cast<std::uint8_t>(nowMillis >> (8 * i));
    }
    std::vector<std::unique_ptr<Raw>> spoofed;
    for (std::size_t i = 0; i < 80; ++i)
    {
        spoofed.push_back(std::make_unique<Raw>(pair));
        spoofed.back()->Send(pair, RawResponse(RawRequest(static_cast<std::uint8_t>(i)), forgedChallenge));
    }
    PollInto(*pair.server, pair.serverEvents);
    EXPECT_EQ(pair.server->Stats().droppedBadCookie, 80u);
    EXPECT_EQ(pair.server->Stats().droppedRateLimited, 0u);
    EXPECT_EQ(pair.server->PreAuthSlotsInUse(), 0u);

    ASSERT_TRUE(pair.Handshake());
    EXPECT_EQ(pair.server->Stats().droppedRateLimited, 0u);
    EXPECT_EQ(pair.server->EstablishedSessionCount(), 1u);
}

// ==================== End to end over loopback UDP ====================

namespace
{
    constexpr int kUdpWaitMs = 20'000;

    struct UdpSide
    {
        SecureTransport *transport = nullptr;
        PeerId peer = kInvalidPeer;
        std::unique_ptr<Connection> connection;
        std::vector<ReceivedMessage> received;
    };

    std::uint64_t WallMicros()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    void PumpUdp(UdpSide &side)
    {
        const std::uint64_t now = WallMicros();
        side.transport->Poll(
            [&](const TransportEvent &event)
            {
                if (event.type == TransportEventType::PeerConnected)
                {
                    side.peer = event.peer;
                    return;
                }
                if (event.type != TransportEventType::Datagram || event.peer != side.peer)
                {
                    return;
                }
                side.connection->OnPacket(now, event.data,
                                          [&](MessageChannel channel, std::span<const std::uint8_t> message)
                                          {
                                              ReceivedMessage received;
                                              received.channel = channel;
                                              received.data.assign(message.begin(), message.end());
                                              side.received.push_back(std::move(received));
                                          });
            });
        if (side.peer != kInvalidPeer)
        {
            const PeerId peer = side.peer;
            SecureTransport *transport = side.transport;
            const std::uint64_t later = WallMicros();
            side.connection->Update(later, [&](std::span<const std::uint8_t> packet) { transport->Send(peer, packet); });
        }
    }
} // namespace

TEST(N2NetSecureTransportTest, ChannelsOverLoopbackUdpThroughTheSecureLayer)
{
    SecureTransportOptions serverOptions;
    serverOptions.role = SecureRole::Server;
    serverOptions.protocolId = kProtocol;
    serverOptions.tokens = TableFor(kToken, "alice", "admin");
    SecureUdpOpenResult server = SecureTransport::OpenUdp(UdpTransportOptions{}, std::move(serverOptions));
    ASSERT_NE(server.transport, nullptr) << server.message;

    UdpTransportOptions clientUdp;
    clientUdp.acceptUnknownPeers = false;
    SecureTransportOptions clientOptions;
    clientOptions.role = SecureRole::Client;
    clientOptions.protocolId = kProtocol;
    clientOptions.joinToken = kToken;
    SecureUdpOpenResult client = SecureTransport::OpenUdp(clientUdp, std::move(clientOptions));
    ASSERT_NE(client.transport, nullptr) << client.message;

    const UdpConnectResult connected = client.udp->Connect("127.0.0.1", server.udp->LocalPort());
    ASSERT_EQ(connected.error, UdpError::None) << connected.message;
    ASSERT_TRUE(client.transport->Connect(connected.peer));

    ConnectionConfig config = TestConfig();
    config.maxPacketSize = static_cast<std::uint32_t>(client.transport->MaxPacketSize());
    config.initialRtoMicros = 20'000;
    config.minRtoMicros = 10'000;
    config.keepAliveMicros = 100'000;
    config.idleTimeoutMicros = 60'000'000;
    config.reassemblyTimeoutMicros = 60'000'000;
    UdpSide serverSide;
    UdpSide clientSide;
    serverSide.transport = server.transport.get();
    clientSide.transport = client.transport.get();
    serverSide.connection = std::make_unique<Connection>(config, WallMicros());
    clientSide.connection = std::make_unique<Connection>(config, WallMicros());

    constexpr std::uint32_t kCount = 60;
    for (std::uint32_t i = 0; i < kCount; ++i)
    {
        ASSERT_EQ(clientSide.connection->SendReliable(MakeMessage(i, i == 30 ? 20'000 : 1 + (i * 97u) % 1500u)),
                  SendResult::Ok);
    }
    ASSERT_EQ(clientSide.connection->SendUnreliable(MakeMessage(9'999, clientSide.connection->MaxUnreliableMessageSize())),
              SendResult::Ok);
    bool serverQueued = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kUdpWaitMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        PumpUdp(clientSide);
        PumpUdp(serverSide);
        if (!serverQueued && serverSide.peer != kInvalidPeer)
        {
            serverQueued = true;
            for (std::uint32_t i = 0; i < 20; ++i)
            {
                ASSERT_EQ(serverSide.connection->SendReliable(MakeMessage(5'000 + i, 900)), SendResult::Ok);
            }
        }
        const auto reliableAt = [](const UdpSide &side)
        {
            return static_cast<std::size_t>(std::count_if(side.received.begin(), side.received.end(),
                                                          [](const ReceivedMessage &message)
                                                          { return message.channel == MessageChannel::ReliableOrdered; }));
        };
        if (reliableAt(serverSide) >= kCount && reliableAt(clientSide) >= 20)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::vector<std::vector<std::uint8_t>> reliable;
    for (const ReceivedMessage &message : serverSide.received)
    {
        if (message.channel == MessageChannel::ReliableOrdered)
        {
            reliable.push_back(message.data);
        }
    }
    ASSERT_EQ(reliable.size(), kCount) << "within " << kUdpWaitMs << " ms";
    for (std::uint32_t i = 0; i < kCount; ++i)
    {
        ASSERT_EQ(reliable[i], MakeMessage(i, i == 30 ? 20'000 : 1 + (i * 97u) % 1500u)) << "message " << i;
    }
    EXPECT_EQ(std::count_if(clientSide.received.begin(), clientSide.received.end(), [](const ReceivedMessage &message)
                            { return message.channel == MessageChannel::ReliableOrdered; }),
              20);
    EXPECT_EQ(client.transport->GrantedRole(), JoinRole::Admin);
    SecurePeerIdentity identity;
    EXPECT_TRUE(server.transport->GetPeerIdentity(serverSide.peer, identity));
    EXPECT_EQ(identity.user, "alice");
    EXPECT_EQ(server.transport->Stats().droppedAuthFailed, 0u);
    EXPECT_EQ(client.transport->Stats().droppedAuthFailed, 0u);
    EXPECT_FALSE(clientSide.connection->IsFailed());
    EXPECT_FALSE(serverSide.connection->IsFailed());
}

TEST(N2NetSecureTransportTest, TheSecureLayerAllowsANonLoopbackBindWithoutTheInsecureFlag)
{
    std::vector<std::string> warnings;
    UdpTransportOptions udp;
    udp.bindAddress = "0.0.0.0";
    udp.warningSink = [&warnings](std::string_view text) { warnings.emplace_back(text); };
    SecureTransportOptions options;
    options.role = SecureRole::Server;
    options.protocolId = kProtocol;
    const SecureUdpOpenResult opened = SecureTransport::OpenUdp(udp, std::move(options));
    ASSERT_NE(opened.transport, nullptr) << opened.message;
    EXPECT_FALSE(opened.udp->InsecureOverrideUsed());
    EXPECT_TRUE(warnings.empty());

    // Without the secure layer the A4 rule still refuses it
    UdpTransportOptions plain;
    plain.bindAddress = "0.0.0.0";
    EXPECT_EQ(UdpTransport::Open(plain, false).error, UdpError::NonLoopbackRefused);
}

#endif // N2NET_HAS_SECURE
