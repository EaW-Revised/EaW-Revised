#include "relay_directory.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace eawr::net::relay {
core::Result<std::string> password_hash(std::string_view password, std::string_view salt) {
    if (password.empty() || password.size() > 128 || salt.size() != 32)
        return core::Result<std::string>::failure(error(1540, "Invalid password hash input"));
    std::array<unsigned char, 32> result{};
    bool success{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        success = BCryptDeriveKeyPBKDF2(algorithm,
            reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
            reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), static_cast<ULONG>(salt.size()),
            120000, result.data(), static_cast<ULONG>(result.size()), 0) == 0;
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
#else
    success = PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
        120000, EVP_sha256(), static_cast<int>(result.size()), result.data()) == 1;
#endif
    if (!success) return core::Result<std::string>::failure(error(1550, "Password hash failed"));
    constexpr char hex[] = "0123456789abcdef";
    std::string encoded; encoded.reserve(64);
    for (const auto byte : result) { encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
    return core::Result<std::string>::success(std::move(encoded));
}
} // namespace eawr::net::relay
