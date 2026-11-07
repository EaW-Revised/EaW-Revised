#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace eawr::core {
enum class Sha256Backend { automatic, scalar, x86_sha };
struct Sha256CpuFeatures {
    bool sse2{};
    bool ssse3{};
    bool sha{};
};
// Detected once; the SHA backend uses legacy XMM instructions, not AVX/XSAVE.
[[nodiscard]] Sha256CpuFeatures sha256_cpu_features() noexcept;
[[nodiscard]] Sha256Backend sha256_backend_for_features(Sha256CpuFeatures features) noexcept;
// automatic is cached on first use. EAWR_SHA256_BACKEND=scalar forces the portable
// route for a process; an explicit request still checks CPU support before dispatch.
[[nodiscard]] Sha256Backend sha256_backend(Sha256Backend requested = Sha256Backend::automatic) noexcept;
[[nodiscard]] const char* sha256_backend_name(Sha256Backend backend) noexcept;
// Incremental hashing retains one partial block, independent of input size.
// digest()/hex_digest() snapshot the current message; more updates remain valid.
class Sha256Hasher final {
public:
    explicit Sha256Hasher(Sha256Backend backend = Sha256Backend::automatic) noexcept;
    void update(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::array<std::uint8_t, 32> digest() const noexcept;
    [[nodiscard]] std::string hex_digest() const;
private:
    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<std::uint8_t, 64> tail_{};
    std::uint64_t length_{};
    std::size_t tail_size_{};
    Sha256Backend backend_;
};
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes, Sha256Backend backend) noexcept;
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes, Sha256Backend backend);
} // namespace eawr::core
