#include "log.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace eawr::online_test {
namespace {
bool digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// Length of a dotted quad starting at `at`, or 0.
std::size_t ipv4_length(const std::string& text, std::size_t at) {
    std::size_t cursor = at;
    for (int part = 0; part < 4; ++part) {
        std::size_t digits{};
        while (cursor < text.size() && digit(text[cursor]) && digits < 4) { ++cursor; ++digits; }
        if (digits == 0 || digits > 3) return 0;
        if (part < 3) { if (cursor >= text.size() || text[cursor] != '.') return 0; ++cursor; }
    }
    if (cursor < text.size() && text[cursor] == ':') { // optional :port
        std::size_t end = cursor + 1;
        while (end < text.size() && digit(text[end])) ++end;
        if (end > cursor + 1) cursor = end;
    }
    return cursor - at;
}
// Hex groups separated by ':' with at least two separators, e.g. 2001:db8::1 or fe80::1%3.
std::size_t ipv6_length(const std::string& text, std::size_t at) {
    std::size_t cursor = at, colons{}, groups{};
    while (cursor < text.size() && (hex(text[cursor]) || text[cursor] == ':')) {
        if (text[cursor] == ':') ++colons; else ++groups;
        ++cursor;
    }
    return colons >= 2 && groups >= 1 ? cursor - at : 0;
}
} // namespace

std::string sanitize(std::string text, const std::vector<std::string>& secrets) {
    constexpr std::string_view redacted = "<redacted>";
    const auto token = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    for (const auto& secret : secrets) {
        if (secret.empty()) continue;
        std::size_t at = 0;
        while ((at = text.find(secret, at)) != std::string::npos) {
            const auto end = at + secret.size();
            // Invitation codes and credential tokens can occur inside longer SHA-256 values.
            if ((at == 0 || !token(static_cast<unsigned char>(text[at - 1])))
                && (end == text.size() || !token(static_cast<unsigned char>(text[end])))) {
                text.replace(at, secret.size(), redacted);
                at += redacted.size();
            } else at = end;
        }
    }
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const bool boundary = i == 0 || !(std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '.');
        if (boundary && digit(text[i])) {
            if (const auto length = ipv4_length(text, i)) { out += "<address>"; i += length; continue; }
        }
        if (boundary && (hex(text[i]) || text[i] == ':')) {
            if (const auto length = ipv6_length(text, i)) { out += "<address>"; i += length; continue; }
        }
        out += text[i++];
    }
    return out;
}

std::string Log::line(const std::string& text) {
    const auto clean = sanitize(text, secrets_);
    if (stream_.is_open()) {
        const auto now = std::time(nullptr);
        std::tm parts{};
#ifdef _WIN32
        gmtime_s(&parts, &now);
#else
        gmtime_r(&now, &parts);
#endif
        std::ostringstream stamp;
        stamp << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
        stream_ << stamp.str() << ' ' << clean << std::endl;
    }
    return clean;
}
}
