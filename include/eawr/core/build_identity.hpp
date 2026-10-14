#pragma once
#include <string_view>

namespace eawr::core {
// PROJECT_VERSION + configure-time HEAD (+ dirty), or EAWR_BUILD_IDENTITY.
// A commit without reconfigure keeps the old value. Handshakes treat this as
// informational-plus-equality; it never enters replay bytes or world hashes.
[[nodiscard]] std::string_view build_identity() noexcept;
}
