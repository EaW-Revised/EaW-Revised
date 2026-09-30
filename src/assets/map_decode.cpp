#include "asset_internal.hpp"
#include "eawr/assets/map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>


namespace eawr::assets {

namespace {
using detail::Chunk;
using detail::Mini;
using detail::Reader;

template <typename T>
core::Result<T> fail(const Source& source, std::string message, const std::uint64_t offset,
                     const std::string_view code = diagnostic_codes::structure) {
    return core::Result<T>::failure(detail::error(source, code, std::move(message), offset));
}

RawField raw_field(const Mini& mini) {
    return {mini.type, mini.offset, {mini.payload.begin(), mini.payload.end()}};
}

RawChunk raw_chunk(const Chunk& chunk) {
    // Group bytes are represented by their child tree.  Retaining the complete
    // group payload as well would multiply retained memory at every nesting
    // level for a hostile-but-bounded input.
    RawChunk result{chunk.type, chunk.group, chunk.header_offset,
                    chunk.group ? std::vector<std::byte>{} : std::vector<std::byte>(chunk.payload.begin(), chunk.payload.end()), {}};
    result.children.reserve(chunk.children.size());
    for (const auto& child : chunk.children) result.children.push_back(raw_chunk(child));
    return result;
}

const Chunk* direct(const Chunk& parent, const std::uint32_t id) {
    const auto found = std::find_if(parent.children.begin(), parent.children.end(),
                                    [=](const Chunk& item) { return item.type == id; });
    return found == parent.children.end() ? nullptr : &*found;
}

std::vector<const Chunk*> directs(const Chunk& parent, const std::uint32_t id) {
    std::vector<const Chunk*> result;
    for (const auto& child : parent.children) if (child.type == id) result.push_back(&child);
    return result;
}

bool read_u32(const Mini& mini, std::uint32_t& value) {
    Reader reader(mini.payload); return reader.u32(value) && reader.remaining() == 0;
}
bool read_f32(const Mini& mini, float& value) {
    Reader reader(mini.payload); return reader.f32(value) && reader.remaining() == 0;
}
bool read_vec3(const Mini& mini, Vec3f& value) {
    Reader reader(mini.payload); return reader.vec3(value) && reader.remaining() == 0;
}
bool read_string(const Mini& mini, std::string& value) {
    Reader reader(mini.payload); return reader.string(value) && reader.remaining() == 0;
}

bool finite(const Vec3f& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::uint32_t last_id(const std::string_view path) {
    const auto slash = path.rfind('/');
    const auto tail = slash == std::string_view::npos ? path : path.substr(slash + 1);
    std::uint32_t value = 0;
    for (const char digit : tail) {
        if (digit < '0' || digit > '9') return 0;
        value = value * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    return value;
}

// One typed notice, mirrored into the generic notice list.
void issue(Map& map, const MapIssue kind, std::string path, const std::uint64_t offset,
           const std::uint64_t size, std::string message,
           const std::optional<std::uint32_t> ordinal = std::nullopt, const int field = -1) {
    const auto field_id = field < 0 ? std::optional<std::uint8_t>{} : std::optional<std::uint8_t>{static_cast<std::uint8_t>(field)};
    map.notices.push_back({last_id(path), offset, size, message});
    map.issues.push_back({kind, std::move(path), offset, size, ordinal, field_id, std::move(message)});
}

void append_utf8(std::string& out, const std::uint32_t code_point) {
    if (code_point < 0x80U) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

// Zero-terminated UTF-16LE: even length, one terminal NUL unit, no interior
// NUL and only paired surrogates.  The two-byte empty value is valid.
bool decode_utf16z(const std::span<const std::byte> payload, std::string& out) {
    if (payload.size() < 2 || (payload.size() & 1U) != 0) return false;
    const std::size_t units = payload.size() / 2;
    const auto unit = [&](const std::size_t index) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[index * 2])) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[index * 2 + 1])) << 8U);
    };
    if (unit(units - 1) != 0) return false;
    out.clear();
    for (std::size_t index = 0; index + 1 < units; ++index) {
        std::uint32_t code_point = unit(index);
        if (code_point == 0) return false;
        if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            if (index + 2 >= units) return false;
            const std::uint32_t low = unit(index + 1);
            if (low < 0xDC00U || low > 0xDFFFU) return false;
            code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
            ++index;
        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            return false;
        }
        append_utf8(out, code_point);
    }
    return true;
}

// Root minis the contract names, whether or not their meaning is pinned.
bool known_root_field(const std::uint8_t id) {
    return id <= 0x0B || (id >= 0x10 && id <= 0x12);
}

// Chunk paths whose children are checked against this table.
constexpr std::array<std::string_view, 6> checked_paths{
    "1", "1/256", "1/256/2", "1/256/4", "1/257", "1/257/2",
};
// Known paths whose content is retained without a per-child schema check:
// either leaves, or groups (object graph, waves, patches) kept losslessly.
constexpr std::array<std::string_view, 21> retained_paths{
    "1/256/2/0", "1/256/4/5", "1/256/4/6", "1/256/8",
    "1/257/0", "1/257/1", "1/257/2/3", "1/257/5", "1/257/6", "1/257/7", "1/257/8", "1/257/9",
    "1/258", "1/259", "1/261", "1/266", "1/267", "1/268",
    "2", "3", "19",
};

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& table, const std::string_view path) {
    return std::find(table.begin(), table.end(), path) != table.end();
}

// Contract framing step 7: every unknown bounded node is retained in the raw
// tree and named once per path by an offset/size notice.
void walk_schema(Map& map, const std::vector<Chunk>& chunks, const std::string& parent,
                 std::set<std::string>& seen) {
    for (const auto& chunk : chunks) {
        const std::string path = parent.empty() ? std::to_string(chunk.type)
                                                : parent + '/' + std::to_string(chunk.type);
        if (listed(checked_paths, path)) {
            walk_schema(map, chunk.children, path, seen);
        } else if (path == "19") {
            if (seen.insert(path).second) {
                issue(map, MapIssue::preview_not_decoded, path, chunk.header_offset, chunk.payload.size(),
                      "preview-like raster block is bounded and retained, not decoded");
            }
        } else if (!listed(retained_paths, path) && seen.insert(path).second) {
            issue(map, MapIssue::unknown_chunk, path, chunk.header_offset, chunk.payload.size(),
                  "unknown chunk is bounded and retained without a semantic view");
        }
    }
}

void extract_environment(Map& map, const Chunk& main) {
    const auto* bundle = direct(main, 256); if (!bundle) return;
    if (!direct(*bundle, 8)) {
        issue(map, MapIssue::optional_section_absent, "1/256/8", bundle->header_offset, 0,
              "optional 1/256/8 record is absent");
    }
    const auto* list = direct(*bundle, 4); if (!list) return;
    std::uint32_t ordinal = 0;
    for (const auto* record : directs(*list, 6)) {
        const auto record_ordinal = ordinal++;
        auto minis = detail::parse_minis(*record, map.source);
        if (!minis) {
            issue(map, MapIssue::environment_stream_invalid, "1/256/4/6", record->header_offset,
                  record->payload.size(), "invalid environment mini-record stream");
            continue;
        }
        EnvironmentDescriptor environment;
        environment.record_ordinal = record_ordinal;
        for (const auto& mini : minis.value()) {
            environment.fields.push_back(raw_field(mini));
            std::string value;
            if (!read_string(mini, value)) continue;
            if (mini.type == 0x14) environment.name = std::move(value);
            else if (mini.type == 0x19) {
                environment.primary_sky = std::move(value);
                environment.primary_sky_offset = mini.offset;
            } else if (mini.type == 0x1a) {
                environment.secondary_sky = std::move(value);
                environment.secondary_sky_offset = mini.offset;
            } else if (mini.type == 0x2f) {
                environment.cloud_texture = std::move(value);
                environment.cloud_texture_offset = mini.offset;
            }
        }
        map.environments.push_back(std::move(environment));
    }
}

// 1/259 holds scalar minis and vector3 minis.  Two vectors form a volume only
// when they are adjacent in the stream, carry ids k and k+1, and bound each
// other (minimum <= maximum on every axis).  Anything else, such as the
// standalone vector of the older header form, is kept raw by field id; no
// pairing is guessed and nothing is recentred, reordered or clamped.
void extract_volumes(Map& map, const Chunk& main) {
    const auto* bundle = direct(main, 259);
    if (!bundle) {
        issue(map, MapIssue::optional_section_absent, "1/259", main.header_offset, 0,
              "1/259 volume section is absent");
        return;
    }
    if (bundle->group) {
        issue(map, MapIssue::volume_stream_invalid, "1/259", bundle->header_offset, bundle->payload.size(),
              "1/259 is a group, not a volume mini-record stream; retained raw only");
        return;
    }
    auto minis = detail::parse_minis(*bundle, map.source);
    if (!minis) {
        issue(map, MapIssue::volume_stream_invalid, "1/259", bundle->header_offset, bundle->payload.size(),
              "invalid volume mini-record stream");
        return;
    }
    const auto& fields = minis.value();
    for (const auto& mini : fields) map.volume_fields.push_back(raw_field(mini));
    for (std::size_t index = 0; index + 1 < fields.size();) {
        Vec3f minimum{}, maximum{};
        const bool paired = fields[index + 1].type == fields[index].type + 1 &&
            read_vec3(fields[index], minimum) && read_vec3(fields[index + 1], maximum) &&
            finite(minimum) && finite(maximum) &&
            minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z;
        if (!paired) { ++index; continue; }
        map.volumes.push_back({fields[index].type, fields[index + 1].type,
                               fields[index].offset, fields[index + 1].offset, minimum, maximum});
        index += 2;
    }
}

void extract_water(Map& map, const Chunk& terrain_bundle) {
    // The mini-to-editor-control mapping remains open.  Keep each bounded
    // record losslessly rather than assigning names to unproven numeric values.
    // The grid minis 0, 1, 4 and 5 belong to Terrain::header_fields, not here.
    if (const auto* header = direct(terrain_bundle, 0)) {
        // An unparseable header is reported once, as terrain_header_invalid.
        if (auto minis = detail::parse_minis(*header, map.source)) {
            WaterRecord water{"1/257/0", header->header_offset, {}};
            for (const auto& mini : minis.value()) {
                if (mini.type == 0 || mini.type == 1 || mini.type == 4 || mini.type == 5) continue;
                water.fields.push_back(raw_field(mini));
                if (mini.type != 0x1d && mini.type != 0x1e) continue;
                std::string name;
                if (read_string(mini, name)) {
                    map.water_textures.push_back({mini.type, std::move(name)});
                } else {
                    issue(map, MapIssue::water_texture_invalid, "1/257/0", mini.offset, mini.payload.size(),
                          "water texture mini is not a terminated narrow string", std::nullopt, mini.type);
                }
            }
            if (!water.fields.empty()) map.water_records.push_back(std::move(water));
        }
    }
    std::set<std::string> reported;
    const auto retain = [&](const auto& self, const Chunk& record, const std::string& path) -> void {
        if (record.group) {
            for (const auto& child : record.children) self(self, child, path + '/' + std::to_string(child.type));
            return;
        }
        auto minis = detail::parse_minis(record, map.source);
        if (!minis) {
            // The leaf stays in the raw tree; name each failing path once.
            if (reported.insert(path).second) {
                issue(map, MapIssue::water_stream_invalid, path, record.header_offset, record.payload.size(),
                      "water leaf is not a mini-record stream; retained raw only");
            }
            return;
        }
        WaterRecord water{path, record.header_offset, {}};
        for (const auto& mini : minis.value()) water.fields.push_back(raw_field(mini));
        if (!water.fields.empty()) map.water_records.push_back(std::move(water));
    };
    if (const auto* waves = direct(terrain_bundle, 9)) retain(retain, *waves, "1/257/9");
}

using CrcEntry = std::pair<std::uint32_t, const ObjectTypeRef*>;

// A CRC index over the catalog, built once per map.  Resolving each placement
// by rehashing every catalog id is quadratic over a real corpus; the index
// keeps the multimap semantics and the catalog's own candidate order intact.
std::vector<CrcEntry> crc_index(const ObjectTypeCatalog& catalog) {
    std::vector<CrcEntry> index;
    index.reserve(catalog.entries.size());
    for (const auto& entry : catalog.entries) {
        index.emplace_back(object_type_crc(entry.logical_name), &entry);
    }
    std::stable_sort(index.begin(), index.end(),
                     [](const CrcEntry& left, const CrcEntry& right) { return left.first < right.first; });
    return index;
}

void classify_orientation(Map& map, Placement& placement, const Chunk& payload) {
    const auto ordinal = placement.key.record_ordinal;
    const auto size = payload.payload.size();
    if (!placement.orientation_degrees) {
        placement.orientation_status = OrientationStatus::absent;
        issue(map, MapIssue::orientation_absent, "1/258/1/1100/1113/1200", payload.header_offset, size,
              "placement orientation mini 5 is absent or malformed; no rotation is assumed", ordinal, 5);
    } else if (!finite(*placement.orientation_degrees)) {
        placement.orientation_status = OrientationStatus::nonfinite;
        issue(map, MapIssue::orientation_nonfinite, "1/258/1/1100/1113/1200", payload.header_offset, size,
              "placement orientation has a NaN or infinite component", ordinal, 5);
    } else if (placement.orientation_degrees->x == 0.0F && placement.orientation_degrees->y == 0.0F) {
        placement.orientation_status = OrientationStatus::yaw_only;
    } else {
        placement.orientation_status = OrientationStatus::unsupported_three_axis_order;
        issue(map, MapIssue::orientation_three_axis, "1/258/1/1100/1113/1200", payload.header_offset, size,
              "placement retains nonzero roll/pitch; Euler composition order is unresolved", ordinal, 5);
    }
}

// Contract path 1/258/1/1100/1113/1200, walked by direct children only.  Every
// 1100 keeps its ordinal; one without exactly one 1113/1200 payload stays an
// unresolved row instead of vanishing or being split.
void extract_placements(Map& map, const Chunk& main, const ObjectTypeCatalog& catalog) {
    const auto* graph = direct(main, 258); if (!graph) return;
    const auto* objects = direct(*graph, 1); if (!objects) return;
    const auto index = crc_index(catalog);
    std::uint32_t ordinal = 0;
    for (const auto& record : objects->children) {
        if (record.type != 1100) continue;
        Placement placement;
        placement.key = {map.source, ordinal++};
        placement.byte_offset = record.header_offset;
        const auto record_ordinal = placement.key.record_ordinal;
        const auto bodies = directs(record, 1113);
        const auto payloads = bodies.size() == 1 ? directs(*bodies.front(), 1200) : std::vector<const Chunk*>{};
        if (bodies.size() != 1 || payloads.size() != 1) {
            issue(map, MapIssue::placement_record_shape, "1/258/1/1100", record.header_offset, record.payload.size(),
                  "persisted-object record does not hold exactly one 1113/1200 payload", record_ordinal);
            map.placements.push_back(std::move(placement));
            continue;
        }
        const auto* payload = payloads.front();
        auto minis = detail::parse_minis(*payload, map.source);
        if (!minis) {
            issue(map, MapIssue::placement_stream_invalid, "1/258/1/1100/1113/1200", payload->header_offset,
                  payload->payload.size(), "invalid persisted-object mini-record stream", record_ordinal);
            map.placements.push_back(std::move(placement));
            continue;
        }
        for (const auto& mini : minis.value()) {
            placement.fields.push_back(raw_field(mini));
            std::uint32_t integer{}; Vec3f vector{};
            if (mini.type == 0 && read_u32(mini, integer)) placement.serialized_object_id = integer;
            else if (mini.type == 1 && read_u32(mini, integer)) placement.type_crc = integer;
            else if (mini.type == 4 && read_vec3(mini, vector)) placement.position = vector;
            else if (mini.type == 5 && read_vec3(mini, vector)) placement.orientation_degrees = vector;
        }
        if (placement.type_crc) {
            const auto first = std::lower_bound(
                index.begin(), index.end(), *placement.type_crc,
                [](const CrcEntry& entry, const std::uint32_t crc) { return entry.first < crc; });
            for (auto match = first; match != index.end() && match->first == *placement.type_crc; ++match) {
                placement.type_candidates.push_back(*match->second);
            }
            placement.type_resolution = placement.type_candidates.empty() ? TypeResolution::missing :
                placement.type_candidates.size() == 1 ? TypeResolution::unique : TypeResolution::collision;
            if (placement.type_resolution == TypeResolution::missing) {
                issue(map, MapIssue::placement_type_missing, "1/258/1/1100/1113/1200", payload->header_offset,
                      payload->payload.size(), "placement type CRC is absent from active XML catalog", record_ordinal, 1);
            } else if (placement.type_resolution == TypeResolution::collision) {
                issue(map, MapIssue::placement_type_collision, "1/258/1/1100/1113/1200", payload->header_offset,
                      payload->payload.size(), "placement type CRC collides in active XML catalog", record_ordinal, 1);
            }
        } else {
            issue(map, MapIssue::placement_crc_absent, "1/258/1/1100/1113/1200", payload->header_offset,
                  payload->payload.size(), "placement lacks a valid type CRC", record_ordinal, 1);
        }
        classify_orientation(map, placement, *payload);
        map.placements.push_back(std::move(placement));
    }
}

// The terrain semantic view.  Any failure leaves the view absent with a
// notice; the raw chunks stay in Map::chunks and the load still succeeds.
std::optional<Terrain> extract_terrain(Map& map, const Chunk& bundle) {
    const auto* header = direct(bundle, 0);
    const auto* plane = direct(bundle, 5);
    if (!plane) {
        if (const auto* legacy = direct(bundle, 1)) {
            issue(map, MapIssue::terrain_legacy_plane, "1/257/1", legacy->header_offset, legacy->payload.size(),
                  "legacy 1/257/1 terrain plane has no vertex intensity; no current-format view is produced");
            return std::nullopt;
        }
    }
    if (!header || !plane) {
        issue(map, MapIssue::terrain_absent, "1/257", bundle.header_offset, bundle.payload.size(),
              "land terrain requires a 1/257/0 header and a 1/257/5 sample plane");
        return std::nullopt;
    }
    auto minis = detail::parse_minis(*header, map.source);
    if (!minis) {
        issue(map, MapIssue::terrain_header_invalid, "1/257/0", header->header_offset, header->payload.size(),
              "invalid terrain header mini-record stream");
        return std::nullopt;
    }
    Terrain terrain;
    for (const auto& mini : minis.value()) {
        terrain.header_fields.push_back(raw_field(mini));
        std::uint32_t value{};
        if (!read_u32(mini, value)) continue;
        if (mini.type == 0) terrain.width = value;
        else if (mini.type == 1) terrain.height = value;
        else if (mini.type == 4) terrain.cell_count = value;
        else if (mini.type == 5) terrain.slot_count = value;
    }
    std::size_t cells{}, expected{};
    if (terrain.width == 0 || terrain.height == 0 || !detail::checked_multiply<std::size_t>(terrain.width, terrain.height, cells) ||
        !detail::checked_multiply<std::size_t>(cells, 4, expected) || cells > detail::max_elements) {
        issue(map, MapIssue::terrain_dimensions, "1/257/0", header->header_offset, header->payload.size(),
              "terrain dimensions are zero or exceed safety limits");
        return std::nullopt;
    }
    if (terrain.cell_count != 0 && terrain.cell_count != cells) {
        issue(map, MapIssue::terrain_cell_count, "1/257/0", header->header_offset, header->payload.size(),
              "terrain header cell count disagrees with dimensions", std::nullopt, 4);
        return std::nullopt;
    }
    if (plane->payload.size() != expected) {
        issue(map, MapIssue::terrain_plane_size, "1/257/5", plane->header_offset, plane->payload.size(),
              "terrain sample plane size disagrees with dimensions");
        return std::nullopt;
    }
    if (const auto* descriptors = direct(bundle, 2)) {
        for (const auto* descriptor : directs(*descriptors, 3)) {
            auto fields = detail::parse_minis(*descriptor, map.source);
            if (!fields) {
                issue(map, MapIssue::terrain_material_stream_invalid, "1/257/2/3", descriptor->header_offset,
                      descriptor->payload.size(), "invalid terrain material mini-record stream");
                return std::nullopt;
            }
            TerrainMaterial material;
            for (const auto& mini : fields.value()) {
                material.fields.push_back(raw_field(mini)); std::string text;
                if (mini.type == 0x0c && read_string(mini, text)) material.primary_texture = std::move(text);
                else if (mini.type == 0x13 && read_string(mini, text)) material.secondary_texture = std::move(text);
            }
            terrain.materials.push_back(std::move(material));
        }
    }
    terrain.raw_plane.assign(plane->payload.begin(), plane->payload.end());
    terrain.samples.reserve(cells);
    Reader reader(plane->payload, static_cast<std::size_t>(plane->payload_offset));
    for (std::size_t sample_index = 0; sample_index < cells; ++sample_index) {
        TerrainSample sample;
        const auto sample_offset = reader.absolute();
        if (!reader.i16(sample.height_sample) || !reader.u8(sample.material_slot) || !reader.u8(sample.vertex_intensity)) {
            issue(map, MapIssue::terrain_plane_size, "1/257/5", sample_offset, 4, "truncated terrain sample plane");
            return std::nullopt;
        }
        // A slot must index the declared material array, including when that
        // array is empty.
        if (sample.material_slot >= terrain.materials.size()) {
            issue(map, MapIssue::terrain_material_slot, "1/257/5", sample_offset + 2, 1,
                  "terrain sample " + std::to_string(sample_index) + " material slot " +
                  std::to_string(sample.material_slot) + " is outside " +
                  std::to_string(terrain.materials.size()) + " material descriptors");
            return std::nullopt;
        }
        terrain.samples.push_back(sample);
    }
    return terrain;
}


} // namespace

core::Result<Map> load_map(const std::span<const std::byte> bytes, Source source, const ObjectTypeCatalog& catalog) {
    if (bytes.size() > detail::max_file_size) return core::Result<Map>::failure(detail::error(source, diagnostic_codes::limit, "TED exceeds 512 MiB safety limit"));
    if (source.stored_size != 0 && source.stored_size != bytes.size()) return core::Result<Map>::failure(detail::error(source, diagnostic_codes::source_mismatch, "provenance size does not match supplied TED bytes"));
    Reader root_reader(bytes);
    std::uint32_t root_id{}, root_size{};
    if (!root_reader.u32(root_id) || !root_reader.u32(root_size)) return fail<Map>(source, "TED is missing special root header", 0, diagnostic_codes::truncated);
    if (root_id != 0) return fail<Map>(source, "TED special root ID must be zero", 0);
    std::span<const std::byte> root_payload;
    if (!root_reader.bytes(root_size, root_payload)) return fail<Map>(source, "TED special root payload crosses file bound", 0, diagnostic_codes::bounds);
    Chunk special{0, false, 0, 8, root_payload, {}};
    auto root_minis = detail::parse_minis(special, source); if (!root_minis) return core::Result<Map>::failure(root_minis.error());
    auto chunks = detail::parse_chunks(root_reader.rest(), source, root_reader.absolute());
    if (!chunks) return core::Result<Map>::failure(chunks.error());
    Map map; map.source = std::move(source);
    std::array<unsigned, 256> occurrences{};
    std::array<bool, 256> reported{};
    for (const auto& mini : root_minis.value()) ++occurrences[mini.type];
    std::optional<float> first_extent, second_extent;
    std::uint64_t first_extent_offset{}, second_extent_offset{};
    bool extents_malformed = false;
    for (const auto& mini : root_minis.value()) {
        map.root_fields.push_back(raw_field(mini));
        const bool duplicated = occurrences[mini.type] > 1;
        if ((mini.type == 0 || mini.type == 1) && duplicated) {
            return fail<Map>(map.source, "TED required header field is duplicated", mini.offset, diagnostic_codes::structure);
        }
        std::uint32_t value{};
        if (mini.type == 0 && read_u32(mini, value)) map.format_version = value;
        if (mini.type == 1 && read_u32(mini, value) && (value == 1 || value == 2)) map.kind = static_cast<MapKind>(value);
        std::string text;
        if ((mini.type == 8 || mini.type == 9 || mini.type == 10) && !decode_utf16z(mini.payload, text)) {
            return fail<Map>(map.source, "TED UTF-16 root field is malformed", mini.offset, diagnostic_codes::structure);
        }
        if (!known_root_field(mini.type)) {
            if (!std::exchange(reported[mini.type], true)) {
                issue(map, MapIssue::unknown_root_field, "root", mini.offset, mini.payload.size(),
                      "unknown root mini is bounded and retained raw", std::nullopt, mini.type);
            }
            continue;
        }
        const bool semantic = mini.type == 9 || mini.type == 0x10 || mini.type == 0x11;
        if (semantic && duplicated) {
            if (!std::exchange(reported[mini.type], true)) {
                issue(map, MapIssue::duplicate_root_field, "root", mini.offset, mini.payload.size(),
                      "optional root field is duplicated; no semantic value is chosen", std::nullopt, mini.type);
            }
            continue;
        }
        float extent{};
        if (mini.type == 9) map.context_name = std::move(text);
        else if (mini.type == 0x10) { if (read_f32(mini, extent) && std::isfinite(extent)) { first_extent = extent; first_extent_offset = mini.offset; } else extents_malformed = true; }
        else if (mini.type == 0x11) { if (read_f32(mini, extent) && std::isfinite(extent)) { second_extent = extent; second_extent_offset = mini.offset; } else extents_malformed = true; }
    }
    if (map.format_version != 0x0201U) return fail<Map>(map.source, "TED format version is unsupported", 8, diagnostic_codes::unsupported);
    if (!map.kind) return fail<Map>(map.source, "TED map kind is missing or unsupported", 8, diagnostic_codes::unsupported);
    if (first_extent && second_extent) {
        map.declared_extents = DeclaredExtents{0x10, *first_extent, 0x11, *second_extent,
                                               first_extent_offset, second_extent_offset};
    } else if (first_extent || second_extent || extents_malformed) {
        issue(map, MapIssue::declared_extents_invalid, "root", 8, root_size,
              "root extent pair 0x10/0x11 is incomplete, malformed or nonfinite; body bounds apply");
    }
    for (const auto& chunk : chunks.value()) map.chunks.push_back(raw_chunk(chunk));
    std::set<std::string> seen_paths;
    walk_schema(map, chunks.value(), {}, seen_paths);
    const auto main = std::find_if(chunks.value().begin(), chunks.value().end(), [](const Chunk& item) { return item.type == 1 && item.group; });
    if (main == chunks.value().end()) {
        issue(map, MapIssue::main_group_absent, "1", root_reader.absolute(), 0,
              "TED main group is absent; structural retention is not semantic completion");
        return core::Result<Map>::success(std::move(map));
    }
    extract_environment(map, *main);
    extract_volumes(map, *main);
    extract_placements(map, *main, catalog);

    // Kind 1 exactly predicts 1/257, 1/266 and 1/267.  A disagreement either
    // way is reported and never resolved by guessing the kind from the body.
    bool structure_agrees = true;
    for (const std::uint32_t id : {257U, 266U, 267U}) {
        const auto* group = direct(*main, id);
        const std::string path = "1/" + std::to_string(id);
        if (map.kind == MapKind::land && !group) {
            structure_agrees = false;
            issue(map, MapIssue::kind_structure_mismatch, path, main->header_offset, 0,
                  "land map lacks land group " + path);
        } else if (map.kind == MapKind::space && group) {
            structure_agrees = false;
            issue(map, MapIssue::kind_structure_mismatch, path, group->header_offset, group->payload.size(),
                  "space map carries land-only group " + path + "; it is retained raw only");
        }
    }
    if (map.kind == MapKind::land) {
        if (const auto* terrain_bundle = direct(*main, 257)) {
            map.terrain = extract_terrain(map, *terrain_bundle);
            extract_water(map, *terrain_bundle);
        }
        if (const auto* passability = direct(*main, 266)) {
            if (map.terrain && passability->payload.size() == map.terrain->samples.size() * 4U) {
                map.raw_passability.assign(passability->payload.begin(), passability->payload.end());
            } else {
                issue(map, MapIssue::passability_size, "1/266", passability->header_offset, passability->payload.size(),
                      map.terrain ? "passability plane size disagrees with terrain grid"
                                  : "passability plane is not validated: no terrain grid decoded");
            }
        }
        map.semantic_complete = structure_agrees && map.terrain.has_value() && !map.raw_passability.empty();
    } else {
        // Space maps still need the common environment/object bundles; they do
        // not gain a terrain semantic view merely because framing succeeded.
        map.semantic_complete = structure_agrees && direct(*main, 256) != nullptr && direct(*main, 258) != nullptr;
    }
    return core::Result<Map>::success(std::move(map));
}


} // namespace eawr::assets
