#include "relay_tls.hpp"
#include "relay_https.hpp"
#include "eawr/platform/relay_credentials.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <memory>
#include <mutex>
#include <vector>

namespace eawr::net::relay {
namespace {
// The pinned default Mbed TLS configuration has no PSA threading backend.
// Serialize its native calls across endpoint workers (including TLS 1.3's
// global PSA initialization/key state). All socket callbacks are nonblocking;
// game/GNS threads only access the endpoint queues, never this lock.
std::recursive_mutex& tls_mutex() { static std::recursive_mutex mutex; return mutex; }
}
struct TlsContext {
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt intermediates;
    std::size_t root_count{};
    bool system_trust{};
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    TlsContext() {
        std::lock_guard lock(tls_mutex());
        mbedtls_ssl_config_init(&config); mbedtls_x509_crt_init(&certificate); mbedtls_x509_crt_init(&intermediates);
        mbedtls_pk_init(&key); mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&random);
    }
    ~TlsContext() {
        std::lock_guard lock(tls_mutex());
        mbedtls_ssl_config_free(&config); mbedtls_x509_crt_free(&certificate); mbedtls_x509_crt_free(&intermediates);
        mbedtls_pk_free(&key); mbedtls_ctr_drbg_free(&random); mbedtls_entropy_free(&entropy);
    }
    bool setup(bool server) {
        std::lock_guard lock(tls_mutex());
        constexpr unsigned char purpose[] = "EAWR relay TLS";
        if (mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, purpose, sizeof(purpose)) != 0
            || mbedtls_ssl_config_defaults(&config, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        mbedtls_ssl_conf_renegotiation(&config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
        return true;
    }
};
namespace {
// Both account and machine stores are read once by the context-creation caller.
// Relay callers must perform this work on their background I/O worker.
// CA holds cached intermediates, not trust anchors. Never promote them to ROOT.
// No URL retrieval or revocation fetch occurs. Bounds cap enumeration and parsing.
std::size_t load_system_store(mbedtls_x509_crt* chain, const wchar_t* name) {
    constexpr std::size_t max_certificates = 2048, max_bytes = 8 * 1024 * 1024;
    std::size_t visited{}, bytes{}, loaded{};
    for (const DWORD location : {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE}) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
            location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG, name);
        if (!store) continue;
        const CERT_CONTEXT* cert = nullptr;
        while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
            if (++visited > max_certificates || cert->cbCertEncoded > max_bytes - bytes) {
                CertFreeCertificateContext(cert); break;
            }
            bytes += cert->cbCertEncoded;
            if ((cert->dwCertEncodingType & X509_ASN_ENCODING) != 0
                && mbedtls_x509_crt_parse_der(chain, cert->pbCertEncoded, cert->cbCertEncoded) == 0) ++loaded;
        }
        CertCloseStore(store, 0);
        if (visited >= max_certificates || bytes >= max_bytes) break;
    }
    return loaded;
}
// Mbed TLS searches only forward for an untrusted parent. Store enumeration
// has no chain order, so place cached children before their possible issuers.
// This changes search order only; the verifier still checks every signature.
void order_intermediates(mbedtls_x509_crt* chain) {
    if (!chain->raw.p) return;
    std::vector<mbedtls_x509_crt*> nodes;
    for (auto* crt = chain; crt; crt = crt->next) nodes.push_back(crt);
    const auto issuer_of = [](const mbedtls_x509_crt* parent, const mbedtls_x509_crt* child) {
        return parent != child && parent->subject_raw.len == child->issuer_raw.len
            && std::equal(parent->subject_raw.p, parent->subject_raw.p + parent->subject_raw.len, child->issuer_raw.p);
    };
    std::vector<std::size_t> children(nodes.size());
    for (std::size_t parent = 0; parent < nodes.size(); ++parent)
        for (const auto* child : nodes) if (issuer_of(nodes[parent], child)) ++children[parent];
    std::vector<mbedtls_x509_crt*> ordered;
    ordered.reserve(nodes.size());
    std::vector<bool> used(nodes.size());
    for (std::size_t next = 0; next < nodes.size(); ++next) {
        if (children[next] != 0) continue;
        ordered.push_back(nodes[next]); used[next] = true;
    }
    for (std::size_t next = 0; next < ordered.size(); ++next)
        for (std::size_t parent = 0; parent < nodes.size(); ++parent)
            if (!used[parent] && issuer_of(nodes[parent], ordered[next]) && --children[parent] == 0) {
                ordered.push_back(nodes[parent]); used[parent] = true;
            }
    // Cross-signed cycles cannot be topologically ordered. Retain their store
    // order; the verifier's depth bound and ROOT requirement still apply.
    for (std::size_t i = 0; i < nodes.size(); ++i) if (!used[i]) ordered.push_back(nodes[i]);
    // The first node is embedded in TlsContext; preserve its address and move
    // certificate ownership rather than replacing that embedded chain head.
    if (ordered.front() != chain) {
        auto* first = ordered.front();
        std::swap(*chain, *first);
        for (auto*& crt : ordered) if (crt == chain) crt = first;
        ordered.front() = chain;
    }
    for (std::size_t i = 1; i < ordered.size(); ++i) ordered[i - 1]->next = ordered[i];
    ordered.back()->next = nullptr;
}
std::string fingerprint(const mbedtls_x509_crt* certificate) {
    if (!certificate) return {};
    std::array<unsigned char, 32> hash{};
    if (mbedtls_sha256(certificate->raw.p, certificate->raw.len, hash.data(), 0) != 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : hash) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
}
core::Result<Context> server_context(const std::string& certificate, const std::string& key) {
    std::lock_guard lock(tls_mutex());
    auto ctx = std::make_shared<TlsContext>();
    if (!ctx->setup(true) || platform::relay_credentials::read_certificate(&ctx->certificate, certificate.c_str()) != 0
        || platform::relay_credentials::read_private_key(&ctx->key, key.c_str(), nullptr, mbedtls_ctr_drbg_random, &ctx->random) != 0
        || mbedtls_pk_check_pair(&ctx->certificate.pk, &ctx->key, mbedtls_ctr_drbg_random, &ctx->random) != 0
        || mbedtls_ssl_conf_own_cert(&ctx->config, &ctx->certificate, &ctx->key) != 0)
        return core::Result<Context>::failure(error(1519, "Invalid relay TLS certificate or private key"));
    mbedtls_ssl_conf_authmode(&ctx->config, MBEDTLS_SSL_VERIFY_NONE);
    return core::Result<Context>::success(std::move(ctx));
}
core::Result<Context> client_context(const std::string& ca, const std::string& pin) {
    std::lock_guard lock(tls_mutex());
    auto ctx = std::make_shared<TlsContext>();
    if (!ctx->setup(false) || (!pin.empty() && (pin.size() != 64 || !code_valid(pin.substr(0, 32)) || !code_valid(pin.substr(32)))))
        return core::Result<Context>::failure(error(1519, "Cannot configure relay certificate trust"));
    if (pin.empty()) {
        if (ca.empty()) {
            ctx->system_trust = true;
            ctx->root_count = load_system_store(&ctx->certificate, L"ROOT");
            if (ctx->root_count == 0)
                return core::Result<Context>::failure(error(1742, "Relay system certificate trust is unavailable"));
            load_system_store(&ctx->intermediates, L"CA");
            order_intermediates(&ctx->intermediates);
        } else if (platform::relay_credentials::read_certificate(&ctx->certificate, ca.c_str()) != 0)
            return core::Result<Context>::failure(error(1519, "Cannot configure relay certificate trust"));
        mbedtls_ssl_conf_ca_chain(&ctx->config, &ctx->certificate, nullptr);
    }
    // System mode completes chain verification with the cached, untrusted CA
    // certificates below, before handshake() can admit any application bytes.
    mbedtls_ssl_conf_authmode(&ctx->config, !pin.empty() ? MBEDTLS_SSL_VERIFY_NONE
        : ctx->system_trust ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_REQUIRED);
    return core::Result<Context>::success(std::move(ctx));
}
std::size_t system_root_count(const Context& context) { return context ? context->root_count : 0; }
core::Result<std::string> certificate_fingerprint(const std::string& path) {
    std::lock_guard lock(tls_mutex());
    mbedtls_x509_crt cert; mbedtls_x509_crt_init(&cert);
    const int result = platform::relay_credentials::read_certificate(&cert, path.c_str());
    const auto value = result == 0 ? fingerprint(&cert) : std::string{};
    mbedtls_x509_crt_free(&cert);
    if (value.empty()) return core::Result<std::string>::failure(error(1519, "Cannot read relay certificate"));
    return core::Result<std::string>::success(value);
}
core::Result<void> create_certificate(const std::string& certificate, const std::string& key, std::chrono::seconds lifetime, const std::string& dns_name) {
    std::lock_guard lock(tls_mutex());
    if (lifetime < std::chrono::seconds(-59) || lifetime > std::chrono::hours(24 * 365) || !certificate_name_valid(dns_name))
        return core::Result<void>::failure(error(1519, "Invalid certificate lifetime"));
    if (platform::relay_credentials::exists(certificate) || platform::relay_credentials::exists(key))
        return core::Result<void>::failure(error(1519, "Certificate generation refuses to overwrite files"));
    TlsContext ctx;
    mbedtls_x509write_cert cert; mbedtls_x509write_crt_init(&cert);
    mbedtls_mpi serial; mbedtls_mpi_init(&serial);
    struct Cleanup { mbedtls_x509write_cert& cert; mbedtls_mpi& serial;
        ~Cleanup() { mbedtls_x509write_crt_free(&cert); mbedtls_mpi_free(&serial); } } cleanup{cert, serial};
    if (!ctx.setup(true) || mbedtls_pk_setup(&ctx.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0
        || mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(ctx.key), mbedtls_ctr_drbg_random, &ctx.random) != 0
        || mbedtls_mpi_lset(&serial, 1) != 0)
        return core::Result<void>::failure(error(1529, "Certificate key generation failed"));
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &ctx.key); mbedtls_x509write_crt_set_issuer_key(&cert, &ctx.key);
    auto date = [](std::time_t time) {
        std::tm utc{}; gmtime_s(&utc, &time); std::array<char, 16> value{};
        std::strftime(value.data(), value.size(), "%Y%m%d%H%M%S", &utc); return std::string(value.data());
    };
    const auto now = std::time(nullptr);
    const auto before = date(now - 60), after = date(now + lifetime.count());
    std::vector<unsigned char> name(dns_name.begin(), dns_name.end());
    unsigned char loopback[] = {127, 0, 0, 1};
    mbedtls_x509_san_list dns{}, ip{};
    dns.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    dns.node.san.unstructured_name.p = name.data(); dns.node.san.unstructured_name.len = name.size(); dns.next = &ip;
    ip.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    ip.node.san.unstructured_name.p = loopback; ip.node.san.unstructured_name.len = sizeof(loopback);
    if (mbedtls_x509write_crt_set_serial(&cert, &serial) != 0
        || mbedtls_x509write_crt_set_subject_name(&cert, ("CN=" + dns_name).c_str()) != 0
        || mbedtls_x509write_crt_set_issuer_name(&cert, ("CN=" + dns_name).c_str()) != 0
        || mbedtls_x509write_crt_set_subject_alternative_name(&cert, &dns) != 0
        || mbedtls_x509write_crt_set_validity(&cert, before.c_str(), after.c_str()) != 0)
        return core::Result<void>::failure(error(1529, "Certificate generation failed"));
    std::array<unsigned char, 4096> cert_pem{}, key_pem{};
    if (mbedtls_x509write_crt_pem(&cert, cert_pem.data(), cert_pem.size(), mbedtls_ctr_drbg_random, &ctx.random) != 0
        || mbedtls_pk_write_key_pem(&ctx.key, key_pem.data(), key_pem.size()) != 0)
        return core::Result<void>::failure(error(1529, "Certificate signing failed"));
    if (!platform::relay_credentials::write_private_key(key, key_pem.data())) return core::Result<void>::failure(error(1519, "Cannot securely create relay private key"));
    if (!platform::relay_credentials::write_certificate(certificate, cert_pem.data())) return core::Result<void>::failure(error(1519, "Cannot write relay certificate files"));
    return core::Result<void>::success();
}
core::Result<std::string> random_code() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return core::Result<std::string>::failure(error(1529, "Relay secure random generation failed"));
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return core::Result<std::string>::success(std::move(result));
}
core::Result<GnsTurnCredentials> turn_credentials(const std::string& uri, const std::string& secret,
        std::chrono::seconds lifetime) {
    std::lock_guard lock(tls_mutex());
    if (uri.empty()) return core::Result<GnsTurnCredentials>::success({});
    auto id = random_code(); if (!id) return core::Result<GnsTurnCredentials>::failure(id.error());
    const auto expiry = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()) + lifetime;
    GnsTurnCredentials result{uri, std::to_string(expiry.count()) + ":" + id.value(), {}, lifetime};
    std::array<unsigned char, 20> hash{};
    std::array<unsigned char, 32> base64{};
    std::size_t count{};
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
            reinterpret_cast<const unsigned char*>(secret.data()), secret.size(),
            reinterpret_cast<const unsigned char*>(result.username.data()), result.username.size(), hash.data()) != 0
        || mbedtls_base64_encode(base64.data(), base64.size(), &count, hash.data(), hash.size()) != 0)
        return core::Result<GnsTurnCredentials>::failure(error(1529, "TURN credential generation failed"));
    result.password.assign(reinterpret_cast<const char*>(base64.data()), count);
    return core::Result<GnsTurnCredentials>::success(std::move(result));
}
bool secret_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    volatile unsigned difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) difference = difference | static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}
struct TlsStream::Native {
    mbedtls_ssl_context ssl;
    TlsStream* owner;
    std::string name;
    explicit Native(TlsStream* stream) : owner(stream) { std::lock_guard lock(tls_mutex()); mbedtls_ssl_init(&ssl); }
    ~Native() { std::lock_guard lock(tls_mutex()); mbedtls_ssl_free(&ssl); }
    static int read(void* pointer, unsigned char* data, std::size_t size) {
        auto& stream = *static_cast<Native*>(pointer)->owner;
        const auto io = socket::read(stream.handle_, {data, size});
        stream.read_bytes_ += io.count;
        if (io.status == socket::Status::pending) return MBEDTLS_ERR_SSL_WANT_READ;
        if (io.status == socket::Status::error) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        return static_cast<int>(io.count);
    }
    static int write(void* pointer, const unsigned char* data, std::size_t size) {
        auto& stream = *static_cast<Native*>(pointer)->owner;
        const auto io = socket::write(stream.handle_, {data, size});
        if (io.status == socket::Status::pending) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (io.status != socket::Status::ready) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        return static_cast<int>(io.count);
    }
};
TlsStream::TlsStream(Context ctx, socket::Handle handle, bool server, const std::string& name, const std::string& pin,
        std::size_t bytes, std::size_t frames)
    : native_(std::make_unique<Native>(this)), context_(std::move(ctx)), handle_(handle), pin_(pin),
      max_bytes_(bytes), max_frames_(frames) {
    std::lock_guard lock(tls_mutex());
    if (mbedtls_ssl_setup(&native_->ssl, &context_->config) != 0) failed_ = true;
    if (!server && pin.empty() && (name.empty() || mbedtls_ssl_set_hostname(&native_->ssl, name.c_str()) != 0)) failed_ = true;
    native_->name = name;
    mbedtls_ssl_set_bio(&native_->ssl, native_.get(), Native::write, Native::read, nullptr);
}
TlsStream::~TlsStream() { std::lock_guard lock(tls_mutex()); native_.reset(); socket::close(handle_); }
int TlsStream::handshake() {
    std::lock_guard lock(tls_mutex());
    const int result = mbedtls_ssl_handshake(&native_->ssl);
    auto reject_verification = [this](std::uint32_t flags) {
        failure_ = flags & MBEDTLS_X509_BADCERT_CN_MISMATCH
            ? error(1741, "Relay certificate name does not match")
            : error(1740, "Relay certificate is not trusted");
        failed_ = true;
    };
    if (result != 0) {
        if (pin_.empty() && result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
            reject_verification(mbedtls_ssl_get_verify_result(&native_->ssl));
        return result;
    }
    if (context_->system_trust) {
        // Build an untrusted chain from the peer and the cached intermediates;
        // only ROOT is passed as trust_ca. This also checks name and dates.
        mbedtls_x509_crt chain; mbedtls_x509_crt_init(&chain);
        struct Cleanup { mbedtls_x509_crt& chain; ~Cleanup() { mbedtls_x509_crt_free(&chain); } } cleanup{chain};
        std::size_t count{}, bytes{};
        const auto append = [&](const mbedtls_x509_crt* crt) {
            for (; crt && crt->raw.p; crt = crt->next) {
                if (++count > 4096 || crt->raw.len > 16 * 1024 * 1024 - bytes) return false;
                bytes += crt->raw.len;
                if (mbedtls_x509_crt_parse_der(&chain, crt->raw.p, crt->raw.len) != 0) return false;
            }
            return true;
        };
        const auto* peer = mbedtls_ssl_get_peer_cert(&native_->ssl);
        const bool parsed = peer && append(peer) && append(&context_->intermediates);
        std::uint32_t flags{};
        const bool verified = parsed && mbedtls_x509_crt_verify(&chain, &context_->certificate, nullptr,
            native_->name.c_str(), &flags, nullptr, nullptr) == 0;
        // The SSL layer checks TLS-specific key usage in addition to X.509
        // chain verification; cached intermediates must not waive those checks.
        flags |= mbedtls_ssl_get_verify_result(&native_->ssl)
            & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE);
        if (!verified || flags != 0) {
            reject_verification(flags); return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
        }
    }
    if (!pin_.empty()) {
        const auto* cert = mbedtls_ssl_get_peer_cert(&native_->ssl);
        if (!cert || !secret_equal(fingerprint(cert), pin_)
            || mbedtls_x509_time_is_future(&cert->valid_from) || mbedtls_x509_time_is_past(&cert->valid_to)) {
            failed_ = true; return MBEDTLS_ERR_SSL_BAD_CERTIFICATE;
        }
    }
    return 1;
}
int TlsStream::write(std::span<const std::uint8_t> bytes) { std::lock_guard lock(tls_mutex()); return mbedtls_ssl_write(&native_->ssl, bytes.data(), bytes.size()); }
int TlsStream::read(std::span<std::uint8_t> bytes) { std::lock_guard lock(tls_mutex()); return mbedtls_ssl_read(&native_->ssl, bytes.data(), bytes.size()); }
bool TlsStream::retry(int result) {
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
    failed_ = true; return false;
}
namespace {
// Nonblocking Mbed TLS channel over a native socket the caller owns.
class MbedChannel final : public TlsChannel {
public:
    MbedChannel(Context context, socket::Handle handle) : context_(std::move(context)), handle_(handle) {
        std::lock_guard lock(tls_mutex());
        mbedtls_ssl_init(&ssl_);
    }
    ~MbedChannel() override {
        std::lock_guard lock(tls_mutex());
        mbedtls_ssl_close_notify(&ssl_);
        mbedtls_ssl_free(&ssl_);
    }
    bool open(bool server, const std::string& name) {
        std::lock_guard lock(tls_mutex());
        if (mbedtls_ssl_setup(&ssl_, &context_->config) != 0) return false;
        if (!server && (name.empty() || mbedtls_ssl_set_hostname(&ssl_, name.c_str()) != 0)) return false;
        mbedtls_ssl_set_bio(&ssl_, this, send, receive, nullptr);
        return true;
    }
    TlsStep handshake() override {
        std::lock_guard lock(tls_mutex());
        return step(mbedtls_ssl_handshake(&ssl_));
    }
    TlsStep write(std::span<const std::uint8_t> bytes, std::size_t& written) override {
        std::lock_guard lock(tls_mutex());
        const int result = mbedtls_ssl_write(&ssl_, bytes.data(), std::min<std::size_t>(bytes.size(), 16384));
        if (result > 0) { written = static_cast<std::size_t>(result); return TlsStep::done; }
        return step(result);
    }
    TlsStep read(std::span<std::uint8_t> bytes, std::size_t& count) override {
        std::lock_guard lock(tls_mutex());
        const int result = mbedtls_ssl_read(&ssl_, bytes.data(), std::min<std::size_t>(bytes.size(), 16384));
        if (result > 0) { count = static_cast<std::size_t>(result); return TlsStep::done; }
        return step(result);
    }
private:
    Context context_;
    socket::Handle handle_;
    mbedtls_ssl_context ssl_;
    static TlsStep step(int result) {
        if (result >= 0) return TlsStep::done;
        // TLS 1.3 servers send tickets after the handshake; they carry no data.
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE
            || result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET || result == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS) return TlsStep::wants_io;
        if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == MBEDTLS_ERR_SSL_CONN_EOF)
            return TlsStep::closed;
        return TlsStep::failed;
    }
    static int send(void* self, const unsigned char* data, std::size_t size) {
        const auto io = socket::write(static_cast<MbedChannel*>(self)->handle_, {data, size});
        if (io.status == socket::Status::pending) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return io.status == socket::Status::ready ? static_cast<int>(io.count) : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    static int receive(void* self, unsigned char* data, std::size_t size) {
        const auto io = socket::read(static_cast<MbedChannel*>(self)->handle_, {data, size});
        if (io.status == socket::Status::pending) return MBEDTLS_ERR_SSL_WANT_READ;
        if (io.status == socket::Status::closed) return 0;
        return io.status == socket::Status::ready ? static_cast<int>(io.count) : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
};
}
std::unique_ptr<TlsChannel> open_tls_channel(socket::Handle handle, const TlsChannelOptions& options) {
    Context ctx;
    if (options.server) {
        auto server = server_context(options.certificate, options.key);
        if (!server) return {};
        ctx = std::move(server).value();
    } else if (!options.ca_file.empty()) {
        auto client = client_context(options.ca_file, {});
        if (!client) return {};
        ctx = std::move(client).value();
    } else {
        std::lock_guard lock(tls_mutex());
        ctx = std::make_shared<TlsContext>();
        if (!ctx->setup(false) || (ctx->root_count = load_system_store(&ctx->certificate, L"ROOT")) == 0) return {};
        mbedtls_ssl_conf_authmode(&ctx->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&ctx->config, &ctx->certificate, nullptr);
    }
    auto channel = std::make_unique<MbedChannel>(std::move(ctx), handle);
    if (!channel->open(options.server, options.server_name)) return {};
    return channel;
}
} // namespace eawr::net::relay
#endif
