// Mutex type for MBEDTLS_THREADING_ALT: a pointer to a std::mutex owned by the callbacks in EditorTls.cpp.
#ifndef N2_MBEDTLS_THREADING_ALT_H
#define N2_MBEDTLS_THREADING_ALT_H

typedef struct mbedtls_threading_mutex_t
{
    void *impl;
} mbedtls_threading_mutex_t;

#endif
