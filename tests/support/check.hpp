#pragma once

#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace eawr::testing {

struct Checker {
    int failures = 0;

    void check(bool condition, std::string_view message, std::ostream& output, std::string_view prefix = "FAIL: ") {
        if (!condition) {
            if (!prefix.empty()) output << prefix;
            output << message << '\n';
            ++failures;
        }
    }

    void check_before_print(bool condition, std::string_view message,
        std::ostream& output, std::string_view prefix = "FAIL: ") {
        if (!condition) {
            ++failures;
            if (!prefix.empty()) output << prefix;
            output << message << '\n';
        }
    }

    [[nodiscard]] int finish() const noexcept { return failures ? 1 : 0; }
};

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

} // namespace eawr::testing
