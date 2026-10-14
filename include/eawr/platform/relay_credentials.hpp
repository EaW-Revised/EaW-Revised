#pragma once

// Native credential file access for the optional relay TLS backends. Keep the
// backend parsers and their return values intact; TLS policy stays in net.
#include <cstddef>
#include <string>
#ifdef _WIN32
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#else
#include <openssl/ssl.h>
#endif

namespace eawr::platform::relay_credentials {
[[nodiscard]] bool exists(const std::string& path);
#ifdef _WIN32
int read_certificate(mbedtls_x509_crt* certificate, const char* path);
int read_private_key(mbedtls_pk_context* key, const char* path, const char* password,
                     int (*random)(void*, unsigned char*, std::size_t), void* context);
[[nodiscard]] bool write_private_key(const std::string& path, const unsigned char* pem);
[[nodiscard]] bool write_certificate(const std::string& path, const unsigned char* pem);
#else
int read_certificate_chain(SSL_CTX* context, const char* path);
int read_private_key(SSL_CTX* context, const char* path, int type);
int read_trust(SSL_CTX* context, const char* path, const char* directory);
[[nodiscard]] BIO* open_pem(const char* path, const char* mode);
void restrict_private_key(const std::string& path);
#endif
} // namespace eawr::platform::relay_credentials
