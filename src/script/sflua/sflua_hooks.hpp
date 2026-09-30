#pragma once

// C library replacements used while compiling upstream Lua for the
// authoritative profile (see sflua_upstream.hpp). They make the VM, lexer and
// string library independent of the process locale and of `unsigned long`
// width, and route every number <-> text conversion through the numeric
// backend.

#if !defined(EAWR_SFLUA_HARDWARE_TWIN)
#include "eawr/script/numeric/lua_number.hpp"
#endif

namespace eawr::script::sflua_hooks {

// C-locale ASCII classification; EOF and bytes >= 0x80 are in no class.
[[nodiscard]] constexpr bool in_range(int character, int first, int last) noexcept {
    return character >= first && character <= last;
}
[[nodiscard]] constexpr int ascii_isdigit(int c) noexcept { return in_range(c, '0', '9') ? 1 : 0; }
[[nodiscard]] constexpr int ascii_islower(int c) noexcept { return in_range(c, 'a', 'z') ? 1 : 0; }
[[nodiscard]] constexpr int ascii_isupper(int c) noexcept { return in_range(c, 'A', 'Z') ? 1 : 0; }
[[nodiscard]] constexpr int ascii_isalpha(int c) noexcept { return ascii_islower(c) | ascii_isupper(c); }
[[nodiscard]] constexpr int ascii_isalnum(int c) noexcept { return ascii_isalpha(c) | ascii_isdigit(c); }
[[nodiscard]] constexpr int ascii_isspace(int c) noexcept { return (c == ' ' || in_range(c, '\t', '\r')) ? 1 : 0; }
[[nodiscard]] constexpr int ascii_iscntrl(int c) noexcept { return (in_range(c, 0, 0x1F) || c == 0x7F) ? 1 : 0; }
[[nodiscard]] constexpr int ascii_isprint(int c) noexcept { return in_range(c, 0x20, 0x7E) ? 1 : 0; }
[[nodiscard]] constexpr int ascii_ispunct(int c) noexcept {
    return (in_range(c, 0x21, 0x7E) && ascii_isalnum(c) == 0) ? 1 : 0;
}
[[nodiscard]] constexpr int ascii_isxdigit(int c) noexcept {
    return (ascii_isdigit(c) != 0 || in_range(c, 'a', 'f') || in_range(c, 'A', 'F')) ? 1 : 0;
}
[[nodiscard]] constexpr int ascii_tolower(int c) noexcept { return ascii_isupper(c) != 0 ? c + ('a' - 'A') : c; }
[[nodiscard]] constexpr int ascii_toupper(int c) noexcept { return ascii_islower(c) != 0 ? c - ('a' - 'A') : c; }

// strcoll in the "C" locale: unsigned bytewise order.
[[nodiscard]] int bytewise_collate(const char* left, const char* right) noexcept;

// strtoul with a 32-bit `unsigned long` (the retail Win32 width) on every
// target: ASCII whitespace, optional sign, optional 0x prefix for base 16,
// saturation at 0xFFFFFFFF on overflow, negation modulo 2^32.
[[nodiscard]] unsigned long strtoul32(const char* text, char** end, int base) noexcept;

#if !defined(EAWR_SFLUA_HARDWARE_TWIN)
// lua_str2number: strtod contract on the backend parser.
[[nodiscard]] numeric::LuaNumber parse_number(const char* text, char** end) noexcept;

// lua_number2str: "%.14g" into luaV_tostring's 32-byte buffer.
void format_number(char* buffer, numeric::LuaNumber value) noexcept;

// sprintf for the upstream call sites: string.format items (whose e/E/f/g/G
// arguments are LuaNumber) and base-library "%p" names. Buffers are at least
// 128 bytes, 512 for string.format.
int format_item(char* buffer, const char* format, ...) noexcept;
#endif

} // namespace eawr::script::sflua_hooks
