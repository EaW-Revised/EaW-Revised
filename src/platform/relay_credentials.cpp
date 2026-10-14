#include "eawr/platform/relay_credentials.hpp"

#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
#endif

namespace eawr::platform::relay_credentials {
bool exists(const std::string& path) { return std::filesystem::exists(path); }
#ifdef _WIN32
int read_certificate(mbedtls_x509_crt* certificate, const char* path) {
    return mbedtls_x509_crt_parse_file(certificate, path);
}
int read_private_key(mbedtls_pk_context* key, const char* path, const char* password,
                     int (*random)(void*, unsigned char*, std::size_t), void* context) {
    return mbedtls_pk_parse_keyfile(key, path, password, random, context);
}
bool write_private_key(const std::string& path, const unsigned char* pem) {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    struct Handle { HANDLE value; ~Handle() { CloseHandle(value); } } token_guard{token};
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return false;
    std::vector<unsigned char> information(size);
    if (!GetTokenInformation(token, TokenUser, information.data(), size, &size)) return false;
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(information.data())->User.Sid, &sid)) return false;
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> sid_guard(sid, LocalFree);
    const auto descriptor = std::wstring(L"O:") + sid + L"D:P(A;;FA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR raw{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &raw, nullptr)) return false;
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor_guard(raw, LocalFree);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    const auto file = CreateFileW(std::filesystem::path(path).c_str(), GENERIC_WRITE, 0, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const Handle file_guard{file};
    const auto length = static_cast<DWORD>(std::strlen(reinterpret_cast<const char*>(pem)));
    DWORD written{};
    return WriteFile(file, pem, length, &written, nullptr) && written == length;
}
bool write_certificate(const std::string& path, const unsigned char* pem) {
    std::ofstream output(path);
    output << pem;
    output.close();
    return static_cast<bool>(output);
}
#else
int read_certificate_chain(SSL_CTX* context, const char* path) {
    return SSL_CTX_use_certificate_chain_file(context, path);
}
int read_private_key(SSL_CTX* context, const char* path, int type) {
    return SSL_CTX_use_PrivateKey_file(context, path, type);
}
int read_trust(SSL_CTX* context, const char* path, const char* directory) {
    return SSL_CTX_load_verify_locations(context, path, directory);
}
BIO* open_pem(const char* path, const char* mode) { return BIO_new_file(path, mode); }
void restrict_private_key(const std::string& path) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}
#endif
} // namespace eawr::platform::relay_credentials
