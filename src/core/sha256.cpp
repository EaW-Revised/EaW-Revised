#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>

// Only the qualified x86-64 route is enabled. Other targets, including ARM,
// keep the portable compressor until native runtime validation is available.
#if (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC) \
    && (defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__))
#define EAWR_X86_SHA 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace eawr::core {
namespace {
[[nodiscard]] std::uint32_t rotr(const std::uint32_t value, const unsigned bits) noexcept {
    return std::rotr(value, static_cast<int>(bits));
}

constexpr std::array<std::uint32_t, 64> sha_k{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

void sha_transform_scalar(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto base = index * 4;
        words[index] = (static_cast<std::uint32_t>(block[base]) << 24U)
            | (static_cast<std::uint32_t>(block[base + 1]) << 16U)
            | (static_cast<std::uint32_t>(block[base + 2]) << 8U)
            | static_cast<std::uint32_t>(block[base + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const auto s0 = rotr(words[index - 15], 7) ^ rotr(words[index - 15], 18)
            ^ (words[index - 15] >> 3U);
        const auto s1 = rotr(words[index - 2], 17) ^ rotr(words[index - 2], 19)
            ^ (words[index - 2] >> 10U);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < 64; ++index) {
        const auto sigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto temporary1 = h + sigma1 + choose + sha_k[index] + words[index];
        const auto sigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

using Compressor = void (*)(std::array<std::uint32_t, 8>&, const std::uint8_t*, std::size_t) noexcept;

void compress_scalar(std::array<std::uint32_t, 8>& state, const std::uint8_t* bytes, const std::size_t blocks) noexcept {
    for (std::size_t block = 0; block < blocks; ++block) sha_transform_scalar(state, bytes + block * 64);
}

#if defined(EAWR_X86_SHA)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sha,ssse3"), noinline))
#else
__declspec(noinline)
#endif
void compress_x86_sha(std::array<std::uint32_t, 8>& state, const std::uint8_t* bytes, const std::size_t blocks) noexcept {
    // FIPS 180-4 section 6.2; Intel SHA instruction definitions, tables 3 and
    // figures 4/5. Lanes are low-to-high F,E,B,A and H,G,D,C respectively.
    const auto as_int = [](const std::uint32_t word) { return std::bit_cast<int>(word); };
    auto abef = _mm_setr_epi32(as_int(state[5]), as_int(state[4]), as_int(state[1]), as_int(state[0]));
    auto cdgh = _mm_setr_epi32(as_int(state[7]), as_int(state[6]), as_int(state[3]), as_int(state[2]));
    const auto endian = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (std::size_t block = 0; block < blocks; ++block) {
        const auto saved_abef = abef;
        const auto saved_cdgh = cdgh;
        __m128i schedule[4];
        for (std::size_t group = 0; group < 4; ++group) {
            schedule[group] = _mm_shuffle_epi8(_mm_loadu_si128(
                reinterpret_cast<const __m128i*>(bytes + block * 64 + group * 16)), endian);
        }
        for (std::size_t group = 0; group < 16; ++group) {
            const auto slot = group & 3U;
            if (group >= 4) {
                // W[t-16] + sigma0(W[t-15]), then W[t-7], then sigma1(W[t-2]).
                const auto partial = _mm_sha256msg1_epu32(schedule[slot], schedule[(slot + 1) & 3U]);
                const auto minus_seven = _mm_alignr_epi8(schedule[(slot + 3) & 3U], schedule[(slot + 2) & 3U], 4);
                schedule[slot] = _mm_sha256msg2_epu32(_mm_add_epi32(partial, minus_seven), schedule[(slot + 3) & 3U]);
            }
            const auto wk = _mm_add_epi32(schedule[slot], _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(sha_k.data() + group * 4)));
            cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
            abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0x0e));
        }
        abef = _mm_add_epi32(abef, saved_abef);
        cdgh = _mm_add_epi32(cdgh, saved_cdgh);
    }
    std::array<std::uint32_t, 4> first{}, second{};
    _mm_storeu_si128(reinterpret_cast<__m128i*>(first.data()), abef);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(second.data()), cdgh);
    state = {first[3], first[2], second[3], second[2], first[1], first[0], second[1], second[0]};
}
#endif

Sha256CpuFeatures detect_features() noexcept {
    Sha256CpuFeatures features;
#if defined(EAWR_X86_SHA)
    unsigned maximum{}, ecx{}, edx{}, ebx{};
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuidex(registers, 0, 0);
    maximum = static_cast<unsigned>(registers[0]);
    if (maximum >= 1) {
        __cpuidex(registers, 1, 0);
        ecx = static_cast<unsigned>(registers[2]); edx = static_cast<unsigned>(registers[3]);
    }
    if (maximum >= 7) {
        __cpuidex(registers, 7, 0);
        ebx = static_cast<unsigned>(registers[1]);
    }
#else
    maximum = __get_cpuid_max(0, nullptr);
    unsigned ignored_a{}, ignored_b{}, ignored_c{}, ignored_d{};
    if (maximum >= 1) __cpuid_count(1, 0, ignored_a, ignored_b, ecx, edx);
    if (maximum >= 7) __cpuid_count(7, 0, ignored_a, ebx, ignored_c, ignored_d);
    (void)ignored_a; (void)ignored_b; (void)ignored_c; (void)ignored_d;
#endif
    // x86-64's OS ABI guarantees XMM state preservation. No AVX state is used.
    features.sse2 = (edx & (1U << 26U)) != 0;
    features.ssse3 = (ecx & (1U << 9U)) != 0;
    features.sha = (ebx & (1U << 29U)) != 0;
#endif
    return features;
}

Compressor compressor(const Sha256Backend backend) noexcept {
#if defined(EAWR_X86_SHA)
    if (backend == Sha256Backend::x86_sha) return compress_x86_sha;
#else
    (void)backend;
#endif
    return compress_scalar;
}

} // namespace

Sha256CpuFeatures sha256_cpu_features() noexcept {
    static const auto features = detect_features();
    return features;
}

Sha256Backend sha256_backend_for_features(const Sha256CpuFeatures features) noexcept {
#if defined(EAWR_X86_SHA)
    if (features.sse2 && features.ssse3 && features.sha) return Sha256Backend::x86_sha;
#else
    (void)features;
#endif
    return Sha256Backend::scalar;
}

Sha256Backend sha256_backend(const Sha256Backend requested) noexcept {
    if (requested == Sha256Backend::scalar) return Sha256Backend::scalar;
    if (requested == Sha256Backend::x86_sha) return sha256_backend_for_features(sha256_cpu_features());
    static const auto selected = [] {
#if defined(_MSC_VER)
        char override_buffer[16]{};
        std::size_t length{};
        const auto* override_value = getenv_s(&length, override_buffer, sizeof(override_buffer), "EAWR_SHA256_BACKEND") == 0
            ? override_buffer : nullptr;
#else
        const auto* override_value = std::getenv("EAWR_SHA256_BACKEND");
#endif
        if (override_value && std::string_view(override_value) == "scalar") return Sha256Backend::scalar;
        return sha256_backend_for_features(sha256_cpu_features());
    }();
    return selected;
}

const char* sha256_backend_name(const Sha256Backend backend) noexcept {
    switch (backend) {
    case Sha256Backend::automatic: return "automatic";
    case Sha256Backend::scalar: return "scalar";
    case Sha256Backend::x86_sha: return "x86-sha";
    }
    return "scalar";
}

std::array<std::uint8_t, 32> sha256(const std::span<const std::uint8_t> bytes) noexcept {
    return sha256(bytes, Sha256Backend::automatic);
}

std::array<std::uint8_t, 32> sha256(const std::span<const std::uint8_t> bytes, const Sha256Backend backend) noexcept {
    const auto compress = compressor(sha256_backend(backend));
    std::array<std::uint32_t, 8> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    const auto blocks = bytes.size() / 64;
    if (blocks != 0) compress(state, bytes.data(), blocks);
    const auto remainder = bytes.size() % 64;
    const auto offset = bytes.size() - remainder;
    std::array<std::uint8_t, 128> tail{};
    if (remainder != 0) std::copy_n(bytes.data() + offset, remainder, tail.begin());
    tail[remainder] = 0x80U;
    const std::size_t padded_size = remainder < 56 ? 64U : 128U;
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (unsigned index = 0; index < 8; ++index) {
        tail[padded_size - 1U - index] = static_cast<std::uint8_t>(bit_length >> (8U * index));
    }
    compress(state, tail.data(), padded_size / 64);
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4] = static_cast<std::uint8_t>(state[index] >> 24U);
        digest[index * 4 + 1] = static_cast<std::uint8_t>(state[index] >> 16U);
        digest[index * 4 + 2] = static_cast<std::uint8_t>(state[index] >> 8U);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(state[index]);
    }
    return digest;
}

std::string sha256_hex(const std::span<const std::uint8_t> bytes) {
    return sha256_hex(bytes, Sha256Backend::automatic);
}

std::string sha256_hex(const std::span<const std::uint8_t> bytes, const Sha256Backend backend) {
    const auto digest = sha256(bytes, backend);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

} // namespace eawr::core
