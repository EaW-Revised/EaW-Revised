#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend::report_privacy {

// Input is a JSON document. Only string values are changed, so JSON keys and
// unrelated mod names containing the account as a substring remain intact.
inline bool word_byte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 128; }
inline bool equal_byte(char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}
inline std::size_t match_prefix(std::string_view value, std::size_t at, std::string_view prefix) {
    std::size_t i = at;
    for (char c : prefix) {
        if (i >= value.size()) return 0;
        if (c == '/' || c == '\\') {
            if (value[i] == '/') ++i;
            else if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '\\' || value[i + 1] == '/')) i += 2;
            else return 0;
        } else if (!equal_byte(value[i++], c)) return 0;
    }
    if (i < value.size() && word_byte(static_cast<unsigned char>(value[i]))) return 0;
    return i - at;
}
inline std::string sanitize(std::string_view document, std::vector<std::string> paths, std::vector<std::string> names) {
    paths.erase(std::remove_if(paths.begin(), paths.end(), [](const auto& p) { return p.empty(); }), paths.end());
    names.erase(std::remove_if(names.begin(), names.end(), [](const auto& n) { return n.empty(); }), names.end());
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    std::string result;
    result.reserve(document.size());
    for (std::size_t pos = 0; pos < document.size();) {
        if (document[pos] != '"') { result += document[pos++]; continue; }
        const std::size_t begin = pos++;
        while (pos < document.size()) {
            if (document[pos] == '\\' && pos + 1 < document.size()) { pos += 2; continue; }
            if (document[pos++] == '"') break;
        }
        const std::size_t end = pos;
        std::size_t next = end;
        while (next < document.size() && std::isspace(static_cast<unsigned char>(document[next]))) ++next;
        if (next < document.size() && document[next] == ':') { result.append(document.substr(begin, end - begin)); continue; }
        result += '"';
        std::string_view value = document.substr(begin + 1, end - begin - 2);
        bool boundary = true;
        for (std::size_t i = 0; i < value.size();) {
            std::size_t matched = 0;
            for (const auto& path : paths) if ((matched = match_prefix(value, i, path)) != 0) break;
            if (matched) { result += "<profile>"; i += matched; boundary = false; continue; }
            // Keep escapes intact and conservatively treat their decoded value
            // as a boundary, so a name immediately after one is redacted.
            if (value[i] == '\\' && i + 1 < value.size()) {
                const auto count = value[i + 1] == 'u' && i + 5 < value.size() ? 6U : 2U;
                result.append(value.substr(i, count));
                i += count;
                boundary = true;
                continue;
            }
            for (const auto& name : names) {
                if (!boundary) continue;
                if (i + name.size() > value.size()) continue;
                bool same = true;
                for (std::size_t j = 0; j < name.size(); ++j) same &= equal_byte(value[i + j], name[j]);
                if (same && (i + name.size() == value.size() || !word_byte(static_cast<unsigned char>(value[i + name.size()])))) {
                    matched = name.size(); break;
                }
            }
            if (matched) { result += "<user>"; i += matched; boundary = false; }
            else { boundary = !word_byte(static_cast<unsigned char>(value[i])); result += value[i++]; }
        }
        result += '"';
    }
    return result;
}
} // namespace eawr::presentation::godot_backend::report_privacy
