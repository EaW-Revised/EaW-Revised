#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace eawr::online_test {
// Removes what must never reach the log or the console copy of it: every registered secret
// (invitation code, server address, pin) and anything shaped like an IPv4 or IPv6 address.
[[nodiscard]] std::string sanitize(std::string text, const std::vector<std::string>& secrets);

// Append-only log next to the executable. Every line passes through sanitize().
class Log {
public:
    explicit Log(const std::filesystem::path& path) : stream_(path, std::ios::trunc) {}
    void add_secret(std::string secret) { if (!secret.empty()) secrets_.push_back(std::move(secret)); }
    // Returns the sanitized line so the caller can print the same text.
    std::string line(const std::string& text);
    [[nodiscard]] bool ok() const { return stream_.is_open(); }
private:
    std::ofstream stream_;
    std::vector<std::string> secrets_;
};
}
