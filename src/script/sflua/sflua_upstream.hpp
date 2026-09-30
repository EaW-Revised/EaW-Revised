#pragma once

// Included first by each wrapper in upstream/, which compiles one unmodified
// upstream Lua 5.0.2 source file inside EAWR_SFLUA_BEGIN/END. The C headers
// the sources include are already included (outside the namespace) by
// sflua_config.hpp, so their nested includes are no-ops.

#include "sflua_config.hpp"
#include "sflua_hooks.hpp"

// Process-locale independence for the lexer, number coercion and string
// library: C-locale ASCII classes and bytewise collation.
#undef isalnum
#undef isalpha
#undef iscntrl
#undef isdigit
#undef islower
#undef isprint
#undef ispunct
#undef isspace
#undef isupper
#undef isxdigit
#undef tolower
#undef toupper
#define isalnum(c) ::eawr::script::sflua_hooks::ascii_isalnum(c)
#define isalpha(c) ::eawr::script::sflua_hooks::ascii_isalpha(c)
#define iscntrl(c) ::eawr::script::sflua_hooks::ascii_iscntrl(c)
#define isdigit(c) ::eawr::script::sflua_hooks::ascii_isdigit(c)
#define islower(c) ::eawr::script::sflua_hooks::ascii_islower(c)
#define isprint(c) ::eawr::script::sflua_hooks::ascii_isprint(c)
#define ispunct(c) ::eawr::script::sflua_hooks::ascii_ispunct(c)
#define isspace(c) ::eawr::script::sflua_hooks::ascii_isspace(c)
#define isupper(c) ::eawr::script::sflua_hooks::ascii_isupper(c)
#define isxdigit(c) ::eawr::script::sflua_hooks::ascii_isxdigit(c)
#define tolower(c) ::eawr::script::sflua_hooks::ascii_tolower(c)
#define toupper(c) ::eawr::script::sflua_hooks::ascii_toupper(c)
#define strcoll(left, right) ::eawr::script::sflua_hooks::bytewise_collate((left), (right))
// tonumber(s, base) uses the retail Win32 32-bit `unsigned long`.
#define strtoul(text, end, base) ::eawr::script::sflua_hooks::strtoul32((text), (end), (base))

#if !defined(EAWR_SFLUA_HARDWARE_TWIN)
#define lua_str2number(text, end) ::eawr::script::sflua_hooks::parse_number((text), (end))
#define lua_number2str(buffer, value) ::eawr::script::sflua_hooks::format_number((buffer), (value))
#define sprintf ::eawr::script::sflua_hooks::format_item
#endif
