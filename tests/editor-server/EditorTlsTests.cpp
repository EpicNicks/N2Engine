// The editor host's TLS layer (--tls). Compiled to nothing unless the N2ENGINE_EDITOR_TLS option is on: the tests act
// as the TLS client, so they use Mbed TLS directly (its code is built into N2EditorServer). Everything goes over real
// loopback sockets to a real EditorServer, and the test thread (or a helper thread) plays the host's main loop.

#include <gtest/gtest.h>

#ifdef N2ENGINE_EDITOR_TLS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Logger.hpp>
#include <engine/io/ProjectFile.hpp>

#include <mbedtls/build_info.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
namespace fs = std::filesystem;

namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr const char *TestToken = "tls-test-access-token";

    const std::vector<uint8_t> NoSuchEntity = []
    {
        BufferWriter w;
        w.WriteString("00000000-0000-0000-0000-000000000000");
        return w.Release();
    }();

    std::vector<uint8_t> HelloPayload(const std::string &token)
    {
        BufferWriter w;
        w.WriteString("tls test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        return w.Release();
    }

    /// A request as the wire has it: [type: 1][declared length: 4][payload]
    std::vector<uint8_t> Frame(CommandType type, const std::vector<uint8_t> &payload, uint32_t declaredLength)
    {
        BufferWriter w;
        w.WriteU8(static_cast<uint8_t>(type));
        w.WriteU32(declaredLength);
        w.WriteBytes(payload);
        return w.Release();
    }

    std::vector<uint8_t> Frame(CommandType type, const std::vector<uint8_t> &payload = {})
    {
        return Frame(type, payload, static_cast<uint32_t>(payload.size()));
    }

    struct Response
    {
        uint8_t type = 0xEE;
        std::string body;
    };

    void EnsureCrypto()
    {
        static const bool ok = psa_crypto_init() == PSA_SUCCESS;
        ASSERT_TRUE(ok);
    }

    std::string Hex(const unsigned char *bytes, size_t count)
    {
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string text;
        for (size_t i = 0; i < count; ++i)
        {
            text += digits[bytes[i] >> 4];
            text += digits[bytes[i] & 0x0F];
        }
        return text;
    }

    std::string ReadFileText(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    void WriteFileText(const fs::path &path, const std::string &text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    /// A folder of its own under the system temp folder, removed afterwards
    class TempDirectory
    {
    public:
        TempDirectory()
        {
            std::random_device random;
            _path = fs::temp_directory_path() / ("n2-tls-test-" + std::to_string(random()) + std::to_string(random()));
            fs::create_directories(_path);
        }

        ~TempDirectory()
        {
            std::error_code ignored;
            fs::remove_all(_path, ignored);
        }

        TempDirectory(const TempDirectory &) = delete;
        TempDirectory &operator=(const TempDirectory &) = delete;

        [[nodiscard]] const fs::path &Path() const { return _path; }

    private:
        fs::path _path;
    };

    /// A plain TCP connection to the loopback server (a client that doesn't speak TLS, or hasn't started to)
    class PlainConnection
    {
    public:
        explicit PlainConnection(int port)
        {
            mbedtls_net_init(&_net);
            _connected = mbedtls_net_connect(&_net, "127.0.0.1", std::to_string(port).c_str(),
                                             MBEDTLS_NET_PROTO_TCP) == 0;
        }

        ~PlainConnection()
        {
            mbedtls_net_free(&_net);
        }

        PlainConnection(const PlainConnection &) = delete;
        PlainConnection &operator=(const PlainConnection &) = delete;

        [[nodiscard]] bool IsConnected() const { return _connected; }

        bool Send(const void *data, size_t size)
        {
            return mbedtls_net_send(&_net, static_cast<const unsigned char *>(data), size) == static_cast<int>(size);
        }

        /// Reads whatever the server sends until it closes the connection (true) or `limit` passes (false)
        bool DrainUntilClosed(std::chrono::milliseconds limit, std::vector<uint8_t> *received = nullptr)
        {
            const auto end = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < end)
            {
                unsigned char buffer[512];
                const int count = mbedtls_net_recv_timeout(&_net, buffer, sizeof(buffer), 100);
                if (count == MBEDTLS_ERR_SSL_TIMEOUT)
                {
                    continue;
                }
                if (count <= 0)
                {
                    return true; // closed, or reset
                }
                if (received != nullptr)
                {
                    received->insert(received->end(), buffer, buffer + count);
                }
            }
            return false;
        }

    private:
        mbedtls_net_context _net;
        bool _connected = false;
    };

    /// A TLS client for the loopback server: certificate checking is off (the tests pin the fingerprint themselves)
    class TlsClient
    {
    public:
        explicit TlsClient(int port, bool onlyTls12 = false)
        {
            EnsureCrypto();
            mbedtls_net_init(&_net);
            mbedtls_ssl_init(&_ssl);
            mbedtls_ssl_config_init(&_config);
            mbedtls_ctr_drbg_init(&_random);
            mbedtls_entropy_init(&_entropy);

            _connected = mbedtls_net_connect(&_net, "127.0.0.1", std::to_string(port).c_str(),
                                             MBEDTLS_NET_PROTO_TCP) == 0 &&
                         mbedtls_ctr_drbg_seed(&_random, mbedtls_entropy_func, &_entropy, nullptr, 0) == 0 &&
                         mbedtls_ssl_config_defaults(&_config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                     MBEDTLS_SSL_PRESET_DEFAULT) == 0;
            if (!_connected)
            {
                return;
            }
            mbedtls_ssl_conf_authmode(&_config, MBEDTLS_SSL_VERIFY_NONE);
            mbedtls_ssl_conf_rng(&_config, mbedtls_ctr_drbg_random, &_random);
            mbedtls_ssl_conf_read_timeout(&_config, 10000); // a hung server fails the test instead of blocking it
            if (onlyTls12)
            {
                mbedtls_ssl_conf_max_tls_version(&_config, MBEDTLS_SSL_VERSION_TLS1_2);
            }
            _connected = mbedtls_ssl_setup(&_ssl, &_config) == 0;
            mbedtls_ssl_set_bio(&_ssl, &_net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
        }

        ~TlsClient()
        {
            mbedtls_ssl_free(&_ssl);
            mbedtls_ssl_config_free(&_config);
            mbedtls_ctr_drbg_free(&_random);
            mbedtls_entropy_free(&_entropy);
            mbedtls_net_free(&_net);
        }

        TlsClient(const TlsClient &) = delete;
        TlsClient &operator=(const TlsClient &) = delete;

        [[nodiscard]] bool IsConnected() const { return _connected; }

        /// 0 when the handshake completed, otherwise the library's error
        int Handshake()
        {
            int code = 0;
            do
            {
                code = mbedtls_ssl_handshake(&_ssl);
            } while (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE);
            return code;
        }

        /// What a client that pins would compare: SHA-256 of the certificate the server presented
        [[nodiscard]] std::string PeerFingerprint() const
        {
            const mbedtls_x509_crt *certificate = mbedtls_ssl_get_peer_cert(&_ssl);
            if (certificate == nullptr)
            {
                return {};
            }
            unsigned char digest[32];
            if (mbedtls_sha256(certificate->raw.p, certificate->raw.len, digest, 0) != 0)
            {
                return {};
            }
            return Hex(digest, sizeof(digest));
        }

        [[nodiscard]] std::string Version() const { return mbedtls_ssl_get_version(&_ssl); }

        bool Send(const std::vector<uint8_t> &bytes)
        {
            size_t sent = 0;
            while (sent < bytes.size())
            {
                const int code = mbedtls_ssl_write(&_ssl, bytes.data() + sent, bytes.size() - sent);
                if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE)
                {
                    continue;
                }
                if (code <= 0)
                {
                    return false;
                }
                sent += static_cast<size_t>(code);
            }
            return true;
        }

        bool ReadExact(uint8_t *out, size_t count)
        {
            size_t received = 0;
            while (received < count)
            {
                const int code = mbedtls_ssl_read(&_ssl, out + received, count - received);
                if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    code == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                {
                    continue;
                }
                if (code <= 0)
                {
                    return false;
                }
                received += static_cast<size_t>(code);
            }
            return true;
        }

        /// The next response frame; nullopt when the connection closed or timed out first
        std::optional<Response> ReadResponse()
        {
            uint8_t header[5];
            if (!ReadExact(header, sizeof(header)))
            {
                return std::nullopt;
            }
            uint32_t size = 0;
            std::memcpy(&size, header + 1, sizeof(size));
            Response response;
            response.type = header[0];
            response.body.resize(size);
            if (size > 0 && !ReadExact(reinterpret_cast<uint8_t *>(response.body.data()), size))
            {
                return std::nullopt;
            }
            return response;
        }

        std::optional<Response> Roundtrip(const std::vector<uint8_t> &request)
        {
            if (!Send(request))
            {
                return std::nullopt;
            }
            return ReadResponse();
        }

        /// True once the server has ended the connection (a close_notify, a reset or end of file)
        bool IsClosedByServer()
        {
            uint8_t byte = 0;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                const int code = mbedtls_ssl_read(&_ssl, &byte, 1);
                if (code == MBEDTLS_ERR_SSL_WANT_READ || code == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    code == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                {
                    continue;
                }
                return code <= 0 && code != MBEDTLS_ERR_SSL_TIMEOUT;
            }
            return false;
        }

    private:
        mbedtls_net_context _net;
        mbedtls_ssl_context _ssl;
        mbedtls_ssl_config _config;
        mbedtls_ctr_drbg_context _random;
        mbedtls_entropy_context _entropy;
        bool _connected = false;
    };

    /// Plays the host's main loop (EditorServer::ProcessCommands) on its own thread, so a client can block on reads
    class MainLoop
    {
    public:
        explicit MainLoop(EditorServer &server)
            : _thread(
                  [this, &server]
                  {
                      while (!_stop)
                      {
                          server.ProcessCommands(std::chrono::milliseconds(5));
                      }
                  })
        {
        }

        ~MainLoop()
        {
            _stop = true;
            _thread.join();
        }

        MainLoop(const MainLoop &) = delete;
        MainLoop &operator=(const MainLoop &) = delete;

    private:
        std::atomic<bool> _stop{false};
        std::thread _thread;
    };

    /// A TLS server on an OS-picked port, with its identity in a temporary folder
    struct TlsServerFixture
    {
        explicit TlsServerFixture(const std::string &token = "",
                                  std::chrono::milliseconds helloTimeout = EditorServer::DefaultHelloTimeout,
                                  bool stopOnDisconnect = false)
        {
            if (!token.empty())
            {
                EXPECT_TRUE(server.SetAccessToken(token));
            }
            EXPECT_TRUE(server.SetHelloTimeout(helloTimeout));
            if (stopOnDisconnect)
            {
                EXPECT_TRUE(server.SetStopOnDisconnect(true));
            }
            const auto enabled = server.EnableTls(directory.Path());
            EXPECT_TRUE(enabled.has_value()) << (enabled ? "" : enabled.error());
            started = enabled.has_value() && server.Start(0);
            if (started)
            {
                loop = std::make_unique<MainLoop>(server);
            }
        }

        ~TlsServerFixture()
        {
            loop.reset(); // the main loop first: Stop() then closes the queue under no one
            server.Stop();
        }

        TlsServerFixture(const TlsServerFixture &) = delete;
        TlsServerFixture &operator=(const TlsServerFixture &) = delete;

        TempDirectory directory;
        EditorServer server;
        bool started = false;
        std::unique_ptr<MainLoop> loop;
    };

    /// Waits (up to a limit) for a condition the server's threads make true
    template <typename Condition>
    bool WaitFor(Condition condition, std::chrono::milliseconds limit = std::chrono::seconds(10))
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= end)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }
}

// ==================== The identity: certificate and key files ====================

TEST(EditorTlsIdentityTest, FirstUseMakesACertificateAndKeyAndALaterLoadKeepsThem)
{
    TempDirectory temp;
    const fs::path directory = temp.Path() / "editor-tls"; // not there yet: Load makes it

    auto first = EditorTlsServer::Load(directory);
    ASSERT_TRUE(first.has_value()) << first.error();
    const std::string fingerprint = (*first)->Fingerprint();
    ASSERT_EQ(fingerprint.size(), 64u);
    for (const char c : fingerprint)
    {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "lowercase hex, no colons: " << fingerprint;
    }

    const fs::path certificateFile = directory / EditorTlsServer::CertificateFileName;
    const fs::path keyFile = directory / EditorTlsServer::KeyFileName;
    ASSERT_TRUE(fs::exists(certificateFile));
    ASSERT_TRUE(fs::exists(keyFile));
    const std::string certificateText = ReadFileText(certificateFile);
    const std::string keyText = ReadFileText(keyFile);
    EXPECT_NE(certificateText.find("BEGIN CERTIFICATE"), std::string::npos);
    EXPECT_EQ(certificateText.find("PRIVATE KEY"), std::string::npos) << "the certificate file never holds the key";
    EXPECT_NE(keyText.find("PRIVATE KEY"), std::string::npos);

    // The fingerprint is the SHA-256 of the certificate in the file, as a client would compute it
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    ASSERT_EQ(mbedtls_x509_crt_parse(&parsed, reinterpret_cast<const unsigned char *>(certificateText.c_str()),
                                     certificateText.size() + 1),
              0);
    unsigned char digest[32];
    ASSERT_EQ(mbedtls_sha256(parsed.raw.p, parsed.raw.len, digest, 0), 0);
    mbedtls_x509_crt_free(&parsed);
    EXPECT_EQ(Hex(digest, sizeof(digest)), fingerprint);

    // A second load (a restarted host) keeps both files and so the fingerprint a client pinned
    auto second = EditorTlsServer::Load(directory);
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_EQ((*second)->Fingerprint(), fingerprint);
    EXPECT_EQ(ReadFileText(certificateFile), certificateText);
    EXPECT_EQ(ReadFileText(keyFile), keyText);
}

TEST(EditorTlsIdentityTest, OnlyOneOfTheTwoFilesIsAnErrorAndNothingIsReplaced)
{
    TempDirectory temp;
    {
        auto made = EditorTlsServer::Load(temp.Path());
        ASSERT_TRUE(made.has_value()) << made.error();
    }
    const fs::path certificateFile = temp.Path() / EditorTlsServer::CertificateFileName;
    const fs::path keyFile = temp.Path() / EditorTlsServer::KeyFileName;
    const std::string keyText = ReadFileText(keyFile);

    ASSERT_TRUE(fs::remove(certificateFile));
    const auto lost = EditorTlsServer::Load(temp.Path());
    ASSERT_FALSE(lost.has_value());
    EXPECT_NE(lost.error().find("only one"), std::string::npos) << lost.error();
    EXPECT_FALSE(fs::exists(certificateFile)) << "a certificate is not quietly made beside a key";
    EXPECT_EQ(ReadFileText(keyFile), keyText) << "the key is never replaced";
}

TEST(EditorTlsIdentityTest, FilesThatAreNotACertificateAndKeyAreRefusedAndKept)
{
    TempDirectory temp;
    WriteFileText(temp.Path() / EditorTlsServer::CertificateFileName, "not a certificate\n");
    WriteFileText(temp.Path() / EditorTlsServer::KeyFileName, "not a key\n");

    const auto loaded = EditorTlsServer::Load(temp.Path());
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().find("isn't a valid"), std::string::npos) << loaded.error();
    EXPECT_EQ(ReadFileText(temp.Path() / EditorTlsServer::KeyFileName), "not a key\n");
}

TEST(EditorTlsIdentityTest, ACertificateAndAKeyThatDontBelongTogetherAreRefused)
{
    TempDirectory first;
    TempDirectory second;
    {
        ASSERT_TRUE(EditorTlsServer::Load(first.Path()).has_value());
        ASSERT_TRUE(EditorTlsServer::Load(second.Path()).has_value());
    }
    // The first's certificate with the second's key
    WriteFileText(second.Path() / EditorTlsServer::CertificateFileName,
                  ReadFileText(first.Path() / EditorTlsServer::CertificateFileName));
    const auto loaded = EditorTlsServer::Load(second.Path());
    ASSERT_FALSE(loaded.has_value());
    EXPECT_NE(loaded.error().find("don't belong together"), std::string::npos) << loaded.error();
}

TEST(EditorTlsIdentityTest, TheDefaultFolderIsEditorTlsBesideTheUserDataFolders)
{
    const fs::path directory = EditorTlsServer::DefaultDirectory();
    if (directory.empty())
    {
        GTEST_SKIP() << "this machine has no per-user data folder (no APPDATA or HOME)";
    }
    EXPECT_EQ(directory.filename(), "editor-tls");
    EXPECT_EQ(directory.parent_path(), IO::ProjectFile::UserDataBase());
    EXPECT_TRUE(directory.is_absolute());
}

// ==================== Over the socket ====================

TEST(EditorTlsServerTest, EnablingTlsNeedsAWorkingFolderAndCantChangeWhileServing)
{
    TempDirectory temp;
    EditorServer server;
    EXPECT_FALSE(server.IsTlsEnabled());
    EXPECT_TRUE(server.TlsFingerprint().empty());

    // An empty path is not a place to put a key
    EXPECT_FALSE(server.EnableTls({}).has_value());
    EXPECT_FALSE(server.IsTlsEnabled());

    ASSERT_TRUE(server.EnableTls(temp.Path()).has_value());
    EXPECT_TRUE(server.IsTlsEnabled());
    EXPECT_EQ(server.TlsFingerprint().size(), 64u);

    ASSERT_TRUE(server.Start(0));
    EXPECT_FALSE(server.EnableTls(temp.Path()).has_value()) << "not while it is running";
    server.Stop();
}

TEST(EditorTlsServerTest, AClientCompletesTheHandshakeSeesTheFingerprintAndRunsHelloAndACommand)
{
    TlsServerFixture host(TestToken);
    ASSERT_TRUE(host.started);

    TlsClient client(host.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    ASSERT_EQ(client.Handshake(), 0);

    // What the ready line reports is what the client sees, so a client that pins it can't be impersonated
    EXPECT_EQ(client.PeerFingerprint(), host.server.TlsFingerprint());
    EXPECT_EQ(client.PeerFingerprint().size(), 64u);
    RecordProperty("tls_version", client.Version());
    EXPECT_NE(client.Version().find("TLSv1."), std::string::npos) << client.Version();

    const auto hello = client.Roundtrip(Frame(CommandType::Hello, HelloPayload(TestToken)));
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);

    // A normal command after Hello: answered by the main loop, with the engine's own error (there is no scene)
    const auto command = client.Roundtrip(Frame(CommandType::DestroyEntity, NoSuchEntity));
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->type, ErrorType);
}

TEST(EditorTlsServerTest, ATls12OnlyClientIsServed)
{
    TlsServerFixture host;
    ASSERT_TRUE(host.started);

    TlsClient client(host.server.GetPort(), /*onlyTls12=*/true);
    ASSERT_TRUE(client.IsConnected());
    ASSERT_EQ(client.Handshake(), 0);
    EXPECT_EQ(client.Version(), "TLSv1.2");
    const auto hello = client.Roundtrip(Frame(CommandType::Hello, HelloPayload("")));
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);
}

TEST(EditorTlsServerTest, TheTokenGateAppliesOverTls)
{
    TlsServerFixture host(TestToken);
    ASSERT_TRUE(host.started);

    {
        // A command before Hello is refused with the usual error, and the connection is closed
        TlsClient client(host.server.GetPort());
        ASSERT_EQ(client.Handshake(), 0);
        const auto refused = client.Roundtrip(Frame(CommandType::DestroyEntity, NoSuchEntity));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType);
        EXPECT_EQ(refused->body, EditorServer::HelloRequiredError);
        EXPECT_TRUE(client.IsClosedByServer());
    }
    {
        // A wrong token is refused and ends the connection
        TlsClient client(host.server.GetPort());
        ASSERT_EQ(client.Handshake(), 0);
        const auto refused = client.Roundtrip(Frame(CommandType::Hello, HelloPayload("not the token")));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType);
        EXPECT_TRUE(client.IsClosedByServer());
    }
    {
        // The pre-Hello payload cap: a header declaring more than 64 KiB is refused before any payload is read
        TlsClient client(host.server.GetPort());
        ASSERT_EQ(client.Handshake(), 0);
        const auto refused = client.Roundtrip(Frame(CommandType::Hello, {}, EditorServer::MaxPayloadBytesBeforeHello + 1));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType);
        EXPECT_TRUE(client.IsClosedByServer());
    }
    {
        // The right token works, on a new connection (one client at a time, and the others have gone)
        TlsClient client(host.server.GetPort());
        ASSERT_EQ(client.Handshake(), 0);
        const auto hello = client.Roundtrip(Frame(CommandType::Hello, HelloPayload(TestToken)));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType);
    }
}

TEST(EditorTlsServerTest, AHelloThatNeverComesStillClosesTheConnectionAfterTheDeadline)
{
    TlsServerFixture host(TestToken, std::chrono::milliseconds(700));
    ASSERT_TRUE(host.started);

    TlsClient client(host.server.GetPort());
    ASSERT_EQ(client.Handshake(), 0);
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(client.IsClosedByServer());
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(8));
}

TEST(EditorTlsServerTest, SeveralFramesInOneTlsRecordAreAllAnswered)
{
    // The buffered-bytes case: the library decrypts a whole record at once, so the second request is already inside
    // it (and the socket is empty) when the server has read the first. A wait on the socket alone would never wake.
    TlsServerFixture host;
    ASSERT_TRUE(host.started);

    TlsClient client(host.server.GetPort());
    ASSERT_EQ(client.Handshake(), 0);

    std::vector<uint8_t> batch;
    for (int i = 0; i < 3; ++i)
    {
        const std::vector<uint8_t> request = Frame(CommandType::DestroyEntity, NoSuchEntity);
        batch.insert(batch.end(), request.begin(), request.end());
    }
    ASSERT_LT(batch.size(), 16000u) << "the test needs them to fit one record";
    ASSERT_TRUE(client.Send(batch)); // one write: one record

    for (int i = 0; i < 3; ++i)
    {
        const auto response = client.ReadResponse();
        ASSERT_TRUE(response.has_value()) << "response " << i << " never came";
        EXPECT_EQ(response->type, ErrorType);
    }
}

TEST(EditorTlsServerTest, AFrameLargerThanOneTlsRecordIsRead)
{
    TlsServerFixture host;
    ASSERT_TRUE(host.started);

    TlsClient client(host.server.GetPort());
    ASSERT_EQ(client.Handshake(), 0);

    // A CreateEntity whose name is 100000 bytes: several 16 KiB records. The scene isn't there, so the answer is an
    // Error, but only once the whole payload was read.
    BufferWriter w;
    w.WriteString(std::string(100000, 'n'));
    const std::vector<uint8_t> payload = w.Release();
    ASSERT_GT(payload.size(), 3u * 16384u);
    const auto response = client.Roundtrip(Frame(CommandType::CreateEntity, payload));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ErrorType);

    // And the connection is still in step: the next request is answered too
    const auto next = client.Roundtrip(Frame(CommandType::DestroyEntity, NoSuchEntity));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->type, ErrorType);
}

TEST(EditorTlsServerTest, GarbageInPlaceOfAHandshakeClosesTheConnectionAndTheServerKeepsAccepting)
{
    TlsServerFixture host;
    ASSERT_TRUE(host.started);

    {
        PlainConnection garbage(host.server.GetPort());
        ASSERT_TRUE(garbage.IsConnected());
        const std::string text = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        ASSERT_TRUE(garbage.Send(text.data(), text.size()));
        EXPECT_TRUE(garbage.DrainUntilClosed(std::chrono::seconds(8)));
    }
    {
        // Bytes that start like a TLS record but are not one
        PlainConnection garbage(host.server.GetPort());
        ASSERT_TRUE(garbage.IsConnected());
        std::vector<uint8_t> bytes(300, 0xFF);
        bytes[0] = 0x16;
        bytes[1] = 0x03;
        bytes[2] = 0x01;
        ASSERT_TRUE(garbage.Send(bytes.data(), bytes.size()));
        EXPECT_TRUE(garbage.DrainUntilClosed(std::chrono::seconds(8)));
    }

    // The accept loop is not wedged: a real client is served at once
    TlsClient client(host.server.GetPort());
    ASSERT_EQ(client.Handshake(), 0);
    const auto hello = client.Roundtrip(Frame(CommandType::Hello, HelloPayload("")));
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);
}

TEST(EditorTlsServerTest, AStalledHandshakeIsClosedAtTheDeadlineAndTheServerKeepsAccepting)
{
    // With no token: the handshake deadline is the Hello timeout whether or not Hello is required
    TlsServerFixture host("", std::chrono::milliseconds(600));
    ASSERT_TRUE(host.started);

    const auto begin = std::chrono::steady_clock::now();
    {
        // Connects and sends nothing
        PlainConnection silent(host.server.GetPort());
        ASSERT_TRUE(silent.IsConnected());
        EXPECT_TRUE(silent.DrainUntilClosed(std::chrono::seconds(8)));
    }
    const auto silentTook = std::chrono::steady_clock::now() - begin;
    EXPECT_GE(silentTook, std::chrono::milliseconds(400)) << "it was held until the deadline, not dropped early";
    EXPECT_LT(silentTook, std::chrono::seconds(6));

    {
        // Starts a TLS record and stops
        PlainConnection partial(host.server.GetPort());
        ASSERT_TRUE(partial.IsConnected());
        const uint8_t start[3] = {0x16, 0x03, 0x01};
        ASSERT_TRUE(partial.Send(start, sizeof(start)));
        EXPECT_TRUE(partial.DrainUntilClosed(std::chrono::seconds(8)));
    }

    TlsClient client(host.server.GetPort());
    ASSERT_EQ(client.Handshake(), 0);
    const auto hello = client.Roundtrip(Frame(CommandType::Hello, HelloPayload("")));
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);
}

TEST(EditorTlsServerTest, APlaintextClientAgainstATlsHostGetsNoAnswerAndIsClosed)
{
    TlsServerFixture host(TestToken);
    ASSERT_TRUE(host.started);

    PlainConnection plain(host.server.GetPort());
    ASSERT_TRUE(plain.IsConnected());
    const std::vector<uint8_t> hello = Frame(CommandType::Hello, HelloPayload(TestToken));
    ASSERT_TRUE(plain.Send(hello.data(), hello.size()));

    std::vector<uint8_t> received;
    EXPECT_TRUE(plain.DrainUntilClosed(std::chrono::seconds(8), &received));
    // At most a TLS alert (content type 21): never a protocol response, and the token is not in anything it sent
    if (!received.empty())
    {
        EXPECT_EQ(received[0], 0x15) << "only a TLS alert may come back";
    }
    EXPECT_EQ(std::search(received.begin(), received.end(), TestToken, TestToken + std::strlen(TestToken)),
              received.end());
}

TEST(EditorTlsServerTest, AFailedHandshakeIsNotASessionSoStopOnDisconnectIgnoresIt)
{
    // No token, so every ordinary connection counts as a session; one that never finished a handshake must not
    TlsServerFixture host("", EditorServer::DefaultHelloTimeout, /*stopOnDisconnect=*/true);
    ASSERT_TRUE(host.started);

    {
        PlainConnection garbage(host.server.GetPort());
        ASSERT_TRUE(garbage.IsConnected());
        const std::string text = "not tls at all";
        ASSERT_TRUE(garbage.Send(text.data(), text.size()));
        EXPECT_TRUE(garbage.DrainUntilClosed(std::chrono::seconds(8)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_TRUE(host.server.IsRunning());
    EXPECT_FALSE(host.server.HasClient());

    {
        TlsClient client(host.server.GetPort());
        ASSERT_EQ(client.Handshake(), 0);
        const auto hello = client.Roundtrip(Frame(CommandType::Hello, HelloPayload("")));
        ASSERT_TRUE(hello.has_value());
    } // the client's session ends here
    EXPECT_TRUE(WaitFor([&] { return !host.server.IsRunning(); })) << "a real session ending still stops the host";
}

namespace
{
    /// Collects every log line while it exists (the subscription goes with it, even when an ASSERT returns early)
    class LogCapture
    {
    public:
        LogCapture()
            : _subscription(Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
              {
                  const std::lock_guard<std::mutex> lock(_mutex);
                  _lines.emplace_back(message);
              })
        {
        }

        ~LogCapture()
        {
            Logger::logEvent -= _subscription;
        }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] std::vector<std::string> Lines()
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            return _lines;
        }

    private:
        std::mutex _mutex;
        std::vector<std::string> _lines;
        size_t _subscription = 0;
    };
}

TEST(EditorTlsServerTest, NothingLogsTheTokenOrTheKey)
{
    LogCapture capture;
    std::string keyBody;
    {
        TlsServerFixture host(TestToken);
        ASSERT_TRUE(host.started);

        // The key's first base64 line (after the BEGIN line): any log line holding it would leak key material
        std::istringstream key(ReadFileText(host.directory.Path() / EditorTlsServer::KeyFileName));
        std::string line;
        std::getline(key, line);
        std::getline(key, keyBody);
        ASSERT_GT(keyBody.size(), 20u);

        {
            TlsClient wrong(host.server.GetPort());
            ASSERT_EQ(wrong.Handshake(), 0);
            (void)wrong.Roundtrip(Frame(CommandType::Hello, HelloPayload("a-wrong-tls-token")));
        }
        {
            TlsClient right(host.server.GetPort());
            ASSERT_EQ(right.Handshake(), 0);
            ASSERT_TRUE(right.Roundtrip(Frame(CommandType::Hello, HelloPayload(TestToken))).has_value());
        }
        {
            PlainConnection garbage(host.server.GetPort());
            const std::string text = "garbage";
            (void)garbage.Send(text.data(), text.size());
            (void)garbage.DrainUntilClosed(std::chrono::seconds(8));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the main loop posts the last lines
    }

    const std::vector<std::string> lines = capture.Lines();
    EXPECT_FALSE(lines.empty());
    for (const std::string &line : lines)
    {
        EXPECT_EQ(line.find(TestToken), std::string::npos) << line;
        EXPECT_EQ(line.find("a-wrong-tls-token"), std::string::npos) << line;
        EXPECT_EQ(line.find("PRIVATE KEY"), std::string::npos) << line;
        EXPECT_EQ(line.find(keyBody), std::string::npos) << line;
    }
}

#else // !N2ENGINE_EDITOR_TLS

// Without the option there is no TLS to test: an empty program part (HostOptionsTests.cpp checks the refusal)
TEST(EditorTlsTest, ThisBuildHasNoTls)
{
    SUCCEED();
}

#endif
