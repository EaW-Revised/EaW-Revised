#include "eawr/core/json_text.hpp"

#include <sstream>

namespace eawr::core {
namespace {

enum class Controls { escaped, short_escaped, unicode, spaces, dropped, verbatim };

std::string quote(const std::string_view text, const Controls controls, const bool ascii = false) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : text) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        default:
            if (character < 0x20U) {
                if (controls == Controls::verbatim) { output << static_cast<char>(character); continue; }
                if (controls == Controls::spaces) { output << ' '; continue; }
                if (controls == Controls::dropped) continue;
                if (controls != Controls::unicode) {
                    switch (character) {
                    case '\n': output << "\\n"; continue;
                    case '\r': output << "\\r"; continue;
                    case '\t': output << "\\t"; continue;
                    case '\b': if (controls == Controls::short_escaped) { output << "\\b"; continue; } break;
                    case '\f': if (controls == Controls::short_escaped) { output << "\\f"; continue; } break;
                    default: break;
                    }
                }
                output << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            } else if (ascii && character >= 0x7fU) {
                output << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

} // namespace

std::string json_quote(const std::string_view text) { return quote(text, Controls::escaped); }
std::string json_quote_short_controls(const std::string_view text) { return quote(text, Controls::short_escaped); }
std::string json_quote_ascii(const std::string_view text) { return quote(text, Controls::unicode, true); }
std::string json_quote_ascii_short_controls(const std::string_view text) { return quote(text, Controls::escaped, true); }
std::string json_quote_space_controls(const std::string_view text) { return quote(text, Controls::spaces); }
std::string json_quote_drop_controls(const std::string_view text) { return quote(text, Controls::dropped); }

std::string json_quote_verbatim_controls(const std::string_view text) { return quote(text, Controls::verbatim); }

} // namespace eawr::core
