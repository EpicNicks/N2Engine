#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace N2Engine::Editor
{
    /// What one non-blocking TLS call did. WantRead and WantWrite mean "call again once the socket is readable (or
    /// writable)", with the same arguments for Write.
    enum class TlsStep
    {
        Done,
        WantRead,
        WantWrite,
        /// The connection is unusable: the peer closed it, or the handshake or a record was refused
        Failed,
    };

    class EditorTlsServer;

    /**
     * One TLS connection, over a connected socket the caller owns (and closes after this goes). Every call is
     * non-blocking: the caller waits on the socket between calls (EditorServer::WaitUntilReady). Used from one thread.
     * Nothing here includes a TLS library header, so a build without N2ENGINE_EDITOR_TLS compiles the same interface
     * (the calls then fail).
     */
    class EditorTlsSession
    {
    public:
        ~EditorTlsSession();
        EditorTlsSession(const EditorTlsSession &) = delete;
        EditorTlsSession &operator=(const EditorTlsSession &) = delete;

        /// Advances the server side of the handshake; Done once the connection is secured
        [[nodiscard]] TlsStep Handshake();
        /// Reads up to `size` decrypted bytes; on Done `count` (at least 1) is how many arrived
        [[nodiscard]] TlsStep Read(void *data, size_t size, size_t &count);
        /// Encrypts and sends up to `size` bytes; on Done `count` (at least 1) is how many were taken. After
        /// WantWrite call again with the same data and size.
        [[nodiscard]] TlsStep Write(const void *data, size_t size, size_t &count);
        /// Decrypted bytes the library already holds, which a wait on the socket can't see: Read has them at once
        [[nodiscard]] bool HasBufferedInput() const;
        /// One best-effort attempt to tell the peer the connection is ending
        void Close();
        /// Why the last call failed, for a log line: never holds a key or any data that was sent
        [[nodiscard]] const std::string &LastError() const;

    private:
        friend class EditorTlsServer;
        struct Impl;
        explicit EditorTlsSession(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };

    /**
     * The host's TLS identity (a self-signed EC P-256 certificate and its key) and the settings every connection
     * shares: TLS 1.2 or newer (1.3 when the peer speaks it). Created once, before the server starts; sessions are
     * made and used on the network thread only.
     */
    class EditorTlsServer
    {
    public:
        /// The certificate file and key file inside a TLS directory
        static constexpr const char *CertificateFileName = "host-cert.pem";
        static constexpr const char *KeyFileName = "host-key.pem";

        /// The TLS library's one-time setup (thread-safety callbacks, then PSA crypto); safe to call repeatedly. The
        /// tests' in-process client calls it before touching the library. False when it fails or TLS isn't built in.
        [[nodiscard]] static bool InitializeLibrary();

        /// Whether this build has TLS (the N2ENGINE_EDITOR_TLS CMake option)
        [[nodiscard]] static bool IsAvailable();

        /// Where the host keeps its identity: <ProjectFile::UserDataBase()>/editor-tls, beside the per-project user
        /// data folders and never inside a project. Empty when the user has no data folder (UserDataBase fell back to
        /// the working directory), since a key must not be written there.
        [[nodiscard]] static std::filesystem::path DefaultDirectory();

        /**
         * Loads the certificate and key from `directory`, or makes and saves them when the directory has neither
         * (first use). An error when TLS isn't built in, when only one of the two files exists or either is invalid (a
         * key is never silently replaced: delete both to start over), or when a file can't be written. On POSIX the
         * directory and key file are created owner-only.
         */
        [[nodiscard]] static std::expected<std::unique_ptr<EditorTlsServer>, std::string>
        Load(const std::filesystem::path &directory);

        ~EditorTlsServer();
        EditorTlsServer(const EditorTlsServer &) = delete;
        EditorTlsServer &operator=(const EditorTlsServer &) = delete;

        /// SHA-256 of the certificate's DER encoding as 64 lowercase hex digits, no colons: what a client pins
        [[nodiscard]] const std::string &Fingerprint() const;

        /// A session for a connected socket (made non-blocking here), ready for Handshake; null on failure
        [[nodiscard]] std::unique_ptr<EditorTlsSession> NewSession(int socket) const;

    private:
        struct Impl;
        explicit EditorTlsServer(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };
}
