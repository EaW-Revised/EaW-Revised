#include "../support/env.hpp"
#include "../support/check.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool condition, const char* message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

void check(const std::span<const std::uint8_t> bytes, const std::string_view expected) {
    using namespace eawr::core;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto backend : {Sha256Backend::scalar, Sha256Backend::x86_sha, Sha256Backend::automatic}) {
        const auto binary = sha256(bytes, backend);
        std::string rendered;
        for (const auto byte : binary) { rendered += digits[byte >> 4U]; rendered += digits[byte & 15U]; }
        expect(rendered == expected, "binary SHA matches independent vector");
        expect(sha256_hex(bytes, backend) == expected, "hex SHA matches independent vector");
    }
    expect(sha256(bytes) == sha256(bytes, Sha256Backend::scalar), "default binary route equals fallback");
    expect(sha256_hex(bytes) == expected, "default hex route matches vector");
}

std::string table_hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output(bytes.size() * 2, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        output[i * 2] = digits[bytes[i] >> 4U];
        output[i * 2 + 1] = digits[bytes[i] & 15U];
    }
    return output;
}
std::string stream_hex(const std::array<std::uint8_t, 32>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}
// Explicit diagnostic mode only. No timing assertion: the pool is shared.
int hex_benchmark() {
    using namespace eawr::core;
    constexpr std::size_t cases = 64, rounds = 32, iterations = 256;
    std::array<std::vector<std::uint8_t>, cases> inputs;
    std::array<std::array<std::uint8_t, 32>, cases> digests{};
    std::array<std::string, cases> expected;
    for (std::size_t i = 0; i < cases; ++i) {
        inputs[i].resize(64 + i * 7);
        for (std::size_t at = 0; at < inputs[i].size(); ++at)
            inputs[i][at] = static_cast<std::uint8_t>(i * 19 + at * 37);
        digests[i] = sha256(inputs[i]);
        expected[i] = sha256_hex(inputs[i]);
        expect(table_hex(digests[i]) == expected[i], "table hex preserves every digest byte");
        expect(stream_hex(digests[i]) == expected[i], "isolated stream matches production hex");
    }
    if (failures) return 1;
    constexpr std::array<std::string_view, 4> names{
        "sha256_hex", "sha256_plus_stream", "stream_only", "table_only"};
    std::array<std::vector<double>, names.size()> batches;
    std::array<std::uint64_t, names.size()> checksums{};
    bool identical = true;
    // Rotate arm order and input order across batches. Formatting-only arms
    // consume exactly the same precomputed 32 bytes; full arms hash the input.
    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t step = 0; step < names.size(); ++step) {
            const auto arm = (round + step) % names.size();
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                const auto which = (iteration * 17 + round) % cases;
                std::string output;
                switch (arm) {
                case 0: output = sha256_hex(inputs[which]); break;
                case 1: output = stream_hex(sha256(inputs[which])); break;
                case 2: output = stream_hex(digests[which]); break;
                default: output = table_hex(digests[which]); break;
                }
                // Identical equality/checksum consumption keeps all output
                // bytes observable in every arm, including the fast formatter.
                identical &= output == expected[which];
                for (const unsigned char byte : output) checksums[arm] += byte;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count();
            batches[arm].push_back(static_cast<double>(elapsed) / static_cast<double>(iterations));
        }
    }
    expect(identical, "every timed SHA and formatting output is byte-identical");
    expect(std::all_of(checksums.begin(), checksums.end(), [&](auto value) { return value == checksums.front(); }),
        "all benchmark arms consume identical output bytes");
    std::cout << "SHA256-HEX-MICROBENCH {\"backend\":\"" << sha256_backend_name(sha256_backend())
        << "\",\"cases\":" << cases << ",\"batches\":" << rounds << ",\"iterations_per_batch\":" << iterations
        << ",\"byte_identical\":" << (identical && !failures ? "true" : "false")
        << ",\"timing_includes_equality_and_checksum\":true,\"arms\":{" << std::setprecision(12);
    for (std::size_t arm = 0; arm < names.size(); ++arm) {
        double total{};
        for (const auto value : batches[arm]) total += value;
        std::sort(batches[arm].begin(), batches[arm].end());
        const auto p95 = batches[arm][(rounds * 95 + 99) / 100 - 1];
        std::cout << (arm ? "," : "") << '\"' << names[arm] << "\":{\"mean_ns_per_call\":"
            << total / static_cast<double>(rounds) << ",\"p95_batch_mean_ns\":" << p95 << '}';
    }
    std::cout << "}}\n";
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--hex-benchmark") return hex_benchmark();
    using namespace eawr::core;
    const auto features = sha256_cpu_features();
    std::cout << "backend=" << sha256_backend_name(sha256_backend())
        << " explicit-x86=" << sha256_backend_name(sha256_backend(Sha256Backend::x86_sha))
        << " sse2=" << features.sse2 << " ssse3=" << features.ssse3 << " sha=" << features.sha << '\n';
    expect(sha256_backend(Sha256Backend::scalar) == Sha256Backend::scalar, "forced fallback stays scalar");
    for (unsigned bits = 0; bits < 7; ++bits) {
        expect(sha256_backend_for_features({(bits & 1U) != 0, (bits & 2U) != 0, (bits & 4U) != 0})
            == Sha256Backend::scalar, "missing any required feature selects fallback");
    }
    expect(sha256_backend(Sha256Backend::x86_sha) == sha256_backend_for_features(features),
        "explicit acceleration is guarded by actual features");
    const auto forced_value = eawr::testing::environment("EAWR_SHA256_BACKEND");
    const auto* forced = forced_value ? forced_value->c_str() : nullptr;
    expect(sha256_backend() == (forced && std::string_view(forced) == "scalar"
        ? Sha256Backend::scalar : sha256_backend_for_features(features)), "startup selection respects scalar override");
    const auto text = [](const std::string_view value) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    };
    check(text("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check(text("hello"), "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    check(text("The quick brown fox jumps over the lazy dog"),
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    check(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check(std::vector<std::uint8_t>(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Pattern vectors independently generated with Python hashlib (FIPS 180-4).
    struct Vector { std::size_t length; std::string_view digest; };
    constexpr Vector vectors[]{
        {0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1U, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {3U, "2e815fcec63d3682617b43b177da21a355e10cfd9dcf3d5a5e23d2b80db898d2"},
        {55U, "333e180b5ac056c481fb709addd4316f527ec82a137d8a7eec44b8062f157c20"},
        {56U, "2525d8e4edfdbb05a789ce67fc6dd26d5f97429d89b1e526a7102e804df4a1e8"},
        {57U, "4583cace9e4a622b33871a278a98444335b3ec683174ba405a1944b453b11b5b"},
        {63U, "32698c88be43feba30a7d3864e3ef0d85d68a576a2979da839e8437bc01ec223"},
        {64U, "6ca90947e82b1b1b3b1ed114fb1bf8f9305ca0f4d52ae841b8d8387dca646744"},
        {65U, "9fc2679fef0b52c85b9cc94f91c4b0337b048d44dcab1505272b33bf1adbdadc"},
        {119U, "3eb7b02c44beb3298c7726946a63c483c1bd480746b16db15e72a1c46d00bac8"},
        {120U, "c2a1acbe22005e1d75fc794c0c746ab32a9f105e491218014d6a733c8b565fa8"},
        {127U, "5d60c9e066cfbd26d04347264f3c98f564e6f7bd02b61a78b707cab822f09fae"},
        {128U, "3bcf6d6fa648ef4e50a37b5c2ee55caf1bd597cc4202dee99501b65fef74e861"},
        {129U, "ff22f8ab18415f7468681e788d4ec6282b2a678efd6519c4d41127adc8409a60"},
        {255U, "370a6fd5d796372870f013146486361799a6fe7a3bd53278d3250fd0e1301c05"},
        {256U, "8e4eca67f6e1bdb96eea613a31a8b890cc8b6ee85347b651c6033c7d54152ecc"},
        {257U, "c8fc477fced806d72c6805c8fd391f46f706fa2c9b3c9fcf296cfcf6cf904637"},
        {4097U, "f760f97a44730bef680f7d86016b033bc8e8fc3fc1c2bbcddf3ddc4804336fd4"},
        {65537U, "ca71382bca38c900f382eb22c7914b2af51bb4f7bbb2666716cb001eaa6a77ee"},
    };
    for (const auto& vector : vectors) {
        std::vector<std::uint8_t> storage(vector.length + 17);
        for (std::size_t i = 0; i < vector.length; ++i) storage[i + 17] = static_cast<std::uint8_t>((i * 37 + i / 3) & 255U);
        check(std::span<const std::uint8_t>(storage).subspan(17), vector.digest); // unaligned loads
    }
    // Deterministic random multiblock messages, exercising all residues and both padding cases.
    std::uint32_t random = 0x19760524U;
    for (std::size_t length = 0; length < 8192; length += 17) {
        std::vector<std::uint8_t> bytes(length);
        for (auto& byte : bytes) {
            random ^= random << 13U; random ^= random >> 17U; random ^= random << 5U;
            byte = static_cast<std::uint8_t>(random);
        }
        expect(sha256(bytes, Sha256Backend::x86_sha) == sha256(bytes, Sha256Backend::scalar),
            "random multiblock native and scalar bytes agree");
    }
    return failures == 0 ? 0 : 1;
}
