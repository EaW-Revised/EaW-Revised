#include "report.hpp"
#include "eawr/core/json_text.hpp"
#include <algorithm>
namespace eawr::loadcheck {
void add(Groups& groups, std::string code, std::string example, core::Severity severity) {
    auto& group = groups[std::move(code)];
    ++group.count;
    group.severity = std::max(group.severity, severity);
    if (group.examples.size() < 3) group.examples.push_back(std::move(example));
}
std::size_t count(const Groups& groups, core::Severity severity) {
    std::size_t total{};
    for (const auto& [code, group] : groups) if (group.severity == severity) total += group.count;
    return total;
}
std::string groups_json(const Groups& groups) {
    std::string out = "[";
    for (const auto& [code, group] : groups) {
        if (out.size() > 1) out += ',';
        out += "{\"code\":" + core::json_quote(code) + ",\"severity\":" + core::json_quote(core::to_string(group.severity))
            + ",\"count\":" + std::to_string(group.count) + ",\"examples\":[";
        for (std::size_t i = 0; i < group.examples.size(); ++i) {
            if (i) out += ',';
            const auto& example = group.examples[i];
            auto end = std::min<std::size_t>(example.size(), 2048);
            while (end < example.size() && end && (static_cast<unsigned char>(example[end]) & 0xC0U) == 0x80U) --end;
            out += core::json_quote(example.substr(0, end));
        }
        out += "]}";
    }
    return out + ']';
}
std::string content_code(sim::MatchIdentity identity) {
    std::erase_if(identity.domains, [](const auto& d) { return d.name == "map-path" || d.name == "map-digest"; });
    identity = sim::compute_match_identity(std::move(identity));
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string code;
    for (unsigned i = 0; i < 10; ++i) {
        unsigned value = 0;
        for (unsigned bit = i * 5; bit < i * 5 + 5; ++bit)
            value = (value << 1U) | ((identity.sha256[bit / 8] >> (7U - bit % 8)) & 1U);
        code += alphabet[value];
    }
    return code;
}
}
