#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
int failures{};
void expect(const bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
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
}

int main() {
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
#if defined(_MSC_VER)
    char override_buffer[16]{};
    std::size_t override_length{};
    const auto* forced = getenv_s(&override_length, override_buffer, sizeof(override_buffer), "EAWR_SHA256_BACKEND") == 0
        ? override_buffer : nullptr;
#else
    const auto* forced = std::getenv("EAWR_SHA256_BACKEND");
#endif
    expect(sha256_backend() == (forced && std::string_view(forced) == "scalar"
        ? Sha256Backend::scalar : sha256_backend_for_features(features)), "startup selection respects scalar override");
    const auto text = [](const std::string_view value) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    };
    check(text("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
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
