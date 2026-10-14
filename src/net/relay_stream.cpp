#include "relay_tls.hpp"

#include <algorithm>
#include <array>

namespace eawr::net::relay {
bool TlsStream::queue(Bytes body) {
    if (failed_ || write_shutdown_ || body.size() < 2 || body.size() > max_body || outbound_.size() >= max_frames_
        || body.size() + 4 > max_bytes_ - used_bytes_) return false;
    Bytes framed; u32(framed, static_cast<std::uint32_t>(body.size()));
    framed.insert(framed.end(), body.begin(), body.end());
    used_bytes_ += framed.size(); outbound_.push_back(std::move(framed));
    return true;
}
bool TlsStream::finish_writes() {
    if (write_shutdown_) return true;
    if (!ready_ || !drained() || !socket::shutdown_write(handle_)) return false;
    write_shutdown_ = true; return true;
}
void TlsStream::pump() {
    if (failed_) return;
    if (!ready_) {
        const auto connected = socket::connected(handle_);
        if (connected == socket::Status::pending) return;
        if (connected != socket::Status::ready) { failed_ = true; return; }
        const int result = handshake();
        if (result != 1) { retry(result); return; }
        ready_ = true;
    }
    for (unsigned i = 0; i < 16 && !outbound_.empty(); ++i) {
        const auto& bytes = outbound_.front();
        const int result = write(std::span(bytes).subspan(write_offset_));
        // A blocked TLS write must be retried with its same buffer before
        // another TLS operation can consume pending record state.
        if (result <= 0) { retry(result); return; }
        write_offset_ += static_cast<std::size_t>(result);
        if (write_offset_ == bytes.size()) {
            used_bytes_ -= bytes.size(); outbound_.pop_front(); write_offset_ = 0;
        }
    }
    for (unsigned i = 0; i < 16 && !failed_ && inbound_.empty(); ++i) {
        const auto count = std::min(read_buffer_.size(), wanted_ - input_.size());
        const int result = read(std::span(read_buffer_).first(count));
        if (result <= 0) { retry(result); break; }
        input_.insert(input_.end(), read_buffer_.begin(), read_buffer_.begin() + result);
        if (input_.size() != wanted_) continue;
        if (reading_header_) {
            const auto body_size = read_u32(input_);
            if (body_size < 2 || body_size > max_body) { failed_ = true; break; }
            wanted_ = body_size; input_.clear(); reading_header_ = false;
        } else {
            inbound_.push_back(std::move(input_)); input_.clear(); wanted_ = 4; reading_header_ = true;
        }
    }
}
std::optional<Bytes> TlsStream::receive() {
    if (inbound_.empty()) return {};
    auto result = std::move(inbound_.front()); inbound_.pop_front(); return result;
}
} // namespace eawr::net::relay
