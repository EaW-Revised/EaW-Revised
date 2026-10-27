#include "eawr/assets/effect.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

// EFX-07: a standalone fuzz entry point, linked into the deterministic contract
// runner too. No SDK/compiler, installed fixture or engine is required.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    const auto result = eawr::assets::read_compiled_effect(
        std::as_bytes(std::span(data, size)), {}, eawr::assets::EffectReadLimits{2U * 1024U * 1024U});
    if (!result && (result.error().code.empty() || !result.error().column))
        std::abort(); // A fuzz contract violation must terminate the C ABI entry.
    return 0;
}

void run_effect_reader_fuzz(const std::span<const std::byte> seed) {
    std::vector<std::uint8_t> bytes; bytes.reserve(seed.size());
    for (const auto byte : seed) bytes.push_back(std::to_integer<std::uint8_t>(byte));
    // Every byte gets a deterministic high-bit and zero mutation; the original
    // seed is restored between calls, including after accepted opaque mutations.
    for (std::size_t i = 0U; i < bytes.size(); ++i) {
        const auto original = bytes[i]; bytes[i] ^= 0x80U;
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size()); bytes[i] = 0U;
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size()); bytes[i] = original;
    }
    std::uint32_t state = 0x5d3a12c7U;
    for (std::size_t sample = 0U; sample < 256U; ++sample) {
        std::vector<std::uint8_t> random(sample);
        for (auto& byte : random) { state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U; byte = static_cast<std::uint8_t>(state); }
        LLVMFuzzerTestOneInput(random.data(), random.size());
    }
}
