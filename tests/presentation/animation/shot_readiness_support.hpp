#pragma once

#include "eawr/presentation/animation/shot_readiness.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace shot_readiness_test_support {

namespace playback = eawr::presentation::animation;
[[nodiscard]] std::optional<float> parse_seconds(const std::string_view text);
int usage(const std::string_view reason);

int run_shot_readiness_cases(int argc, char** argv);

template <typename T>
[[nodiscard]] std::optional<T> parse_unsigned(const std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace shot_readiness_test_support
