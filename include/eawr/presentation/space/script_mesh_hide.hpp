#pragma once
#include "eawr/core/text.hpp"
#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
namespace eawr::presentation::space {
inline void set_script_mesh_hidden(std::span<const std::string> names, std::set<std::uint32_t>& hidden,
    std::string_view name, bool hide) {
    const auto match = std::find_if(names.begin(), names.end(), [&](const auto& mesh) { return core::iequals(mesh, name); });
    if (match == names.end()) return;
    const auto index = static_cast<std::uint32_t>(match - names.begin());
    if (hide) hidden.insert(index);
    else hidden.erase(index);
}
} // namespace eawr::presentation::space
