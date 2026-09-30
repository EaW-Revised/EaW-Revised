#pragma once

#include <string>
#include <string_view>

namespace eawr::script::numeric::test {

// Runs `code` on the P0 hardware-double VM and returns its string result,
// or "error: <message>".
[[nodiscard]] std::string p0_run(std::string_view code);

} // namespace eawr::script::numeric::test
