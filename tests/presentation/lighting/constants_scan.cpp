// lighting_constants_scan: read-only generator for
// plan/inventories/lighting-constants.json (P1-04). It mounts each profile's
// effective VFS view, records the XML-sourced lighting/shadow constants with
// their logical source locations, summarises the TED environment light
// records of every map, and expands the pinned reference maps' environments
// through the candidate field mapping and both irradiance policies. The host
// installation path is an input and is never written.

#include "eawr/assets/map.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lighting = eawr::presentation::lighting;

constexpr std::array<std::string_view, 2> pinned_maps{
    "data/art/maps/_land_planet_alderaan_02.ted",
    "data/art/maps/_space_planet_alderaan_01.ted",
};

// Tags inventoried from the effective XML. They are the only lighting or
// shadow constants the EaW/FoC/Remake XML corpus carries (see
// plan/inventories/xml-tags.json); the tactical sun, fills and ambient are
// authored per map in the TED environment records instead.
struct XmlSource final {
    std::string_view logical_path;
    std::vector<std::string_view> tags;
};

const std::array<XmlSource, 2> xml_sources{{
    {"data/xml/gameconstants.xml",
     {"Battle_Load_Planet_Ambient", "Galactic_Zoom_Light_Level", "Galactic_Zoom_In_Light_Angle",
      "Galactic_Zoom_Out_Light_Angle"}},
    {"data/xml/graphicdetails.xml", {"ShadowDetail", "ShadowVolumes", "SoftShadows", "DynamicLighting"}},
}};

std::string json(const std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out << '\\' << static_cast<char>(c);
        } else if (c < 0x20U || c >= 0x7fU) {
            out << "\\u00" << hex[c >> 4U] << hex[c & 15U];
        } else {
            out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}

std::string number(const double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    std::string text(buffer);
    if (text == "-0.000000") text = "0.000000";
    return text;
}

std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
    return std::string(value);
}

struct XmlHit final {
    std::string tag;
    std::string value;
    std::string parent;
    std::string logical_path;
    std::uint64_t line{};
};

// A line-oriented scan is enough for these flat, one-value-per-line files; it
// records, as context only, the last element opened alone on a line before
// the hit (not necessarily its parent).
std::vector<XmlHit> scan_xml(const std::string_view logical_path, const std::string& text,
                             const std::vector<std::string_view>& tags) {
    std::vector<XmlHit> hits;
    std::istringstream lines(text);
    std::string line;
    std::string parent;
    std::uint64_t number_of_line = 0;
    while (std::getline(lines, line)) {
        ++number_of_line;
        const std::string stripped = trim(line);
        if (stripped.size() > 2 && stripped.front() == '<' && stripped.back() == '>'
            && stripped.find('/') == std::string::npos && stripped.find(' ') == std::string::npos
            && stripped[1] != '!' && stripped[1] != '?') {
            parent = stripped.substr(1, stripped.size() - 2);
        }
        for (const std::string_view tag : tags) {
            const std::string open = "<" + std::string(tag) + ">";
            const std::string close = "</" + std::string(tag) + ">";
            const std::size_t begin = stripped.find(open);
            if (begin == std::string::npos) continue;
            const std::size_t end = stripped.find(close, begin);
            if (end == std::string::npos) continue;
            hits.push_back({std::string(tag),
                            trim(std::string_view(stripped).substr(begin + open.size(), end - begin - open.size())),
                            parent, std::string(logical_path), number_of_line});
        }
    }
    return hits;
}

std::vector<lighting::RawField> raw_fields(const eawr::assets::EnvironmentDescriptor& environment) {
    std::vector<lighting::RawField> fields;
    for (const auto& field : environment.fields) fields.push_back({field.id, field.bytes});
    return fields;
}

void write_color(std::ostream& out, const lighting::Color& color) {
    out << '[' << number(color.r) << ", " << number(color.g) << ", " << number(color.b) << ']';
}

void write_vector(std::ostream& out, const lighting::Vec3& vector) {
    out << '[' << number(vector.x) << ", " << number(vector.y) << ", " << number(vector.z) << ']';
}

void write_coefficients(std::ostream& out, const lighting::ColorCoefficients& coefficients) {
    out << '[';
    for (std::size_t channel = 0; channel < 3; ++channel) {
        out << (channel == 0 ? "[" : ", [");
        for (std::size_t index = 0; index < 9; ++index) {
            out << (index == 0 ? "" : ", ") << number(coefficients.rgb[channel][index]);
        }
        out << ']';
    }
    out << ']';
}

struct Profile final {
    std::string name;
    std::vector<std::pair<std::string, std::filesystem::path>> layers;
};

bool scan_profile(std::ostream& out, const Profile& profile) {
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, root] : profile.layers) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root);
        if (!manifest) {
            std::cerr << eawr::core::format_diagnostic(manifest.error()) << '\n';
            return false;
        }
        specs.push_back(std::move(manifest.value().mount));
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) {
        std::cerr << eawr::core::format_diagnostic(filesystem.error()) << '\n';
        return false;
    }
    out << "    {\"profile\": " << json(profile.name) << ", \"view\": \"effective\", \"layers\": [";
    for (std::size_t index = 0; index < profile.layers.size(); ++index) {
        out << (index == 0 ? "" : ", ") << json(profile.layers[index].first);
    }
    out << "],\n      \"xml_constants\": [";
    bool first = true;
    for (const XmlSource& source : xml_sources) {
        auto bytes = filesystem.value().open(source.logical_path);
        auto record = filesystem.value().stat(source.logical_path);
        if (!bytes || !record) continue;
        const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        for (const XmlHit& hit : scan_xml(source.logical_path, text, source.tags)) {
            out << (first ? "\n        " : ",\n        ") << "{\"tag\": " << json(hit.tag)
                << ", \"value\": " << json(hit.value) << ", \"preceding_open_element\": " << json(hit.parent)
                << ", \"logical_path\": " << json(hit.logical_path)
                << ", \"source_id\": " << json(record.value().source_id) << ", \"line\": " << hit.line << '}';
            first = false;
        }
    }
    out << "\n      ],\n";

    auto listed = filesystem.value().enumerate({}, ".ted");
    if (!listed) {
        std::cerr << eawr::core::format_diagnostic(listed.error()) << '\n';
        return false;
    }
    std::vector<std::string> paths;
    for (const auto& record : listed.value()) paths.push_back(record.canonical_path);
    std::sort(paths.begin(), paths.end());
    std::uint64_t maps = 0, with_records = 0, records = 0, decoded = 0;
    std::map<std::uint32_t, std::pair<double, double>> ranges;
    std::map<std::string, const eawr::assets::Map*> pinned;
    std::vector<std::pair<std::string, eawr::assets::Map>> kept;
    std::map<std::string, std::string> hashes;
    for (const std::string& path : paths) {
        auto map = eawr::assets::load_map(filesystem.value(), path);
        if (!map) continue;
        ++maps;
        if (!map.value().environments.empty()) ++with_records;
        for (const auto& environment : map.value().environments) {
            ++records;
            const auto fields = raw_fields(environment);
            if (lighting::candidate_environment(fields)) ++decoded;
            for (const auto& field : environment.fields) {
                if (field.id > 0x0dU || (field.bytes.size() != 4 && field.bytes.size() != 12)) continue;
                for (std::size_t offset = 0; offset < field.bytes.size(); offset += 4) {
                    std::uint32_t bits = 0;
                    for (std::size_t byte = 0; byte < 4; ++byte) {
                        bits |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.bytes[offset + byte]))
                            << (8U * byte);
                    }
                    const double value = static_cast<double>(std::bit_cast<float>(bits));
                    auto [entry, inserted] = ranges.try_emplace(field.id, value, value);
                    if (!inserted) {
                        entry->second.first = std::min(entry->second.first, value);
                        entry->second.second = std::max(entry->second.second, value);
                    }
                }
            }
        }
        if (std::find(pinned_maps.begin(), pinned_maps.end(), path) != pinned_maps.end()) {
            if (auto bytes = filesystem.value().open(path)) {
                hashes[path] = eawr::sim::sha256_hex(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
            }
            kept.emplace_back(path, std::move(map.value()));
        }
    }
    out << "      \"map_environments\": {\"maps\": " << maps << ", \"maps_with_environment_records\": "
        << with_records << ", \"records\": " << records << ", \"records_decoding_under_candidate\": " << decoded
        << ",\n        \"candidate_field_ranges\": {";
    first = true;
    for (const auto& [id, range] : ranges) {
        char key[8];
        std::snprintf(key, sizeof(key), "0x%02x", static_cast<unsigned>(id));
        out << (first ? "" : ", ") << json(key) << ": [" << number(range.first) << ", " << number(range.second) << ']';
        first = false;
    }
    out << "}},\n      \"pinned_maps\": [";
    first = true;
    for (const auto& [path, map] : kept) {
        out << (first ? "\n        " : ",\n        ") << "{\"logical_path\": " << json(path)
            << ", \"sha256\": " << json(hashes[path]) << ", \"environments\": [";
        first = false;
        for (std::size_t index = 0; index < map.environments.size(); ++index) {
            const auto& environment = map.environments[index];
            const auto fields = raw_fields(environment);
            const auto candidate = lighting::candidate_environment(fields);
            out << (index == 0 ? "\n          " : ",\n          ") << "{\"index\": " << index
                << ", \"name\": " << json(environment.name.value_or(""))
                << ", \"sky\": " << json(environment.primary_sky.value_or(""));
            if (!candidate) {
                out << ", \"candidate\": null}";
                continue;
            }
            out << ", \"candidate\": {\"lights\": [";
            constexpr std::array<std::string_view, 3> slots{"sun", "fill1", "fill2"};
            for (std::size_t light = 0; light < 3; ++light) {
                const auto& value = candidate->lights[light];
                out << (light == 0 ? "" : ", ") << "{\"slot\": " << json(slots[light]) << ", \"color\": ";
                write_color(out, value.color);
                out << ", \"intensity\": " << number(value.color.a) << ", \"direction\": ";
                write_vector(out, value.direction);
                out << '}';
            }
            out << "], \"ambient\": ";
            write_color(out, candidate->ambient);
            out << ", \"specular\": ";
            write_color(out, candidate->specular);
            out << ", \"sh_light_all_coefficients\": ";
            write_coefficients(out, lighting::project_lights(
                std::span<const lighting::DirectionalLight>(candidate->lights.data(), 3)));
            out << "}}";
        }
        out << "\n        ]}";
    }
    out << "\n      ]}";
    return true;
}

} // namespace

int main(const int argc, char** argv) {
    std::filesystem::path game_root, mod_root, report;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string_view flag = argv[index];
        if (flag == "--game-root") game_root = argv[index + 1];
        else if (flag == "--mod-root") mod_root = argv[index + 1];
        else if (flag == "--report") report = argv[index + 1];
    }
    if (game_root.empty() || report.empty()) {
        std::cerr << "Usage: lighting_constants_scan --game-root <GAME_ROOT> [--mod-root <REMAKE_DATA>] --report <file>\n";
        return 2;
    }
    const std::filesystem::path base = game_root / "GameData" / "Data";
    const std::filesystem::path expansion = game_root / "corruption" / "Data";
    std::vector<Profile> profiles{{"eaw", {{"base", base}}}, {"foc", {{"expansion", expansion}, {"base", base}}}};
    if (!mod_root.empty()) {
        profiles.push_back({"remake", {{"mod", mod_root / "Data"}, {"expansion", expansion}, {"base", base}}});
    }

    std::ostringstream out;
    out << "{\n  \"schema_version\": 1,\n"
        << "  \"generator\": \"lighting_constants_scan --game-root <GAME_ROOT> --mod-root <REMAKE_DATA> --report <OUT>\",\n"
        << "  \"provenance\": \"Read-only scan of each profile's effective VFS view. Logical paths, source ids, "
           "line numbers, content hashes and decoded values only; no original bytes and no host paths.\",\n"
        << "  \"finding\": \"The XML corpus carries no tactical sun, fill or ambient light; those are authored per "
           "map in TED environment records (main chunk 256, list 4, record 6).\",\n"
        << "  \"candidate_mapping\": {\"status\": \"unconfirmed\", \"basis\": \"field sizes and value ranges across "
           "the corpus matched to the MIT alo-viewer Environment structure (three directional lights with colour, "
           "intensity, heading and tilt; specular; ambient); not confirmed against the editor or executable\", "
           "\"fields\": {\"light_color\": [\"0x00\", \"0x01\", \"0x02\"], \"light_intensity\": [\"0x05\", \"0x06\", "
           "\"0x07\"], \"light_heading_radians\": [\"0x08\", \"0x09\", \"0x0a\"], \"light_tilt_radians\": [\"0x0b\", "
           "\"0x0c\", \"0x0d\"], \"specular\": \"0x03\", \"ambient\": \"0x04\"}, \"direction\": \"-(cos t sin h, "
           "-cos t cos h, sin t) in the TED source basis, the retail heading of R-LIT-01 (alo-viewer's 3DTypes.cpp "
           "formula at h - 90 degrees)\"},\n"
        << "  \"profiles\": [\n";
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (index != 0) out << ",\n";
        if (!scan_profile(out, profiles[index])) return 1;
    }
    out << "\n  ]\n}\n";
    std::ofstream file(report, std::ios::binary | std::ios::trunc);
    file << out.str();
    return file ? 0 : 1;
}
