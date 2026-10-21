#include "engine/security/Crypto.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#if N2SECURITY_HAS_CRYPTO
#include <mbedtls/bignum.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>

#if !defined(MBEDTLS_ECP_DP_CURVE25519_ENABLED) || !defined(MBEDTLS_ECDH_C) || !defined(MBEDTLS_CHACHAPOLY_C) ||          \
    !defined(MBEDTLS_HKDF_C) || !defined(MBEDTLS_SHA256_C) || !defined(MBEDTLS_CTR_DRBG_C) || !defined(MBEDTLS_THREADING_ALT)
#error "n2security needs Curve25519 ECDH, ChaCha20-Poly1305, HKDF, SHA-256, CTR_DRBG and MBEDTLS_THREADING_ALT in the Mbed TLS configuration"
#endif
#endif

namespace N2Engine::Security
{
#if N2SECURITY_HAS_CRYPTO
    namespace
    {
        // Mbed TLS threading callbacks (MBEDTLS_THREADING_ALT, cmake/mbedtls/config/): a std::mutex per library mutex.
        // These were the editor host's (EditorTls.cpp); the host now calls InitializeCrypto, so one process installs
        // them once.
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

        /// The process's random generator: seeded on first use, then kept (the library reseeds it on its interval)
        struct SharedRandom
        {
            std::mutex mutex;
            bool seeded = false;
            bool failed = false;
            mbedtls_entropy_context entropy;
            mbedtls_ctr_drbg_context drbg;

            SharedRandom()
            {
                mbedtls_entropy_init(&entropy);
                mbedtls_ctr_drbg_init(&drbg);
            }
            ~SharedRandom()
            {
                mbedtls_ctr_drbg_free(&drbg);
                mbedtls_entropy_free(&entropy);
            }
            SharedRandom(const SharedRandom &) = delete;
            SharedRandom &operator=(const SharedRandom &) = delete;
        };

        SharedRandom &Random()
        {
            static SharedRandom random;
            return random;
        }

        /// Fills `output` from the shared generator; the caller holds random.mutex
        bool FillLocked(SharedRandom &random, unsigned char *output, std::size_t size)
        {
            if (!random.seeded)
            {
                if (random.failed)
                {
                    return false;
                }
                static constexpr unsigned char kPersonalisation[] = "n2security random";
                if (mbedtls_ctr_drbg_seed(&random.drbg, mbedtls_entropy_func, &random.entropy, kPersonalisation,
                                          sizeof(kPersonalisation) - 1) != 0)
                {
                    random.failed = true;
                    return false;
                }
                random.seeded = true;
            }
            std::size_t done = 0;
            while (done < size)
            {
                const std::size_t step = std::min<std::size_t>(size - done, MBEDTLS_CTR_DRBG_MAX_REQUEST);
                if (mbedtls_ctr_drbg_random(&random.drbg, output + done, step) != 0)
                {
                    return false;
                }
                done += step;
            }
            return true;
        }

        /// The f_rng the ECP functions take (key generation, and the blinding of the scalar multiplication)
        int EcpRandom(void * /*context*/, unsigned char *output, std::size_t size)
        {
            SharedRandom &random = Random();
            const std::lock_guard lock(random.mutex);
            return FillLocked(random, output, size) ? 0 : MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
        }

        /// The Curve25519 group and its owned MPIs and points, freed (and wiped by the library) on every path
        struct X25519Scratch
        {
            mbedtls_ecp_group group;
            mbedtls_mpi secret;
            mbedtls_mpi shared;
            mbedtls_ecp_point point;

            X25519Scratch()
            {
                mbedtls_ecp_group_init(&group);
                mbedtls_mpi_init(&secret);
                mbedtls_mpi_init(&shared);
                mbedtls_ecp_point_init(&point);
            }
            ~X25519Scratch()
            {
                mbedtls_ecp_point_free(&point);
                mbedtls_mpi_free(&shared);
                mbedtls_mpi_free(&secret);
                mbedtls_ecp_group_free(&group);
            }
            X25519Scratch(const X25519Scratch &) = delete;
            X25519Scratch &operator=(const X25519Scratch &) = delete;
        };
    } // namespace

    bool CryptoAvailable() noexcept
    {
        return true;
    }

    bool InitializeCrypto()
    {
        static const bool ok = []
        {
            mbedtls_threading_set_alt(MutexInit, MutexFree, MutexLock, MutexUnlock);
            return psa_crypto_init() == PSA_SUCCESS;
        }();
        return ok;
    }

    bool Sha256(std::span<const std::uint8_t> data, Sha256Digest &digest)
    {
        if (!InitializeCrypto())
        {
            return false;
        }
        return mbedtls_sha256(data.data(), data.size(), digest.data(), 0) == 0;
    }

    bool HmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data, Sha256Digest &mac)
    {
        if (!InitializeCrypto())
        {
            return false;
        }
        const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (info == nullptr)
        {
            return false;
        }
        return mbedtls_md_hmac(info, key.data(), key.size(), data.data(), data.size(), mac.data()) == 0;
    }

    bool HkdfSha256(std::span<const std::uint8_t> salt, std::span<const std::uint8_t> inputKey,
                    std::span<const std::uint8_t> info, std::span<std::uint8_t> output)
    {
        if (!InitializeCrypto())
        {
            return false;
        }
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (md == nullptr || output.empty() || output.size() > 255 * kSha256Size)
        {
            return false;
        }
        return mbedtls_hkdf(md, salt.data(), salt.size(), inputKey.data(), inputKey.size(), info.data(), info.size(),
                            output.data(), output.size()) == 0;
    }

    bool RandomBytes(std::span<std::uint8_t> output)
    {
        if (!InitializeCrypto())
        {
            return false;
        }
        if (output.empty())
        {
            return true;
        }
        SharedRandom &random = Random();
        const std::lock_guard lock(random.mutex);
        return FillLocked(random, output.data(), output.size());
    }

    bool X25519GenerateKeyPair(X25519Key &privateKey, X25519Key &publicKey)
    {
        if (!InitializeCrypto())
        {
            return false;
        }
        X25519Scratch scratch;
        std::size_t written = 0;
        const bool ok =
            mbedtls_ecp_group_load(&scratch.group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
            mbedtls_ecdh_gen_public(&scratch.group, &scratch.secret, &scratch.point, EcpRandom, nullptr) == 0 &&
            mbedtls_mpi_write_binary_le(&scratch.secret, privateKey.data(), privateKey.size()) == 0 &&
            mbedtls_ecp_point_write_binary(&scratch.group, &scratch.point, MBEDTLS_ECP_PF_UNCOMPRESSED, &written,
                                           publicKey.data(), publicKey.size()) == 0 &&
            written == publicKey.size();
        if (!ok)
        {
            SecureZero(privateKey.data(), privateKey.size());
        }
        return ok;
    }

    bool X25519SharedSecret(const X25519Key &privateKey, const X25519Key &peerPublicKey, X25519Key &shared)
    {
        if (!InitializeCrypto())
        {
            return false;
        }
        X25519Scratch scratch;
        bool ok = mbedtls_ecp_group_load(&scratch.group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
                  mbedtls_mpi_read_binary_le(&scratch.secret, privateKey.data(), privateKey.size()) == 0 &&
                  mbedtls_ecp_point_read_binary(&scratch.group, &scratch.point, peerPublicKey.data(),
                                                peerPublicKey.size()) == 0 &&
                  mbedtls_ecp_check_pubkey(&scratch.group, &scratch.point) == 0 &&
                  mbedtls_ecdh_compute_shared(&scratch.group, &scratch.shared, &scratch.point, &scratch.secret,
                                              EcpRandom, nullptr) == 0 &&
                  mbedtls_mpi_write_binary_le(&scratch.shared, shared.data(), shared.size()) == 0;
        if (ok)
        {
            // RFC 7748 section 6.1: an all-zero result means a low-order peer key; refuse it (no early exit)
            std::uint8_t any = 0;
            for (const std::uint8_t byte : shared)
            {
                any = static_cast<std::uint8_t>(any | byte);
            }
            ok = any != 0;
        }
        if (!ok)
        {
            SecureZero(shared.data(), shared.size());
        }
        return ok;
    }

    bool ConstantTimeEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        // Every byte, whatever they hold: no early exit. (Not mbedtls_ct_memcmp: its header has no C++ linkage guard.)
        volatile std::uint8_t difference = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
        }
        return difference == 0;
    }

    void SecureZero(void *data, std::size_t size) noexcept
    {
        if (data != nullptr && size > 0)
        {
            mbedtls_platform_zeroize(data, size);
        }
    }

    // ==================== ChaCha20Poly1305 ====================

    struct ChaCha20Poly1305::Impl
    {
        mbedtls_chachapoly_context context;
        bool hasKey = false;

        Impl() { mbedtls_chachapoly_init(&context); }
        ~Impl() { mbedtls_chachapoly_free(&context); }
        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
    };

    ChaCha20Poly1305::ChaCha20Poly1305() : impl_(std::make_unique<Impl>()) {}
    ChaCha20Poly1305::~ChaCha20Poly1305() = default;
    ChaCha20Poly1305::ChaCha20Poly1305(ChaCha20Poly1305 &&) noexcept = default;
    ChaCha20Poly1305 &ChaCha20Poly1305::operator=(ChaCha20Poly1305 &&) noexcept = default;

    bool ChaCha20Poly1305::SetKey(const Key &key)
    {
        if (!impl_ || !InitializeCrypto())
        {
            return false;
        }
        impl_->hasKey = mbedtls_chachapoly_setkey(&impl_->context, key.data()) == 0;
        return impl_->hasKey;
    }

    bool ChaCha20Poly1305::HasKey() const noexcept
    {
        return impl_ && impl_->hasKey;
    }

    bool ChaCha20Poly1305::Seal(const Nonce &nonce, std::span<const std::uint8_t> aad,
                                std::span<const std::uint8_t> plaintext, std::span<std::uint8_t> output)
    {
        if (!HasKey() || output.size() != plaintext.size() + kTagSize)
        {
            return false;
        }
        return mbedtls_chachapoly_encrypt_and_tag(&impl_->context, plaintext.size(), nonce.data(), aad.data(), aad.size(),
                                                  plaintext.data(), output.data(),
                                                  output.data() + plaintext.size()) == 0;
    }

    bool ChaCha20Poly1305::Open(const Nonce &nonce, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> sealed,
                                std::span<std::uint8_t> output)
    {
        if (!HasKey() || sealed.size() < kTagSize || output.size() != sealed.size() - kTagSize)
        {
            return false;
        }
        const std::size_t length = sealed.size() - kTagSize;
        return mbedtls_chachapoly_auth_decrypt(&impl_->context, length, nonce.data(), aad.data(), aad.size(),
                                               sealed.data() + length, sealed.data(), output.data()) == 0;
    }

#else // no crypto library (the web build)

    bool CryptoAvailable() noexcept
    {
        return false;
    }
    bool InitializeCrypto()
    {
        return false;
    }
    bool Sha256(std::span<const std::uint8_t>, Sha256Digest &)
    {
        return false;
    }
    bool HmacSha256(std::span<const std::uint8_t>, std::span<const std::uint8_t>, Sha256Digest &)
    {
        return false;
    }
    bool HkdfSha256(std::span<const std::uint8_t>, std::span<const std::uint8_t>, std::span<const std::uint8_t>,
                    std::span<std::uint8_t>)
    {
        return false;
    }
    bool RandomBytes(std::span<std::uint8_t> output)
    {
        return output.empty();
    }
    bool X25519GenerateKeyPair(X25519Key &, X25519Key &)
    {
        return false;
    }
    bool X25519SharedSecret(const X25519Key &, const X25519Key &, X25519Key &)
    {
        return false;
    }
    bool ConstantTimeEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        unsigned int difference = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            difference |= static_cast<unsigned int>(a[i] ^ b[i]);
        }
        return difference == 0;
    }
    void SecureZero(void *data, std::size_t size) noexcept
    {
        if (data == nullptr)
        {
            return;
        }
        volatile unsigned char *bytes = static_cast<volatile unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i)
        {
            bytes[i] = 0;
        }
    }

    struct ChaCha20Poly1305::Impl
    {
    };
    ChaCha20Poly1305::ChaCha20Poly1305() : impl_(std::make_unique<Impl>()) {}
    ChaCha20Poly1305::~ChaCha20Poly1305() = default;
    ChaCha20Poly1305::ChaCha20Poly1305(ChaCha20Poly1305 &&) noexcept = default;
    ChaCha20Poly1305 &ChaCha20Poly1305::operator=(ChaCha20Poly1305 &&) noexcept = default;
    bool ChaCha20Poly1305::SetKey(const Key &)
    {
        return false;
    }
    bool ChaCha20Poly1305::HasKey() const noexcept
    {
        return false;
    }
    bool ChaCha20Poly1305::Seal(const Nonce &, std::span<const std::uint8_t>, std::span<const std::uint8_t>,
                                std::span<std::uint8_t>)
    {
        return false;
    }
    bool ChaCha20Poly1305::Open(const Nonce &, std::span<const std::uint8_t>, std::span<const std::uint8_t>,
                                std::span<std::uint8_t>)
    {
        return false;
    }
#endif
}
