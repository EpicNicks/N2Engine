// Extra Mbed TLS configuration for the editor host (MBEDTLS_USER_CONFIG_FILE); the vendored library is unmodified.
// Threading is on (with the std::mutex callbacks in EditorTls.cpp) because the tests run a TLS client and the host
// server in one process, and PSA keeps a global key-slot store that is not thread-safe without it.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
