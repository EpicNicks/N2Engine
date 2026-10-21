// Extra Mbed TLS configuration (MBEDTLS_USER_CONFIG_FILE) for every target that uses the vendored library: the game's
// crypto (n2security, the netcode's handshake and packet sealing) and the editor host's TLS. The library is unmodified.
// Threading is on (with the std::mutex callbacks in engine/src/security/Crypto.cpp) because the editor tests run a TLS
// client and the host server in one process, and PSA keeps a global key-slot store that is not thread-safe without it.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
