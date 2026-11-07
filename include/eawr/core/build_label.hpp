#pragma once
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace eawr::core {
// The player-facing form of a build identity: "<label> (<short sha>)", with " dirty" appended for a modified tree.
// The identity is "<label>+<40-hex commit>[+dirty][+gcsyncN]": a release sets the label (the kit, for example 0.3.2g)
// through EAWR_KIT_LABEL, a development build's label is the project version. An identity of any other shape
// (a test override) is shown unchanged. Display only: never compared, hashed or written to a replay.
[[nodiscard]] inline std::string build_label(std::string_view identity) {
    const auto plus = identity.find('+');
    if (plus == std::string_view::npos || plus == 0) return std::string(identity);
    const auto rest = identity.substr(plus + 1);
    const auto end = rest.find('+');
    const auto sha = rest.substr(0, end);
    if (sha.size() != 40) return std::string(identity);
    for (const char c : sha)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::string(identity);
    std::string out(identity.substr(0, plus));
    out += " (";
    out += sha.substr(0, 7);
    out += ')';
    for (std::size_t at = end; at != std::string_view::npos && at < rest.size();) {
        const auto next = rest.find('+', at + 1);
        if (rest.substr(at + 1, next == std::string_view::npos ? next : next - at - 1) == "dirty") out += " dirty";
        at = next;
    }
    return out;
}
}
