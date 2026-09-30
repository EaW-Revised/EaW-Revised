#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <sstream>

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

void sha_transform(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
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

} // namespace

std::array<std::uint8_t, 32> sha256(const std::span<const std::uint8_t> bytes) noexcept {
    std::array<std::uint32_t, 8> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::size_t offset = 0;
    while (bytes.size() - offset >= 64) {
        sha_transform(state, bytes.data() + offset);
        offset += 64;
    }
    std::array<std::uint8_t, 128> tail{};
    const auto remainder = bytes.size() - offset;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), remainder, tail.begin());
    tail[remainder] = 0x80U;
    const std::size_t padded_size = remainder < 56 ? 64U : 128U;
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (unsigned index = 0; index < 8; ++index) {
        tail[padded_size - 1U - index] = static_cast<std::uint8_t>(bit_length >> (8U * index));
    }
    sha_transform(state, tail.data());
    if (padded_size == 128U) {
        sha_transform(state, tail.data() + 64);
    }
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
    const auto digest = sha256(bytes);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

} // namespace eawr::core
