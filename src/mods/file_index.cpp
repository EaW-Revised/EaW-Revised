#include "eawr/mods/file_index.hpp"
#include "eawr/mods/steam/text_formats.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <istream>
#include <limits>
#include <map>
#include <ostream>
#include <set>

namespace eawr::mods {
namespace {
using J = steam::JsonValue;
using steam::json_quote;
constexpr std::string_view magic = "EAWR-FILES\n";
constexpr std::size_t record_limit = 1024 * 1024; // A row must also fit the service's line framing.

std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
bool digest(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string candidate_json(const FileCandidate& c, std::string_view role, const FileCandidate& winner) {
    return "{\"layer\":" + json_quote(c.layer) + ",\"origin\":" + json_quote(c.origin)
        + ",\"source\":" + json_quote(c.source) + ",\"size\":" + std::to_string(c.size)
        + ",\"sha256\":" + (c.sha256 ? json_quote(*c.sha256) : "null")
        + ",\"role\":" + json_quote(role) + ",\"identical_to_winner\":"
        + (c.sha256 && winner.sha256 && c.sha256 == winner.sha256 ? "true}" : "false}");
}
void number(std::ostream& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) output.put(static_cast<char>((value >> shift) & 255u));
}
std::optional<std::uint64_t> number(std::istream& input) {
    std::uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 8) {
        const auto c = input.get();
        if (c == std::char_traits<char>::eof()) return {};
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(c)) << shift;
    }
    return value;
}
bool text(std::ostream& output, const std::string& value) {
    if (value.size() > record_limit) return false;
    number(output, value.size());
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}
std::optional<std::string> text(std::istream& input) {
    const auto size = number(input);
    if (!size || *size > record_limit) return {};
    std::string value(static_cast<std::size_t>(*size), '\0');
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!input) return {};
    return value;
}
std::optional<std::uint64_t> count(const J* value) {
    if (!value || value->kind != J::Kind::number || !std::isfinite(value->number)
        || value->number < 0 || value->number > 9007199254740991.0 || std::floor(value->number) != value->number) return {};
    return static_cast<std::uint64_t>(value->number);
}
std::optional<FileRow> parse_row(const J& value, const std::set<std::string>& layers) {
    const auto* path = value.string_member("path");
    const auto* candidates = value.member("candidates");
    if (!path || path->empty() || !candidates || candidates->kind != J::Kind::array || candidates->array.empty()) return {};
    FileRow row{*path, {}};
    for (const auto& entry : candidates->array) {
        const auto* layer = entry.string_member("layer");
        const auto* origin = entry.string_member("origin");
        const auto* source = entry.string_member("source");
        const auto size = count(entry.member("size"));
        const auto* sha = entry.member("sha256");
        if (!layer || !layers.contains(*layer) || !origin || (*origin != "loose" && *origin != "archive") || !source || !size || !sha) return {};
        FileCandidate candidate{*layer, *origin, *source, *size, {}};
        if (candidates->array.size() > 1) {
            if (sha->kind != J::Kind::string || !digest(sha->string)) return {};
            candidate.sha256 = sha->string;
        } else if (sha->kind != J::Kind::null) return {};
        row.candidates.push_back(std::move(candidate));
    }
    return row;
}
} // namespace

bool FileRow::identical() const {
    return candidates.size() > 1 && candidates.front().sha256
        && std::all_of(candidates.begin() + 1, candidates.end(), [&](const auto& candidate) {
            return candidate.sha256 == candidates.front().sha256;
        });
}

void summarize_file_index(FileIndex& index) {
    std::map<std::string, std::size_t> positions;
    for (std::size_t i = 0; i < index.layers.size(); ++i) {
        auto& layer = index.layers[i];
        positions[layer.key] = i;
        layer.files = layer.wins = layer.loses = layer.only = 0;
    }
    for (const auto& row : index.rows) {
        std::set<std::string> providers;
        for (const auto& candidate : row.candidates) providers.insert(candidate.layer);
        for (const auto& key : providers) {
            if (!positions.contains(key)) {
                positions[key] = index.layers.size();
                index.layers.push_back({key});
            }
            auto& layer = index.layers[positions.at(key)];
            ++layer.files;
            if (providers.size() == 1) ++layer.only;
            else if (row.candidates.front().layer == key) ++layer.wins;
            else ++layer.loses;
        }
    }
}

bool matches_file_filter(const FileRow& row, const FileFilter& filter) {
    if (row.candidates.empty() || (filter.conflicts_only && row.candidates.size() < 2)
        || (filter.hide_identical && row.identical())) return false;
    if (!filter.path.empty() && row.path.find(lower(filter.path)) == std::string::npos) return false;
    if (!filter.type.empty()) {
        auto type = lower(filter.type);
        if (type.front() != '.') type.insert(type.begin(), '.');
        if (!row.path.ends_with(type)) return false;
    }
    for (std::size_t i = 0; i < row.candidates.size(); ++i) {
        if (!filter.layer.empty() && row.candidates[i].layer != filter.layer) continue;
        if (filter.role == "either" || (filter.role == "winner" && i == 0) || (filter.role == "loser" && i > 0)) return true;
    }
    return false;
}

std::string file_row_json(const FileRow& row) {
    if (row.candidates.empty()) return "{}";
    const auto& winner = row.candidates.front();
    const std::string_view role = row.candidates.size() == 1 ? "only" : "win";
    std::string out = "{\"path\":" + json_quote(row.path) + ",\"winner\":" + candidate_json(winner, role, winner) + ",\"candidates\":[";
    for (std::size_t i = 0; i < row.candidates.size(); ++i) {
        if (i) out += ',';
        out += candidate_json(row.candidates[i], i == 0 ? role : "lose", winner);
    }
    return out + "],\"identical\":" + (row.identical() ? "true}" : "false}");
}

std::string file_layers_json(const FileIndex& index) {
    std::string out = "[";
    for (const auto& layer : index.layers) {
        if (out.size() > 1) out += ',';
        out += "{\"key\":" + json_quote(layer.key) + ",\"files\":" + std::to_string(layer.files)
            + ",\"wins\":" + std::to_string(layer.wins) + ",\"loses\":" + std::to_string(layer.loses)
            + ",\"only\":" + std::to_string(layer.only) + '}';
    }
    return out + ']';
}

bool write_file_index(std::ostream& output, const FileIndex& index) {
    if (!digest(index.chain_fingerprint)) return false;
    output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    number(output, kFileIndexVersion);
    if (!text(output, index.chain_fingerprint)) return false;
    number(output, index.layers.size());
    for (const auto& layer : index.layers) if (!text(output, layer.key)) return false;
    number(output, index.rows.size());
    for (const auto& row : index.rows) if (!text(output, file_row_json(row))) return false;
    return static_cast<bool>(output);
}

std::optional<FileIndex> read_file_index(std::istream& input, std::string_view fingerprint) {
    std::array<char, magic.size()> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!input || std::string_view(header.data(), header.size()) != magic || number(input) != kFileIndexVersion) return {};
    auto key = text(input);
    if (!key || !digest(*key) || *key != fingerprint) return {};
    FileIndex index;
    index.chain_fingerprint = std::move(*key);
    std::set<std::string> layers;
    const auto layer_count = number(input);
    if (!layer_count) return {};
    for (std::uint64_t i = 0; i < *layer_count; ++i) {
        auto layer = text(input);
        if (!layer || layer->empty() || !layers.insert(*layer).second) return {};
        index.layers.push_back({std::move(*layer)});
    }
    const auto row_count = number(input);
    if (!row_count) return {};
    for (std::uint64_t i = 0; i < *row_count; ++i) {
        auto encoded = text(input);
        const auto value = encoded ? steam::parse_json(*encoded) : std::nullopt;
        auto row = value ? parse_row(*value, layers) : std::nullopt;
        if (!row || (!index.rows.empty() && index.rows.back().path >= row->path)) return {};
        if (row->candidates.size() > 1) {
            index.stats.candidates_hashed += row->candidates.size();
            for (const auto& candidate : row->candidates) index.stats.bytes_hashed += candidate.size;
        }
        index.rows.push_back(std::move(*row));
    }
    if (input.peek() != std::char_traits<char>::eof()) return {};
    index.stats.enumerations = 1;
    summarize_file_index(index);
    return index;
}

} // namespace eawr::mods
