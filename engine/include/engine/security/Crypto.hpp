#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

// n2security's cryptographic primitives: thin wrappers over the vendored Mbed TLS 3.6 (n2_mbedcrypto, its crypto modules
// only), so no Mbed TLS type reaches a header. Native builds only: the web build has no crypto library, and there
// CryptoAvailable() is false and every function fails (returns false) without touching its outputs' secrecy.
//
// Every function checks every Mbed TLS return code and reports failure as false; none throws. Secrets that pass through
// a function's own stack (key schedules, intermediate MPIs) are wiped (mbedtls_platform_zeroize) before it returns.
// Thread-safe: the library runs with MBEDTLS_THREADING_C and std::mutex callbacks (cmake/mbedtls/config/), and the
// shared random generator has its own lock.
namespace N2Engine::Security
{
    /// True in a build that has the crypto library (every native build); false on the web build
    [[nodiscard]] bool CryptoAvailable() noexcept;

    /// The library's one-time setup: the threading callbacks (std::mutex), then PSA crypto. Idempotent and thread-safe;
    /// every function below calls it first. The editor host's TLS calls it too, so there is one setup per process.
    [[nodiscard]] bool InitializeCrypto();

    inline constexpr std::size_t kSha256Size = 32;
    using Sha256Digest = std::array<std::uint8_t, kSha256Size>;

    /// SHA-256 (FIPS 180-4)
    [[nodiscard]] bool Sha256(std::span<const std::uint8_t> data, Sha256Digest &digest);

    /// HMAC-SHA-256 (RFC 2104) of `data` under `key`
    [[nodiscard]] bool HmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data,
                                  Sha256Digest &mac);

    /// HKDF-SHA-256 (RFC 5869), extract then expand: `output.size()` bytes (at most 255 * 32)
    [[nodiscard]] bool HkdfSha256(std::span<const std::uint8_t> salt, std::span<const std::uint8_t> inputKey,
                                  std::span<const std::uint8_t> info, std::span<std::uint8_t> output);

    /// Bytes from the process's CTR_DRBG (AES-256), seeded from the operating system's entropy source (BCrypt on
    /// Windows, getrandom or /dev/urandom on Linux) on first use and reseeded by the library's own interval
    [[nodiscard]] bool RandomBytes(std::span<std::uint8_t> output);

    inline constexpr std::size_t kX25519KeySize = 32;
    /// An X25519 key or shared secret, in RFC 7748's little-endian encoding
    using X25519Key = std::array<std::uint8_t, kX25519KeySize>;

    /// A fresh X25519 key pair (Mbed TLS's Curve25519 ECDH: a clamped random scalar and its public u-coordinate)
    [[nodiscard]] bool X25519GenerateKeyPair(X25519Key &privateKey, X25519Key &publicKey);

    /// The X25519 shared secret of our private key and the peer's public key. False when the library refuses the peer's
    /// key or the result is all zero (a low-order public key: RFC 7748 section 6.1's check), so a peer cannot force a
    /// known secret.
    [[nodiscard]] bool X25519SharedSecret(const X25519Key &privateKey, const X25519Key &peerPublicKey, X25519Key &shared);

    /// True when the two hold the same bytes; the time depends on the length only, not on where they differ (different
    /// lengths are unequal at once: a length is not secret here)
    [[nodiscard]] bool ConstantTimeEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept;

    /// Overwrites the bytes with zeros in a way the compiler cannot drop (mbedtls_platform_zeroize)
    void SecureZero(void *data, std::size_t size) noexcept;

    /**
     * ChaCha20-Poly1305 (RFC 8439) with one key: a 32-byte key, 12-byte nonces, a 16-byte tag. The caller owns nonce
     * uniqueness (N2Net::SecureTransport builds every nonce from a per-direction packet counter, see docs/netcode.html).
     * Open checks the tag (in constant time, in the library) before it writes any plaintext the caller may use: on a
     * false return the output holds nothing meaningful and must be ignored.
     */
    class ChaCha20Poly1305
    {
    public:
        static constexpr std::size_t kKeySize = 32;
        static constexpr std::size_t kNonceSize = 12;
        static constexpr std::size_t kTagSize = 16;
        using Key = std::array<std::uint8_t, kKeySize>;
        using Nonce = std::array<std::uint8_t, kNonceSize>;

        ChaCha20Poly1305();
        ~ChaCha20Poly1305();
        ChaCha20Poly1305(const ChaCha20Poly1305 &) = delete;
        ChaCha20Poly1305 &operator=(const ChaCha20Poly1305 &) = delete;
        ChaCha20Poly1305(ChaCha20Poly1305 &&) noexcept;
        ChaCha20Poly1305 &operator=(ChaCha20Poly1305 &&) noexcept;

        /// False if the library refused it; the object then has no key and Seal and Open fail
        [[nodiscard]] bool SetKey(const Key &key);
        [[nodiscard]] bool HasKey() const noexcept;

        /// Writes plaintext.size() + kTagSize bytes (ciphertext, then tag) to `output`, which must be that large
        [[nodiscard]] bool Seal(const Nonce &nonce, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> plaintext,
                                std::span<std::uint8_t> output);
        /// `sealed` is ciphertext then tag; writes sealed.size() - kTagSize bytes to `output`. False when the tag does
        /// not verify (tampered, wrong key, wrong nonce or wrong aad) or the sizes are wrong.
        [[nodiscard]] bool Open(const Nonce &nonce, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> sealed,
                                std::span<std::uint8_t> output);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
