#pragma once
#include "eawr/assets/assets.hpp"

namespace eawr::assets {
namespace shader_source_diagnostic_codes {
inline constexpr std::string_view budget = "EAWR-SOURCE-0001";
}
struct ShaderSource {
    Source source;
    std::vector<std::byte> bytes;
};
// Borrowed mounted content, never a host-filesystem include search. The root is
// a logical directory. Only a genuine not_found permits the second lookup.
class ShaderSourceProvider {
public:
    ShaderSourceProvider(const vfs::Vfs& mounted, std::string logical_root);
    [[nodiscard]] core::Result<ShaderSource> read(std::string_view logical,
                                                 std::size_t max_bytes) const;
    [[nodiscard]] core::Result<ShaderSource> include(std::string_view parent,
        std::string_view request, bool local, std::size_t max_bytes) const;
private:
    const vfs::Vfs& mounted_;
    std::string root_;
};
} // namespace eawr::assets
