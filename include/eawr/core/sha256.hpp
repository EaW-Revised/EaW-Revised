#pragma once
#include <array>
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
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes, Sha256Backend backend) noexcept;
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes, Sha256Backend backend);
} // namespace eawr::core
