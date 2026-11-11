#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eawr::core {
// Only the digest and sequence are agreement data. The local label is diagnostic metadata.
struct ContentLayerIdentity {
    std::string name;
    std::array<std::uint8_t, 32> digest{};
    friend bool operator==(const ContentLayerIdentity&, const ContentLayerIdentity&) = default;
};

struct PreparedContentIdentity {
    std::array<std::uint8_t, 32> digest{};
    std::vector<ContentLayerIdentity> layers;
    PreparedContentIdentity() = default;
    PreparedContentIdentity(std::array<std::uint8_t, 32> value) : digest(value) {}
};

// Receipts use the bytes actually consumed by preparation. Mount keys only group local receipts;
// neither they nor source paths/timestamps/cache locations enter the resulting content digest.
struct ContentInputReceipt {
    std::string logical_path, layer_id, sha256;
};
} // namespace eawr::core
