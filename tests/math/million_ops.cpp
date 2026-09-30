#include "eawr/sim/math/math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {

using eawr::core::Result;
using eawr::sim::math::Fixed;
using eawr::sim::math::Quat;
using eawr::sim::math::Vec3;

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t size) {
        bit_count_ += static_cast<std::uint64_t>(size) * 8U;
        while (size != 0) {
            const std::size_t count = std::min(size, block_.size() - used_);
            std::copy_n(data, count, block_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += count;
            data += count;
            size -= count;
            if (used_ == block_.size()) {
                compress();
                used_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        block_[used_++] = 0x80U;
        if (used_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.end(),
                      std::uint8_t{0});
            compress();
            used_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.begin() + 56,
                  std::uint8_t{0});
        for (unsigned index = 0; index < 8; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(bit_count_ >> (index * 8U));
        }
        compress();
        std::array<std::uint8_t, 32> output{};
        for (unsigned index = 0; index < state_.size(); ++index) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                output[index * 4U + byte] = static_cast<std::uint8_t>(
                    state_[index] >> ((3U - byte) * 8U));
            }
        }
        return output;
    }

private:
    static constexpr std::array<std::uint32_t, 64> constants_{{
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U,
    }};
    std::array<std::uint32_t, 8> state_{{
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U,
    }};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_{0};
    std::uint64_t bit_count_{0};

    void compress() {
        std::array<std::uint32_t, 64> words{};
        for (unsigned index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block_[index * 4U]) << 24U)
                | (static_cast<std::uint32_t>(block_[index * 4U + 1U]) << 16U)
                | (static_cast<std::uint32_t>(block_[index * 4U + 2U]) << 8U)
                | static_cast<std::uint32_t>(block_[index * 4U + 3U]);
        }
        for (unsigned index = 16; index < 64; ++index) {
            const std::uint32_t a = std::rotr(words[index - 15], 7)
                ^ std::rotr(words[index - 15], 18) ^ (words[index - 15] >> 3U);
            const std::uint32_t b = std::rotr(words[index - 2], 17)
                ^ std::rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + a + words[index - 7] + b;
        }
        auto [a,b,c,d,e,f,g,h] = state_;
        for (unsigned index = 0; index < 64; ++index) {
            const std::uint32_t sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t first = h + sigma1 + choose + constants_[index] + words[index];
            const std::uint32_t sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t second = sigma0 + majority;
            h=g; g=f; f=e; e=d+first; d=c; c=b; b=a; a=first+second;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }
};

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::uint8_t status(const eawr::core::Diagnostic& diagnostic) {
    if (diagnostic.code == eawr::sim::math::diagnostic_codes::overflow) return 1;
    if (diagnostic.code == eawr::sim::math::diagnostic_codes::division_by_zero) return 2;
    return 255;
}

void inject(std::uint64_t index, std::uint8_t& operation,
            std::array<std::int64_t, 8>& value) {
    switch (index & 4095U) {
    case 0: operation=4; value[0]=std::numeric_limits<std::int64_t>::min(); value[1]=-1; break;
    case 1: operation=0; value[0]=std::numeric_limits<std::int64_t>::max(); value[1]=1; break;
    case 2: operation=4; value[1]=0; break;
    case 3: operation=7; value.fill(std::numeric_limits<std::int64_t>::min()); break;
    case 4:
        operation=7;
        value={std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::min(),
               std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max(),
               std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::min(),
               std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()};
        break;
    case 5: operation=3; value[0]=1; value[1]=Fixed::scale/2; break;
    case 6: operation=3; value[0]=3; value[1]=Fixed::scale/2; break;
    case 7: operation=3; value[0]=-3; value[1]=Fixed::scale/2; break;
    default: break;
    }
}

Result<Fixed> evaluate(std::uint8_t operation, const std::array<std::int64_t, 8>& v) {
    switch (operation) {
    case 0: return eawr::sim::math::add(Fixed::from_raw(v[0]), Fixed::from_raw(v[1]));
    case 1: return eawr::sim::math::subtract(Fixed::from_raw(v[0]), Fixed::from_raw(v[1]));
    case 2: return eawr::sim::math::negate(Fixed::from_raw(v[0]));
    case 3: return eawr::sim::math::multiply(Fixed::from_raw(v[0]), Fixed::from_raw(v[1]));
    case 4: return eawr::sim::math::divide(Fixed::from_raw(v[0]), Fixed::from_raw(v[1]));
    case 5: return Fixed::from_ratio(v[0], v[1]);
    case 6:
        return eawr::sim::math::dot(
            Vec3{Fixed::from_raw(v[0]),Fixed::from_raw(v[2]),Fixed::from_raw(v[4])},
            Vec3{Fixed::from_raw(v[1]),Fixed::from_raw(v[3]),Fixed::from_raw(v[5])});
    default:
        return eawr::sim::math::dot(
            Quat{Fixed::from_raw(v[0]),Fixed::from_raw(v[2]),Fixed::from_raw(v[4]),Fixed::from_raw(v[6])},
            Quat{Fixed::from_raw(v[1]),Fixed::from_raw(v[3]),Fixed::from_raw(v[5]),Fixed::from_raw(v[7])});
    }
}

} // namespace

int main(int argc, char** argv) {
    constexpr std::uint64_t count = 1'000'000;
    std::uint64_t state = 0xD1CEB00C5EED0024ULL;
    Sha256 digest;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint8_t operation = static_cast<std::uint8_t>(splitmix64(state) & 7U);
        std::array<std::int64_t, 8> values{};
        for (auto& value : values) {
            value = std::bit_cast<std::int64_t>(splitmix64(state));
        }
        inject(index, operation, values);
        auto result = evaluate(operation, values);
        std::array<std::uint8_t, 10> record{};
        record[0] = operation;
        std::int64_t payload = 0;
        if (result) {
            payload = result.value().raw();
        } else {
            record[1] = status(result.error());
            if (record[1] == 255) {
                std::cerr << "unexpected error code: " << result.error().code << '\n';
                return 2;
            }
        }
        const std::uint64_t bits = std::bit_cast<std::uint64_t>(payload);
        for (unsigned byte = 0; byte < 8; ++byte) {
            record[2U + byte] = static_cast<std::uint8_t>(bits >> (byte * 8U));
        }
        digest.update(record.data(), record.size());
    }
    std::ostringstream text;
    for (const auto byte : digest.finish()) {
        text << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    }
    std::cout << text.str() << '\n';
    std::string expected;
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--expected" && index + 1 < argc) expected = argv[++index];
        else if (argument == "--output" && index + 1 < argc) output_path = argv[++index];
        else {
            std::cerr << "unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }
    if (!output_path.empty()) {
        std::ofstream output(output_path, std::ios::binary);
        if (!output) {
            std::cerr << "could not open digest output\n";
            return 2;
        }
        output << text.str() << '\n';
    }
    if (!expected.empty() && text.str() != expected) {
        std::cerr << "digest mismatch\n";
        return 1;
    }
    return 0;
}
