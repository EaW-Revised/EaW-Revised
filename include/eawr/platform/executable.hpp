#pragma once

#include <optional>
#include <string>

namespace eawr::platform {

// Lowercase hex SHA-256 of the running executable image, or nullopt where the platform
// cannot locate or read it.
[[nodiscard]] std::optional<std::string> current_executable_sha256();

} // namespace eawr::platform
