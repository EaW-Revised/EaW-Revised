#pragma once

#include "relay_protocol.hpp"
#include "platform_socket.hpp"

#ifndef _WIN32
#include <openssl/ssl.h>
#endif

#include <chrono>
#include <deque>
#include <array>
#include <memory>

namespace eawr::net::relay {
#ifdef _WIN32
struct TlsContext;
using Context = std::shared_ptr<TlsContext>;
#else
using Context = std::shared_ptr<SSL_CTX>;
#endif
core::Result<Context> server_context(const std::string& certificate, const std::string& key);
// Windows relay system trust supplements ROOT with cached, untrusted CA
// intermediates. The HTTPS channel instead requires a complete server chain
// against ROOT only. System relay contexts must be used through TlsStream:
// its handshake completes mandatory verification before admitting application data.
core::Result<Context> client_context(const std::string& ca, const std::string& pin);
#ifdef _WIN32
// Private contract seam: successfully parsed ROOT anchors in a system context.
std::size_t system_root_count(const Context& context);
#endif
// Private generator: a short negative lifetime produces an already expired
// certificate for validity contracts; application callers use the default.
// `dns_name` lets contracts issue a trust anchor for a service name (letters, digits, dots, hyphens).
core::Result<void> create_certificate(const std::string& certificate, const std::string& key,
    std::chrono::seconds lifetime = std::chrono::hours(24 * 365), const std::string& dns_name = "localhost");
core::Result<std::string> certificate_fingerprint(const std::string& certificate);
core::Result<std::string> random_code();
core::Result<GnsTurnCredentials> turn_credentials(const std::string& uri, const std::string& secret,
    std::chrono::seconds lifetime);
bool secret_equal(std::string_view a, std::string_view b);

class TlsStream {
public:
    TlsStream(Context context, socket::Handle handle, bool server, const std::string& name, const std::string& pin,
        std::size_t bytes, std::size_t frames);
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    bool queue(Bytes body);
    void pump();
    std::optional<Bytes> receive();
    bool failed() const { return failed_; }
    bool ready() const { return ready_; }
    std::optional<core::Diagnostic> failure() const { return failure_; }
    bool drained() const { return outbound_.empty(); }
    bool finish_writes(); // half-close only after queued application frames drain
    std::size_t take_read_bytes() { const auto result = read_bytes_; read_bytes_ = 0; return result; }
private:
#ifdef _WIN32
    struct Native;
    std::unique_ptr<Native> native_;
#else
    static BIO_METHOD* method();
    static int bio_read(BIO* bio, char* data, int size);
    static int bio_write(BIO* bio, const char* data, int size);
#endif
    Context context_;
    socket::Handle handle_;
#ifndef _WIN32
    SSL* ssl_{};
#endif
    std::string pin_;
    std::optional<core::Diagnostic> failure_;
    bool ready_{}, failed_{}, reading_header_{true}, write_shutdown_{};
    std::size_t max_bytes_{}, max_frames_{}, used_bytes_{}, read_bytes_{}, write_offset_{};
    std::deque<Bytes> outbound_, inbound_;
    Bytes input_;
    std::size_t wanted_{4};
    std::array<std::uint8_t, 16384> read_buffer_{};
    bool retry(int result);
    int handshake();
    int write(std::span<const std::uint8_t> bytes);
    int read(std::span<std::uint8_t> bytes);
};
} // namespace eawr::net::relay
