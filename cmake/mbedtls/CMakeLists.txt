# Mbed TLS 3.6 (vendored unmodified under external/mbedtls), built once for the whole tree. Native builds only: the web
# build has no UDP and no use for it (a browser transport gets its security from the browser), so the root CMakeLists.txt
# does not add this directory there.
#
#  - n2_mbedcrypto: the crypto modules only (no X.509, no TLS). Every native build has it: n2security uses it for the
#    netcode's handshake and packet sealing (X25519, HKDF-SHA-256, HMAC-SHA-256, ChaCha20-Poly1305, SHA-256, CTR_DRBG)
#    and for the token hashes the editor host and the game share.
#  - n2_mbedtls_tls: X.509 and TLS on top of it, only with N2ENGINE_EDITOR_TLS (the editor host's --tls).
#
# One configuration for both (the library's default plus config/n2_mbedtls_user_config.h, which turns threading on with
# std::mutex callbacks that n2security installs), so the two can never disagree on a structure's layout. The define and
# the include directories are PUBLIC: every target that includes an Mbed TLS header must see the same configuration.

set(N2_MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/external/mbedtls)

file(GLOB N2_MBEDTLS_ALL_SOURCES CONFIGURE_DEPENDS "${N2_MBEDTLS_DIR}/library/*.c")

# The split the library's own build uses: X.509 and PKCS#7, then TLS (debug, the MPS reader, sockets, ssl_*)
set(N2_MBEDTLS_X509_SOURCES "")
set(N2_MBEDTLS_TLS_SOURCES "")
set(N2_MBEDTLS_CRYPTO_SOURCES "")
foreach(_n2_source IN LISTS N2_MBEDTLS_ALL_SOURCES)
    get_filename_component(_n2_name "${_n2_source}" NAME)
    if(_n2_name MATCHES "^(x509.*|pkcs7)\\.c$")
        list(APPEND N2_MBEDTLS_X509_SOURCES "${_n2_source}")
    elseif(_n2_name MATCHES "^(ssl_.*|debug|mps_reader|mps_trace|net_sockets)\\.c$")
        list(APPEND N2_MBEDTLS_TLS_SOURCES "${_n2_source}")
    else()
        list(APPEND N2_MBEDTLS_CRYPTO_SOURCES "${_n2_source}")
    endif()
endforeach()

add_library(n2_mbedcrypto STATIC ${N2_MBEDTLS_CRYPTO_SOURCES})
target_include_directories(n2_mbedcrypto PUBLIC
    ${N2_MBEDTLS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)
# The library's own private headers (common.h and friends) sit next to its sources
target_include_directories(n2_mbedcrypto PRIVATE ${N2_MBEDTLS_DIR}/library)
target_compile_definitions(n2_mbedcrypto PUBLIC MBEDTLS_USER_CONFIG_FILE="n2_mbedtls_user_config.h")
# Mbed TLS takes its entropy from BCrypt on Windows
if(WIN32)
    target_link_libraries(n2_mbedcrypto PUBLIC bcrypt)
    set_target_properties(n2_mbedcrypto PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(N2ENGINE_EDITOR_TLS)
    add_library(n2_mbedtls_tls STATIC ${N2_MBEDTLS_X509_SOURCES} ${N2_MBEDTLS_TLS_SOURCES})
    target_include_directories(n2_mbedtls_tls PRIVATE ${N2_MBEDTLS_DIR}/library)
    target_link_libraries(n2_mbedtls_tls PUBLIC n2_mbedcrypto)
    if(WIN32)
        target_link_libraries(n2_mbedtls_tls PUBLIC ws2_32)
        set_target_properties(n2_mbedtls_tls PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()
