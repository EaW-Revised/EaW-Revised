#pragma once
#include "eawr/mods/steam/text_formats.hpp"
#include <array>
#include <charconv>
#include <stdexcept>

namespace eawr::mods_service {
// Shared by the static-CRT service and its engine-linked helper; no ABI crosses the pipes.
inline std::string encode_json(const mods::steam::JsonValue& v) {
    using J = mods::steam::JsonValue;
    using mods::steam::json_quote;
    switch (v.kind) {
    case J::Kind::null: return "null";
    case J::Kind::boolean: return v.boolean ? "true" : "false";
    case J::Kind::number: {
        std::array<char, 64> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v.number);
        if (error != std::errc{}) throw std::runtime_error("cannot encode JSON number");
        return std::string(buffer.data(), end);
    }
    case J::Kind::string: return json_quote(v.string);
    case J::Kind::array: {
        std::string out = "[";
        for (const auto& item : v.array) { if (out.size() > 1) out += ','; out += encode_json(item); }
        return out + ']';
    }
    case J::Kind::object: {
        std::string out = "{";
        for (std::size_t i = 0; i < v.keys.size(); ++i) {
            if (i) out += ',';
            out += json_quote(v.keys[i]) + ':' + encode_json(v.values[i]);
        }
        return out + '}';
    }
    }
    return "null";
}
}
