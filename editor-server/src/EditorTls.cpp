#include "editor-server/EditorTls.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <chrono>
#include <iterator>
#include <mutex>
#include <thread>
#include <string_view>
#include <system_error>
#include <utility>

#include "engine/io/ProjectFile.hpp"

#ifdef N2ENGINE_EDITOR_TLS

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include <mbedtls/build_info.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

namespace fs = std::filesystem;

namespace N2Engine::Editor
{
    namespace
    {
        // ---- The few platform-specific socket calls: everything else in this file is platform neutral ----

        /// What a BIO callback needs: the socket, as the int the server keeps it in
        struct Bio
        {
            int socket = -1;
        };

#ifdef _WIN32
        using RawSocket = SOCKET;

        int LastSocketError()
        {
            return WSAGetLastError();
        }

        bool IsWouldBlock(const int error)
        {
            return error == WSAEWOULDBLOCK;
        }

        bool IsReset(const int error)
        {
            return error == WSAECONNRESET || error == WSAECONNABORTED;
        }

        int SocketSend(const Bio &bio, const unsigned char *data, const size_t size)
        {
            const int length = static_cast<int>(std::min<size_t>(size, 1u << 30));
            return ::send(static_cast<RawSocket>(bio.socket), reinterpret_cast<const char *>(data), length, 0);
        }

        int SocketReceive(const Bio &bio, unsigned char *data, const size_t size)
        {
            const int length = static_cast<int>(std::min<size_t>(size, 1u << 30));
            return ::recv(static_cast<RawSocket>(bio.socket), reinterpret_cast<char *>(data), length, 0);
        }

        bool SetNonBlocking(const int socket)
        {
            u_long enable = 1;
            return ::ioctlsocket(static_cast<RawSocket>(socket), FIONBIO, &enable) == 0;
        }
#else
        using RawSocket = int;

        int LastSocketError()
        {
            return errno;
        }

        bool IsWouldBlock(const int error)
        {
            return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
        }

        bool IsReset(const int error)
        {
            return error == ECONNRESET || error == EPIPE;
        }

        int SocketSend(const Bio &bio, const unsigned char *data, const size_t size)
        {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            return static_cast<int>(::send(static_cast<RawSocket>(bio.socket), data, size, flags));
        }

        int SocketReceive(const Bio &bio, unsigned char *data, const size_t size)
        {
            return static_cast<int>(::recv(static_cast<RawSocket>(bio.socket), data, size, 0));
        }

        bool SetNonBlocking(const int socket)
        {
            const int flags = ::fcntl(socket, F_GETFL, 0);
            return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
        }
#endif

        // ---- mbedTLS glue ----

        int SendCallback(void *context, const unsigned char *data, const size_t size)
        {
            const int sent = SocketSend(*static_cast<const Bio *>(context), data, size);
            if (sent >= 0)
            {
                return sent;
            }
            const int error = LastSocketError();
            if (IsWouldBlock(error))
            {
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            }
            return IsReset(error) ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
        }

        int ReceiveCallback(void *context, unsigned char *data, const size_t size)
        {
            const int received = SocketReceive(*static_cast<const Bio *>(context), data, size);
            if (received >= 0)
            {
                return received; // 0 is the peer closing: mbedTLS reports it as the end of the connection
            }
            const int error = LastSocketError();
            if (IsWouldBlock(error))
            {
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            return IsReset(error) ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
        }

        std::string Describe(const int code)
        {
            std::array<char, 160> text{};
            mbedtls_strerror(code, text.data(), text.size());
            return std::format("{} (-0x{:04X})", text.data(), static_cast<unsigned>(-code));
        }

        TlsStep StepForError(const int code, std::string &error)
        {
            if (code == MBEDTLS_ERR_SSL_WANT_READ)
            {
                return TlsStep::WantRead;
            }
            if (code == MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                return TlsStep::WantWrite;
            }
            error = code == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? std::string("closed by the peer") : Describe(code);
            return TlsStep::Failed;
        }

        // Mbed TLS threading callbacks (MBEDTLS_THREADING_ALT, see mbedtls-config/): a std::mutex per library mutex
        void MutexInit(mbedtls_threading_mutex_t *mutex)
        {
            mutex->impl = new std::mutex();
        }

        void MutexFree(mbedtls_threading_mutex_t *mutex)
        {
            delete static_cast<std::mutex *>(mutex->impl);
            mutex->impl = nullptr;
        }

        int MutexLock(mbedtls_threading_mutex_t *mutex)
        {
            static_cast<std::mutex *>(mutex->impl)->lock();
            return 0;
        }

        int MutexUnlock(mbedtls_threading_mutex_t *mutex)
        {
            static_cast<std::mutex *>(mutex->impl)->unlock();
            return 0;
        }

        /// The library's one-time setup: threading (enabled because the tests run a client and the server's network
        /// thread in one process, and PSA's global key slots need it), then PSA crypto, which TLS 1.3 needs
        bool InitializeCrypto()
        {
            static const bool ok = []
            {
                mbedtls_threading_set_alt(MutexInit, MutexFree, MutexLock, MutexUnlock);
                return psa_crypto_init() == PSA_SUCCESS;
            }();
            return ok;
        }

        std::string ToHex(const unsigned char *bytes, const size_t count)
        {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string text;
            text.reserve(count * 2);
            for (size_t i = 0; i < count; ++i)
            {
                text += digits[bytes[i] >> 4];
                text += digits[bytes[i] & 0x0F];
            }
            return text;
        }

        std::expected<std::string, std::string> ReadWholeFile(const fs::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::unexpected("Can't read " + path.string());
            }
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (file.bad())
            {
                return std::unexpected("Can't read " + path.string());
            }
            return text;
        }

        /// Publishes `text` as `path` only if `path` doesn't exist yet (true), or reports that it does (false), so the
        /// loser of a race between two hosts never replaces the winner's file. The text goes to a uniquely named
        /// temporary file first and is hard-linked into place (atomic, fails when the name is taken), so a reader never
        /// sees half a file. On POSIX the file is made owner-only before anything is written to it (on Windows it
        /// inherits the user's profile folder, which only the user and administrators can read).
        std::expected<bool, std::string> WriteNewFile(const fs::path &path, const std::string_view text,
                                                      mbedtls_ctr_drbg_context &random)
        {
            unsigned char suffix[8];
            if (mbedtls_ctr_drbg_random(&random, suffix, sizeof(suffix)) != 0)
            {
                return std::unexpected("Couldn't pick a temporary name for " + path.string());
            }
            fs::path temporary = path;
            temporary += ".tmp-" + ToHex(suffix, sizeof(suffix));
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (!file)
                {
                    return std::unexpected("Can't write " + path.string());
                }
#ifndef _WIN32
                std::error_code ignored;
                fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                                ignored);
#endif
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
                file.flush();
                if (!file)
                {
                    file.close();
                    std::error_code ignored;
                    fs::remove(temporary, ignored);
                    return std::unexpected("Can't write " + path.string());
                }
            }
            std::error_code error;
            fs::create_hard_link(temporary, path, error);
            const bool taken = error && fs::exists(path, error);
            std::error_code ignored;
            fs::remove(temporary, ignored);
            if (taken)
            {
                return false;
            }
            if (!fs::exists(path, ignored))
            {
                return std::unexpected("Can't write " + path.string());
            }
            return true;
        }
    }

    // ==================== EditorTlsServer ====================

    struct EditorTlsServer::Impl
    {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context random;
        mbedtls_x509_crt certificate;
        mbedtls_pk_context key;
        mbedtls_ssl_config config;
        std::string fingerprint;

        Impl()
        {
            mbedtls_entropy_init(&entropy);
            mbedtls_ctr_drbg_init(&random);
            mbedtls_x509_crt_init(&certificate);
            mbedtls_pk_init(&key);
            mbedtls_ssl_config_init(&config);
        }

        ~Impl()
        {
            mbedtls_ssl_config_free(&config);
            mbedtls_pk_free(&key);
            mbedtls_x509_crt_free(&certificate);
            mbedtls_ctr_drbg_free(&random);
            mbedtls_entropy_free(&entropy);
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
    };

    namespace
    {
        /// A new self-signed EC P-256 certificate and its key, as PEM text
        struct GeneratedIdentity
        {
            std::string certificatePem;
            std::string keyPem;
        };

        std::expected<GeneratedIdentity, std::string> GenerateIdentity(mbedtls_ctr_drbg_context &random)
        {
            mbedtls_pk_context key;
            mbedtls_x509write_cert certificate;
            mbedtls_pk_init(&key);
            mbedtls_x509write_crt_init(&certificate);

            const auto fail = [&](const char *what, const int code) -> std::expected<GeneratedIdentity, std::string>
            {
                mbedtls_x509write_crt_free(&certificate);
                mbedtls_pk_free(&key);
                return std::unexpected(std::string("Couldn't make the TLS certificate: ") + what + ": " +
                                       Describe(code));
            };

            int code = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
            if (code != 0)
            {
                return fail("key setup", code);
            }
            code = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &random);
            if (code != 0)
            {
                return fail("key generation", code);
            }

            // A fixed, long validity: the client pins the certificate's fingerprint and never checks its dates, and a
            // clock that is wrong on the host must not make a fresh certificate "not yet valid" or "expired"
            mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
            mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
            mbedtls_x509write_crt_set_subject_key(&certificate, &key);
            mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
            code = mbedtls_x509write_crt_set_subject_name(&certificate, "CN=N2EditorHost");
            if (code == 0)
            {
                code = mbedtls_x509write_crt_set_issuer_name(&certificate, "CN=N2EditorHost");
            }
            if (code == 0)
            {
                unsigned char serial[16];
                code = mbedtls_ctr_drbg_random(&random, serial, sizeof(serial));
                if (code == 0)
                {
                    serial[0] = static_cast<unsigned char>((serial[0] & 0x7F) | 0x40); // positive, never zero
                    code = mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial));
                }
            }
            if (code == 0)
            {
                code = mbedtls_x509write_crt_set_validity(&certificate, "20240101000000", "20740101000000");
            }
            if (code == 0)
            {
                code = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
            }
            if (code != 0)
            {
                return fail("certificate fields", code);
            }

            std::array<unsigned char, 4096> certificatePem{};
            code = mbedtls_x509write_crt_pem(&certificate, certificatePem.data(), certificatePem.size(),
                                             mbedtls_ctr_drbg_random, &random);
            if (code != 0)
            {
                return fail("signing", code);
            }
            std::array<unsigned char, 2048> keyPem{};
            code = mbedtls_pk_write_key_pem(&key, keyPem.data(), keyPem.size());
            if (code != 0)
            {
                mbedtls_platform_zeroize(keyPem.data(), keyPem.size());
                return fail("key export", code);
            }

            GeneratedIdentity identity{reinterpret_cast<const char *>(certificatePem.data()),
                                       reinterpret_cast<const char *>(keyPem.data())};
            mbedtls_x509write_crt_free(&certificate);
            mbedtls_pk_free(&key);
            // The buffer held the key: don't leave it on the stack
            mbedtls_platform_zeroize(keyPem.data(), keyPem.size());
            return identity;
        }
    }

    bool EditorTlsServer::InitializeLibrary()
    {
        return InitializeCrypto();
    }

    bool EditorTlsServer::IsAvailable()
    {
        return true;
    }

    fs::path EditorTlsServer::DefaultDirectory()
    {
        const fs::path base = IO::ProjectFile::UserDataBase();
        // "." is what UserDataBase falls back to when neither APPDATA nor HOME is set: the working directory could be
        // a project, and a private key must never land in one
        if (base.empty() || base == fs::path("."))
        {
            return {};
        }
        return base / "editor-tls";
    }

    EditorTlsServer::EditorTlsServer(std::unique_ptr<Impl> impl) : _impl(std::move(impl))
    {
    }

    EditorTlsServer::~EditorTlsServer() = default;

    const std::string &EditorTlsServer::Fingerprint() const
    {
        return _impl->fingerprint;
    }

    std::expected<std::unique_ptr<EditorTlsServer>, std::string> EditorTlsServer::Load(const fs::path &directory,
        std::chrono::milliseconds identityWait)
    {
        if (directory.empty())
        {
            return std::unexpected("There is no per-user folder for the TLS certificate");
        }
        if (!InitializeCrypto())
        {
            return std::unexpected("Couldn't start the TLS library's crypto");
        }

        auto impl = std::make_unique<Impl>();
        int code = mbedtls_ctr_drbg_seed(&impl->random, mbedtls_entropy_func, &impl->entropy, nullptr, 0);
        if (code != 0)
        {
            return std::unexpected("Couldn't seed the TLS random generator: " + Describe(code));
        }

        const fs::path certificatePath = directory / CertificateFileName;
        const fs::path keyPath = directory / KeyFileName;
        std::error_code error;
        bool hasCertificate = fs::exists(certificatePath, error);
        const bool hasKey = fs::exists(keyPath, error);
        if (hasKey && !hasCertificate)
        {
            // Another host publishes the key first and the certificate a moment later, so a key alone may be creation
            // in progress: wait for the certificate before treating it as a half-made identity
            const auto end = std::chrono::steady_clock::now() + identityWait;
            while (!(hasCertificate = fs::exists(certificatePath, error)) && std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (hasCertificate != hasKey)
        {
            // Replacing one silently would change what clients have pinned, or leave a key that matches nothing
            return std::unexpected(std::format("{} has only one of {} and {}: delete both to make a new identity",
                                               directory.string(), CertificateFileName, KeyFileName));
        }

        if (!hasCertificate)
        {
            auto generated = GenerateIdentity(impl->random);
            if (!generated)
            {
                return std::unexpected(generated.error());
            }
            fs::create_directories(directory, error);
            if (error)
            {
                return std::unexpected("Can't create " + directory.string());
            }
#ifndef _WIN32
            fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
#endif
            // The key first, and only the key is secret. Neither file is ever replaced: when another host got there
            // first it wins, and this one waits for the winner's certificate and uses that identity instead.
            auto written = WriteNewFile(keyPath, generated->keyPem, impl->random);
            mbedtls_platform_zeroize(generated->keyPem.data(), generated->keyPem.size());
            if (!written)
            {
                return std::unexpected(written.error());
            }
            if (*written)
            {
                written = WriteNewFile(certificatePath, generated->certificatePem, impl->random);
                if (!written)
                {
                    return std::unexpected(written.error());
                }
            }
            else
            {
                const auto end = std::chrono::steady_clock::now() + identityWait;
                while (!fs::exists(certificatePath, error) && std::chrono::steady_clock::now() < end)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        }

        const auto certificateText = ReadWholeFile(certificatePath);
        if (!certificateText)
        {
            return std::unexpected(certificateText.error());
        }
        auto keyText = ReadWholeFile(keyPath);
        if (!keyText)
        {
            return std::unexpected(keyText.error());
        }
        // The PEM parsers want the terminating NUL counted in the length
        code = mbedtls_x509_crt_parse(&impl->certificate,
                                      reinterpret_cast<const unsigned char *>(certificateText->c_str()),
                                      certificateText->size() + 1);
        if (code != 0)
        {
            mbedtls_platform_zeroize(keyText->data(), keyText->size());
            return std::unexpected(std::format("{} isn't a valid certificate: {}", certificatePath.string(),
                                               Describe(code)));
        }
        code = mbedtls_pk_parse_key(&impl->key, reinterpret_cast<const unsigned char *>(keyText->c_str()),
                                    keyText->size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &impl->random);
        mbedtls_platform_zeroize(keyText->data(), keyText->size());
        if (code != 0)
        {
            return std::unexpected(std::format("{} isn't a valid private key: {}", keyPath.string(), Describe(code)));
        }
        code = mbedtls_pk_check_pair(&impl->certificate.pk, &impl->key, mbedtls_ctr_drbg_random, &impl->random);
        if (code != 0)
        {
            return std::unexpected(std::format("{} and {} don't belong together: delete both to make a new identity",
                                               CertificateFileName, KeyFileName));
        }

        code = mbedtls_ssl_config_defaults(&impl->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT);
        if (code != 0)
        {
            return std::unexpected("Couldn't set up the TLS configuration: " + Describe(code));
        }
        mbedtls_ssl_conf_min_tls_version(&impl->config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_rng(&impl->config, mbedtls_ctr_drbg_random, &impl->random);
        // The clients are not authenticated by certificate: the access token does that (Hello)
        mbedtls_ssl_conf_authmode(&impl->config, MBEDTLS_SSL_VERIFY_NONE);
        code = mbedtls_ssl_conf_own_cert(&impl->config, &impl->certificate, &impl->key);
        if (code != 0)
        {
            return std::unexpected("Couldn't use the TLS certificate: " + Describe(code));
        }

        unsigned char digest[32];
        code = mbedtls_sha256(impl->certificate.raw.p, impl->certificate.raw.len, digest, 0);
        if (code != 0)
        {
            return std::unexpected("Couldn't fingerprint the TLS certificate: " + Describe(code));
        }
        impl->fingerprint = ToHex(digest, sizeof(digest));

        return std::unique_ptr<EditorTlsServer>(new EditorTlsServer(std::move(impl)));
    }

    // ==================== EditorTlsSession ====================

    struct EditorTlsSession::Impl
    {
        mbedtls_ssl_context ssl;
        Bio bio;
        std::string error;

        Impl()
        {
            mbedtls_ssl_init(&ssl);
        }

        ~Impl()
        {
            mbedtls_ssl_free(&ssl);
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
    };

    std::unique_ptr<EditorTlsSession> EditorTlsServer::NewSession(const int socket) const
    {
        auto impl = std::make_unique<EditorTlsSession::Impl>();
        impl->bio.socket = socket;
        if (mbedtls_ssl_setup(&impl->ssl, &_impl->config) != 0 || !SetNonBlocking(socket))
        {
            return nullptr;
        }
        mbedtls_ssl_set_bio(&impl->ssl, &impl->bio, SendCallback, ReceiveCallback, nullptr);
        return std::unique_ptr<EditorTlsSession>(new EditorTlsSession(std::move(impl)));
    }

    EditorTlsSession::EditorTlsSession(std::unique_ptr<Impl> impl) : _impl(std::move(impl))
    {
    }

    EditorTlsSession::~EditorTlsSession() = default;

    TlsStep EditorTlsSession::Handshake()
    {
        const int code = mbedtls_ssl_handshake(&_impl->ssl);
        return code == 0 ? TlsStep::Done : StepForError(code, _impl->error);
    }

    TlsStep EditorTlsSession::Read(void *data, const size_t size, size_t &count)
    {
        const int code = mbedtls_ssl_read(&_impl->ssl, static_cast<unsigned char *>(data), size);
        if (code > 0)
        {
            count = static_cast<size_t>(code);
            return TlsStep::Done;
        }
        if (code == 0)
        {
            _impl->error = "closed by the peer";
            return TlsStep::Failed;
        }
        return StepForError(code, _impl->error);
    }

    TlsStep EditorTlsSession::Write(const void *data, const size_t size, size_t &count)
    {
        const int code = mbedtls_ssl_write(&_impl->ssl, static_cast<const unsigned char *>(data), size);
        if (code > 0)
        {
            count = static_cast<size_t>(code);
            return TlsStep::Done;
        }
        return StepForError(code == 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : code, _impl->error);
    }

    bool EditorTlsSession::HasBufferedInput() const
    {
        return mbedtls_ssl_get_bytes_avail(&_impl->ssl) > 0;
    }

    void EditorTlsSession::Close()
    {
        (void)mbedtls_ssl_close_notify(&_impl->ssl);
    }

    const std::string &EditorTlsSession::LastError() const
    {
        return _impl->error;
    }
}

#else // !N2ENGINE_EDITOR_TLS: the same interface, with no TLS library behind it

namespace N2Engine::Editor
{
    struct EditorTlsServer::Impl
    {
    };

    struct EditorTlsSession::Impl
    {
        std::string error = "this build has no TLS support";
    };

    bool EditorTlsServer::InitializeLibrary()
    {
        return false;
    }

    bool EditorTlsServer::IsAvailable()
    {
        return false;
    }

    std::filesystem::path EditorTlsServer::DefaultDirectory()
    {
        return {};
    }

    std::expected<std::unique_ptr<EditorTlsServer>, std::string> EditorTlsServer::Load(const std::filesystem::path &, std::chrono::milliseconds)
    {
        return std::unexpected("this build has no TLS support (configure with -DN2ENGINE_EDITOR_TLS=ON)");
    }

    EditorTlsServer::EditorTlsServer(std::unique_ptr<Impl> impl) : _impl(std::move(impl))
    {
    }

    EditorTlsServer::~EditorTlsServer() = default;

    const std::string &EditorTlsServer::Fingerprint() const
    {
        static const std::string none;
        return none;
    }

    std::unique_ptr<EditorTlsSession> EditorTlsServer::NewSession(int) const
    {
        return nullptr;
    }

    EditorTlsSession::EditorTlsSession(std::unique_ptr<Impl> impl) : _impl(std::move(impl))
    {
    }

    EditorTlsSession::~EditorTlsSession() = default;

    TlsStep EditorTlsSession::Handshake()
    {
        return TlsStep::Failed;
    }

    TlsStep EditorTlsSession::Read(void *, size_t, size_t &)
    {
        return TlsStep::Failed;
    }

    TlsStep EditorTlsSession::Write(const void *, size_t, size_t &)
    {
        return TlsStep::Failed;
    }

    bool EditorTlsSession::HasBufferedInput() const
    {
        return false;
    }

    void EditorTlsSession::Close()
    {
    }

    const std::string &EditorTlsSession::LastError() const
    {
        return _impl->error;
    }
}

#endif
