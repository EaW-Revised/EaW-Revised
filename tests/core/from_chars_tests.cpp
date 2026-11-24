#if defined(__linux__)
#define EAWR_TEST_C_LOCALE_FROM_CHARS
#endif
#include "eawr/core/from_chars.hpp"

#include <array>
#include <bit>
#include <cfenv>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
int failures{};

template <typename T> void check(std::string_view input) {
    T actual = static_cast<T>(42);
#if !defined(__APPLE__)
    T expected = actual;
    const auto reference = std::from_chars(input.data(), input.data() + input.size(), expected);
#endif
    const auto before = std::fegetround();
    const auto result = eawr::core::from_chars(input.data(), input.data() + input.size(), actual);
    if (std::fegetround() != before) { ++failures; std::cerr << "rounding mode changed\n"; }
#if !defined(__APPLE__)
    const bool same_value = (std::isnan(actual) && std::isnan(expected)) ||
        std::bit_cast<std::array<unsigned char, sizeof(T)>>(actual) ==
        std::bit_cast<std::array<unsigned char, sizeof(T)>>(expected);
    if (result.ec != reference.ec || result.ptr - input.data() != reference.ptr - input.data() || !same_value) {
        ++failures; std::cerr << "mismatch: " << input << '\n';
    }
#else
    if (input == "12.5tail" && (result.ec != std::errc{} || result.ptr - input.data() != 4 || actual != T{12.5})) ++failures;
    if (input == "+1" && (result.ec != std::errc::invalid_argument || result.ptr != input.data() || actual != T{42})) ++failures;
    if (input == "1e9999" && (result.ec != std::errc::result_out_of_range || actual != T{42})) ++failures;
#endif
}
}

int main() {
    constexpr std::array cases{"", "+1", " 1", "-0", "12.5tail", ".5", "1e+", "0x1p2", "-0x1p2",
        "nan", "inf", "-inf", "1e9999", "1e-9999", "1.1754943508222875e-38", "1.401298464324817e-45",
        "2.2250738585072014e-308", "4.9406564584124654e-324", "3.4028234663852886e38"};
    for (const auto input : cases) { check<float>(input); check<double>(input); }
    const int original = std::fegetround();
    if (original >= 0 && std::fesetround(FE_DOWNWARD) == 0) {
        // Verify the helper restores a caller's non-default rounding mode.
        float value{};
        constexpr std::string_view text = "1.25";
        const auto result = eawr::core::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || value != 1.25F || std::fegetround() != FE_DOWNWARD) ++failures;
        constexpr std::string_view overflow = "1e9999";
        const auto failed = eawr::core::from_chars(overflow.data(), overflow.data() + overflow.size(), value);
        if (failed.ec != std::errc::result_out_of_range || std::fegetround() != FE_DOWNWARD) ++failures;
        static_cast<void>(std::fesetround(original));
    }
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current ? current : "C";
    for (const char* locale : {"de_DE.UTF-8", "fr_FR.UTF-8", "de_DE.utf8", "fr_FR.utf8"}) {
        if (std::setlocale(LC_NUMERIC, locale)) {
            check<float>("1.5tail");
            check<double>("1.5tail");
            break;
        }
    }
    static_cast<void>(std::setlocale(LC_NUMERIC, saved.c_str()));
    return failures ? 1 : 0;
}
