#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// What a header needs to name the VFS without parsing eawr/vfs/vfs.hpp and its <filesystem> and <functional>.
namespace eawr::vfs {

enum class AssetOrigin : std::uint8_t {
    loose,
    archive,
};

struct AssetRecord {
    std::string canonical_path;
    std::string original_path;
    std::string layer_id;
    AssetOrigin origin{AssetOrigin::loose};
    std::uint64_t size{0};
    std::string source_id;

    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;
};

[[nodiscard]] constexpr std::string_view to_string(const AssetOrigin origin) noexcept {
    return origin == AssetOrigin::loose ? "loose" : "archive";
}

class Vfs;

} // namespace eawr::vfs
