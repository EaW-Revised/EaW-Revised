#include "sflua_hooks.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace eawr::script::sflua_hooks {

int bytewise_collate(const char* left, const char* right) noexcept { return std::strcmp(left, right); }

unsigned long strtoul32(const char* text, char** end, int base) noexcept {
    const char* cursor = text;
    while (ascii_isspace(static_cast<unsigned char>(*cursor)) != 0) {
        ++cursor;
    }
    bool negative = false;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        ++cursor;
    }
    const auto digit_value = [](char character) noexcept -> int {
        const int c = static_cast<unsigned char>(character);
        if (ascii_isdigit(c) != 0) {
            return c - '0';
        }
        if (ascii_islower(c) != 0) {
            return c - 'a' + 10;
        }
        if (ascii_isupper(c) != 0) {
            return c - 'A' + 10;
        }
        return 99;
    };
    if (base == 16 && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') && digit_value(cursor[2]) < 16) {
        cursor += 2;
    }
    if (base < 2 || base > 36 || digit_value(*cursor) >= base) {
        if (end != nullptr) {
            *end = const_cast<char*>(text);
        }
        return 0;
    }
    std::uint64_t value = 0;
    bool overflow = false;
    while (digit_value(*cursor) < base) {
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit_value(*cursor));
        if (value > 0xFFFFFFFFULL) {
            overflow = true;
            value = 0xFFFFFFFFULL;
        }
        ++cursor;
    }
    if (end != nullptr) {
        *end = const_cast<char*>(cursor);
    }
    if (overflow) {
        return 0xFFFFFFFFUL;
    }
    const auto narrow = static_cast<std::uint32_t>(value);
    return negative ? static_cast<std::uint32_t>(0U - narrow) : narrow;
}

#if !defined(EAWR_SFLUA_HARDWARE_TWIN)

numeric::LuaNumber parse_number(const char* text, char** end) noexcept {
    std::size_t consumed = 0;
    const std::uint64_t value = numeric::ActiveBackend::parse_prefix(std::string_view(text), consumed);
    if (end != nullptr) {
        *end = const_cast<char*>(text) + consumed;
    }
    return numeric::LuaNumber::from_repr(value);
}

void format_number(char* buffer, numeric::LuaNumber value) noexcept {
    numeric::ActiveBackend::format_lua(buffer, numeric::decimal::lua_number_buffer_size, value.repr);
}

namespace {

constexpr std::size_t item_buffer_size = 128;
constexpr std::size_t format_item_buffer_size = 512;

// Parses "%[flags][width][.precision]<conversion>" as produced by
// lstrlib's scanformat. Returns false for anything else.
bool parse_number_item(const char* format, numeric::decimal::FormatSpec& spec) noexcept {
    if (*format != '%') {
        return false;
    }
    ++format;
    for (;; ++format) {
        if (*format == '-') {
            spec.left_align = true;
        } else if (*format == '+') {
            spec.plus = true;
        } else if (*format == ' ') {
            spec.space = true;
        } else if (*format == '#') {
            spec.alternate = true;
        } else if (*format == '0') {
            spec.zero_pad = true;
        } else {
            break;
        }
    }
    while (ascii_isdigit(static_cast<unsigned char>(*format)) != 0) {
        spec.width = spec.width * 10 + (*format++ - '0');
    }
    if (*format == '.') {
        ++format;
        spec.precision = 0;
        while (ascii_isdigit(static_cast<unsigned char>(*format)) != 0) {
            spec.precision = spec.precision * 10 + (*format++ - '0');
        }
    }
    const char conversion = *format;
    if (conversion != 'e' && conversion != 'E' && conversion != 'f' && conversion != 'g' && conversion != 'G') {
        return false;
    }
    spec.conversion = conversion;
    return format[1] == '\0';
}

} // namespace

int format_item(char* buffer, const char* format, ...) noexcept {
    std::va_list arguments;
    va_start(arguments, format);
    numeric::decimal::FormatSpec spec;
    int written = 0;
    if (parse_number_item(format, spec)) {
        const numeric::LuaNumber value = va_arg(arguments, numeric::LuaNumber);
        written = static_cast<int>(numeric::ActiveBackend::format(buffer, format_item_buffer_size, value.repr, spec));
    } else {
        // Integer, character, string and pointer items: the C library is
        // exact and locale-independent for these without the ' flag, which
        // lstrlib does not accept.
        written = std::vsnprintf(buffer, item_buffer_size, format, arguments);
    }
    va_end(arguments);
    return written;
}

#endif

} // namespace eawr::script::sflua_hooks
