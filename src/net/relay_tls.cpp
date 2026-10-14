#include "relay_tls.hpp"
#include "relay_https.hpp"
#include "eawr/platform/relay_credentials.hpp"

#ifndef _WIN32

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include <algorithm>
#include <array>

namespace eawr::net::relay {
namespace {
Context context(bool server) {
    Context result(SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()), SSL_CTX_free);
    if (result && SSL_CTX_set_min_proto_version(result.get(), TLS1_2_VERSION) != 1) return {};
    if (result) {
        SSL_CTX_set_options(result.get(), SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_session_cache_mode(result.get(), SSL_SESS_CACHE_OFF);
        SSL_CTX_set_num_tickets(result.get(), 0);
    }
    return result;
}
}
core::Result<Context> server_context(const std::string& certificate, const std::string& key) {
    auto ctx = context(true);
    if (!ctx || platform::relay_credentials::read_certificate_chain(ctx.get(), certificate.c_str()) != 1
        || platform::relay_credentials::read_private_key(ctx.get(), key.c_str(), SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(ctx.get()) != 1)
        return core::Result<Context>::failure(error(1519, "Invalid relay TLS certificate or private key"));
    return core::Result<Context>::success(std::move(ctx));
}
core::Result<Context> client_context(const std::string& ca, const std::string& pin) {
    auto ctx = context(false);
    if (!ctx || (!pin.empty() && !code_valid(pin.substr(0, 32))) || (!pin.empty() && (pin.size() != 64 || !code_valid(pin.substr(32))))
        || (pin.empty() && (ca.empty() ? SSL_CTX_set_default_verify_paths(ctx.get())
                : platform::relay_credentials::read_trust(ctx.get(), ca.c_str(), nullptr)) != 1))
        return core::Result<Context>::failure(error(1519, "Cannot configure relay certificate trust"));
    SSL_CTX_set_verify(ctx.get(), pin.empty() ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    return core::Result<Context>::success(std::move(ctx));
}
namespace {
std::string fingerprint(X509* cert) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned size{};
    if (!cert || X509_digest(cert, EVP_sha256(), digest.data(), &size) != 1 || size != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < size; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}
}
core::Result<std::string> certificate_fingerprint(const std::string& path) {
    std::unique_ptr<BIO, decltype(&BIO_free)> file(platform::relay_credentials::open_pem(path.c_str(), "r"), BIO_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(file ? PEM_read_bio_X509(file.get(), nullptr, nullptr, nullptr) : nullptr, X509_free);
    const auto value = fingerprint(cert.get());
    if (value.empty()) return core::Result<std::string>::failure(error(1519, "Cannot read relay certificate"));
    return core::Result<std::string>::success(value);
}
core::Result<void> create_certificate(const std::string& certificate, const std::string& key, std::chrono::seconds lifetime, const std::string& dns_name) {
    if (lifetime < std::chrono::seconds(-59) || lifetime > std::chrono::hours(24 * 365) || !certificate_name_valid(dns_name))
        return core::Result<void>::failure(error(1519, "Invalid certificate lifetime"));
    if (platform::relay_credentials::exists(certificate) || platform::relay_credentials::exists(key))
        return core::Result<void>::failure(error(1519, "Certificate generation refuses to overwrite files"));
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* raw{};
    if (!generator || EVP_PKEY_keygen_init(generator.get()) != 1
        || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) != 1
        || EVP_PKEY_keygen(generator.get(), &raw) != 1)
        return core::Result<void>::failure(error(1529, "Certificate key generation failed"));
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert) return core::Result<void>::failure(error(1529, "Certificate generation failed"));
    X509_set_version(cert.get(), 2); ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), static_cast<long>(lifetime.count()));
    X509_set_pubkey(cert.get(), private_key.get());
    auto* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(dns_name.c_str()), -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
    X509V3_CTX extensions{}; X509V3_set_ctx(&extensions, cert.get(), cert.get(), nullptr, nullptr, 0);
    const auto names = "DNS:" + dns_name + ",IP:127.0.0.1,IP:::1";
    auto* san = X509V3_EXT_conf_nid(nullptr, &extensions, NID_subject_alt_name, const_cast<char*>(names.c_str()));
    if (!san || X509_add_ext(cert.get(), san, -1) != 1 || X509_sign(cert.get(), private_key.get(), EVP_sha256()) <= 0) {
        X509_EXTENSION_free(san); return core::Result<void>::failure(error(1529, "Certificate signing failed"));
    }
    X509_EXTENSION_free(san);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_out(platform::relay_credentials::open_pem(key.c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> cert_out(platform::relay_credentials::open_pem(certificate.c_str(), "w"), BIO_free);
    if (key_out) platform::relay_credentials::restrict_private_key(key);
    if (!key_out || !cert_out || PEM_write_bio_PrivateKey(key_out.get(), private_key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1
        || PEM_write_bio_X509(cert_out.get(), cert.get()) != 1)
        return core::Result<void>::failure(error(1519, "Cannot write relay certificate files"));
    return core::Result<void>::success();
}
core::Result<std::string> random_code() {
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        return core::Result<std::string>::failure(error(1529, "Relay secure random generation failed"));
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : random) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return core::Result<std::string>::success(std::move(result));
}
core::Result<GnsTurnCredentials> turn_credentials(const std::string& uri, const std::string& secret,
        std::chrono::seconds lifetime) {
    if (uri.empty()) return core::Result<GnsTurnCredentials>::success({});
    auto id = random_code();
    if (!id) return core::Result<GnsTurnCredentials>::failure(id.error());
    const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()) + lifetime;
    GnsTurnCredentials result{uri, std::to_string(expiry.count()) + ":" + id.value(), {}, lifetime};
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned size{};
    if (!HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(result.username.data()), result.username.size(), digest.data(), &size)
        || size != 20)
        return core::Result<GnsTurnCredentials>::failure(error(1529, "TURN credential generation failed"));
    std::array<unsigned char, 32> encoded{};
    const int count = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(size));
    result.password.assign(reinterpret_cast<const char*>(encoded.data()), static_cast<std::size_t>(count));
    return core::Result<GnsTurnCredentials>::success(std::move(result));
}
bool secret_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
BIO_METHOD* TlsStream::method() {
    // Function-local initialization is thread safe; the deleter runs only after
    // all endpoint workers are destroyed during ordinary application teardown.
    static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> value([] {
        auto* result = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "EAWR native socket");
        if (!result) return result;
        BIO_meth_set_read(result, bio_read); BIO_meth_set_write(result, bio_write);
        BIO_meth_set_create(result, [](BIO* bio) { BIO_set_init(bio, 1); return 1; });
        BIO_meth_set_destroy(result, [](BIO*) { return 1; });
        BIO_meth_set_ctrl(result, [](BIO*, int cmd, long, void*) -> long { return cmd == BIO_CTRL_FLUSH ? 1 : 0; });
        return result;
    }(), BIO_meth_free);
    return value.get();
}
int TlsStream::bio_read(BIO* bio, char* data, int size) {
    BIO_clear_retry_flags(bio);
    if (size <= 0) return 0;
    auto& stream = *static_cast<TlsStream*>(BIO_get_data(bio));
    const auto io = socket::read(stream.handle_, {reinterpret_cast<std::uint8_t*>(data), static_cast<std::size_t>(size)});
    stream.read_bytes_ += io.count;
    if (io.status == socket::Status::pending) { BIO_set_retry_read(bio); return -1; }
    if (io.status == socket::Status::error) return -1;
    return static_cast<int>(io.count);
}
int TlsStream::bio_write(BIO* bio, const char* data, int size) {
    BIO_clear_retry_flags(bio);
    if (size <= 0) return 0;
    auto& stream = *static_cast<TlsStream*>(BIO_get_data(bio));
    const auto io = socket::write(stream.handle_, {reinterpret_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)});
    if (io.status == socket::Status::pending) { BIO_set_retry_write(bio); return -1; }
    if (io.status != socket::Status::ready) return -1;
    return static_cast<int>(io.count);
}
TlsStream::TlsStream(Context ctx, socket::Handle handle, bool server, const std::string& name, const std::string& pin,
        std::size_t bytes, std::size_t frames)
    : context_(std::move(ctx)), handle_(handle), pin_(pin), max_bytes_(bytes), max_frames_(frames) {
    ssl_ = SSL_new(context_.get());
    auto* bio = method() ? BIO_new(method()) : nullptr;
    if (!ssl_ || !bio) { BIO_free(bio); failed_ = true; return; }
    BIO_set_data(bio, this);
    SSL_set_bio(ssl_, bio, bio);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
    if (server) SSL_set_accept_state(ssl_);
    else {
        SSL_set_connect_state(ssl_);
        auto* params = SSL_get0_param(ssl_);
        X509_VERIFY_PARAM_set_hostflags(params, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        // OpenSSL's IP parser selects SAN IP verification. DNS names use host
        // verification and SNI; neither path disables chain validation.
        if (pin.empty() && X509_VERIFY_PARAM_set1_ip_asc(params, name.c_str()) != 1) {
            if (SSL_set1_host(ssl_, name.c_str()) != 1 || SSL_set_tlsext_host_name(ssl_, name.c_str()) != 1) failed_ = true;
        }
        if (pin.empty() && name.empty()) failed_ = true;
    }
}
TlsStream::~TlsStream() { SSL_free(ssl_); socket::close(handle_); }
int TlsStream::handshake() {
    ERR_clear_error();
    const int result = SSL_do_handshake(ssl_);
    if (result != 1 && pin_.empty()) {
        const auto verification = SSL_get_verify_result(ssl_);
        if (verification != X509_V_OK)
            failure_ = verification == X509_V_ERR_HOSTNAME_MISMATCH || verification == X509_V_ERR_IP_ADDRESS_MISMATCH
                ? error(1741, "Relay certificate name does not match")
                : error(1740, "Relay certificate is not trusted");
    }
    if (result == 1 && !pin_.empty()) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        std::unique_ptr<X509, decltype(&X509_free)> cert(SSL_get1_peer_certificate(ssl_), X509_free);
#else
        std::unique_ptr<X509, decltype(&X509_free)> cert(SSL_get_peer_certificate(ssl_), X509_free);
#endif
        if (!cert || !secret_equal(fingerprint(cert.get()), pin_)
            || X509_cmp_current_time(X509_get0_notBefore(cert.get())) >= 0
            || X509_cmp_current_time(X509_get0_notAfter(cert.get())) <= 0) {
            failed_ = true; return -1;
        }
    }
    return result;
}
int TlsStream::write(std::span<const std::uint8_t> bytes) { ERR_clear_error(); return SSL_write(ssl_, bytes.data(), static_cast<int>(bytes.size())); }
int TlsStream::read(std::span<std::uint8_t> bytes) { ERR_clear_error(); return SSL_read(ssl_, bytes.data(), static_cast<int>(bytes.size())); }
bool TlsStream::retry(int result) {
    const auto code = SSL_get_error(ssl_, result);
    if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) return true;
    failed_ = true; return false;
}
namespace {
// Nonblocking OpenSSL channel over a native socket the caller owns. The BIO writes
// with MSG_NOSIGNAL through the socket layer, as TlsStream does.
class OpenSslChannel final : public TlsChannel {
public:
    OpenSslChannel(Context context, socket::Handle handle) : context_(std::move(context)), handle_(handle), ssl_(SSL_new(context_.get())) {}
    ~OpenSslChannel() override {
        if (ssl_) { SSL_set_quiet_shutdown(ssl_, 0); ERR_clear_error(); SSL_shutdown(ssl_); SSL_free(ssl_); }
    }
    bool open(bool server, const std::string& name) {
        auto* bio = method() ? BIO_new(method()) : nullptr;
        if (!ssl_ || !bio) { BIO_free(bio); return false; }
        BIO_set_data(bio, &handle_);
        SSL_set_bio(ssl_, bio, bio);
        SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
        if (server) { SSL_set_accept_state(ssl_); return true; }
        SSL_set_connect_state(ssl_);
        X509_VERIFY_PARAM_set_hostflags(SSL_get0_param(ssl_), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        return !name.empty() && SSL_set1_host(ssl_, name.c_str()) == 1 && SSL_set_tlsext_host_name(ssl_, name.c_str()) == 1;
    }
    TlsStep handshake() override { ERR_clear_error(); return step(SSL_do_handshake(ssl_)); }
    TlsStep write(std::span<const std::uint8_t> bytes, std::size_t& written) override {
        ERR_clear_error();
        const int result = SSL_write(ssl_, bytes.data(), static_cast<int>(std::min<std::size_t>(bytes.size(), 16384)));
        if (result > 0) { written = static_cast<std::size_t>(result); return TlsStep::done; }
        return step(result);
    }
    TlsStep read(std::span<std::uint8_t> bytes, std::size_t& count) override {
        ERR_clear_error();
        const int result = SSL_read(ssl_, bytes.data(), static_cast<int>(std::min<std::size_t>(bytes.size(), 16384)));
        if (result > 0) { count = static_cast<std::size_t>(result); return TlsStep::done; }
        return step(result);
    }
private:
    Context context_;
    socket::Handle handle_;
    SSL* ssl_;
    TlsStep step(int result) {
        if (result > 0) return TlsStep::done;
        switch (SSL_get_error(ssl_, result)) {
        case SSL_ERROR_WANT_READ: case SSL_ERROR_WANT_WRITE: return TlsStep::wants_io;
        case SSL_ERROR_ZERO_RETURN: return TlsStep::closed;
        case SSL_ERROR_SYSCALL: return result == 0 && ERR_peek_error() == 0 ? TlsStep::closed : TlsStep::failed;
        default: return TlsStep::failed;
        }
    }
    static BIO_METHOD* method() {
        static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> value([] {
            auto* result = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "EAWR channel socket");
            if (!result) return result;
            BIO_meth_set_read(result, [](BIO* bio, char* data, int size) -> int {
                BIO_clear_retry_flags(bio);
                if (size <= 0) return 0;
                const auto io = socket::read(*static_cast<socket::Handle*>(BIO_get_data(bio)), {reinterpret_cast<std::uint8_t*>(data), static_cast<std::size_t>(size)});
                if (io.status == socket::Status::pending) { BIO_set_retry_read(bio); return -1; }
                return io.status == socket::Status::ready ? static_cast<int>(io.count) : (io.status == socket::Status::closed ? 0 : -1);
            });
            BIO_meth_set_write(result, [](BIO* bio, const char* data, int size) -> int {
                BIO_clear_retry_flags(bio);
                if (size <= 0) return 0;
                const auto io = socket::write(*static_cast<socket::Handle*>(BIO_get_data(bio)), {reinterpret_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)});
                if (io.status == socket::Status::pending) { BIO_set_retry_write(bio); return -1; }
                return io.status == socket::Status::ready ? static_cast<int>(io.count) : -1;
            });
            BIO_meth_set_create(result, [](BIO* bio) { BIO_set_init(bio, 1); return 1; });
            BIO_meth_set_destroy(result, [](BIO*) { return 1; });
            BIO_meth_set_ctrl(result, [](BIO*, int cmd, long, void*) -> long { return cmd == BIO_CTRL_FLUSH ? 1 : 0; });
            return result;
        }(), BIO_meth_free);
        return value.get();
    }
};
}
std::unique_ptr<TlsChannel> open_tls_channel(socket::Handle handle, const TlsChannelOptions& options) {
    auto ctx = context(options.server);
    if (!ctx) return {};
    if (options.server) {
        if (platform::relay_credentials::read_certificate_chain(ctx.get(), options.certificate.c_str()) != 1
            || platform::relay_credentials::read_private_key(ctx.get(), options.key.c_str(), SSL_FILETYPE_PEM) != 1
            || SSL_CTX_check_private_key(ctx.get()) != 1) return {};
    } else {
        // Chain and hostname are always verified: the operating system store, or only the supplied bundle.
        if ((options.ca_file.empty() ? SSL_CTX_set_default_verify_paths(ctx.get())
                : platform::relay_credentials::read_trust(ctx.get(), options.ca_file.c_str(), nullptr)) != 1) return {};
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    }
    auto channel = std::make_unique<OpenSslChannel>(std::move(ctx), handle);
    if (!channel->open(options.server, options.server_name)) return {};
    return channel;
}
} // namespace eawr::net::relay
#endif
