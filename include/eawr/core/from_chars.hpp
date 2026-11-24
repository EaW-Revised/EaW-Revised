#pragma once

#include <charconv>
#include <type_traits>

#if defined(__APPLE__) || defined(EAWR_TEST_C_LOCALE_FROM_CHARS)
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#include <string>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif

namespace eawr::core {

// Match the default floating from_chars contract on older Apple libc++.
// Integer calls and all non-Apple production calls go straight to the standard library.
template <typename T>
[[nodiscard]] inline std::from_chars_result from_chars(const char* first, const char* last, T& value) {
#if defined(__APPLE__) || defined(EAWR_TEST_C_LOCALE_FROM_CHARS)
    if constexpr (std::is_floating_point_v<T>) {
        if (first == last || *first == '+' || *first == ' ' || (*first >= '\t' && *first <= '\r'))
            return {first, std::errc::invalid_argument};
        static const locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        if (!c_locale) return {first, std::errc::invalid_argument};
        // strto*_l accepts hexadecimal prefixes; from_chars(general) consumes only the leading zero.
        std::string input(first, last);
        const std::size_t sign = input.front() == '-' ? 1U : 0U;
        if (input.size() > sign + 1U && input[sign] == '0' && (input[sign + 1U] == 'x' || input[sign + 1U] == 'X'))
            input.resize(sign + 1U);
        struct Environment final {
            int saved_errno = errno;
            int saved_round = std::fegetround();
            bool ready = saved_round >= 0 && std::fesetround(FE_TONEAREST) == 0;
            ~Environment() { if (saved_round >= 0) static_cast<void>(std::fesetround(saved_round)); errno = saved_errno; }
        } environment;
        if (!environment.ready) return {first, std::errc::invalid_argument};
        char* end = nullptr;
        errno = 0;
        T parsed{};
        if constexpr (std::is_same_v<T, float>) parsed = strtof_l(input.c_str(), &end, c_locale);
        else parsed = strtod_l(input.c_str(), &end, c_locale);
        const auto consumed = static_cast<std::size_t>(end - input.c_str());
        if (consumed == 0) return {first, std::errc::invalid_argument};
        if (errno == ERANGE && (parsed == T{} || std::isinf(parsed)))
            return {first + consumed, std::errc::result_out_of_range};
        value = parsed;
        return {first + consumed, {}};
    } else
#endif
    {
        return std::from_chars(first, last, value);
    }
}

} // namespace eawr::core
