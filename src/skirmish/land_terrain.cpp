#include "eawr/skirmish/land_terrain.hpp"
#include "eawr/core/sha256.hpp"

#include <bit>
#include <limits>
#include <memory>
#include <utility>

namespace eawr::skirmish {
namespace {
using sim::math::Fixed;
core::Diagnostic invalid(const char* message) {
    core::Diagnostic diagnostic;
    diagnostic.code = sim::tactical::diagnostic_codes::invalid_land_terrain;
    diagnostic.message = message;
    return diagnostic;
}
std::optional<std::uint32_t> word(const assets::RawField& field) {
    if (field.bytes.size() != 4) return std::nullopt;
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result |= std::to_integer<std::uint32_t>(field.bytes[i]) << (8U * i);
    return result;
}
// Exact binary32 to Q24, rounded half to even, without host rounding-mode dependence.
std::optional<Fixed> fixed_bits(const std::uint32_t bits) {
    const auto exponent = (bits >> 23U) & 255U;
    if (exponent == 255U) return std::nullopt;
    std::uint64_t magnitude = (bits & 0x7FFFFFU) | (exponent ? 0x800000U : 0U);
    const auto shift = static_cast<int>(exponent ? exponent : 1U) - 126;
    if (shift >= 0) {
        if (shift >= 63 || magnitude > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) >> shift))
            return std::nullopt;
        magnitude <<= shift;
    } else if (shift <= -64) magnitude = 0;
    else {
        const auto right = static_cast<unsigned>(-shift);
        const auto remainder = magnitude & ((std::uint64_t{1} << right) - 1);
        magnitude >>= right;
        const auto half = std::uint64_t{1} << (right - 1);
        if (remainder > half || (remainder == half && (magnitude & 1U))) ++magnitude;
    }
    const auto raw = static_cast<std::int64_t>(magnitude);
    return Fixed::from_raw(bits >> 31U ? -raw : raw);
}
}

core::Result<sim::tactical::LandTerrain> load_land_terrain(const assets::Map& map,
    std::vector<sim::tactical::LandMovementMask> movement_masks,
    const std::optional<sim::tactical::LandBounds> map_bounds) {
    using Result = core::Result<sim::tactical::LandTerrain>;
    if (map.kind != assets::MapKind::land || !map.terrain)
        return Result::failure(invalid("land terrain requires a decoded land map"));
    const auto& source = *map.terrain;
    const auto count = static_cast<std::uint64_t>(source.width) * source.height;
    if (count != source.samples.size() || count > std::numeric_limits<std::size_t>::max() / 4
        || map.raw_passability.size() != count * 4)
        return Result::failure(invalid("land map passability plane disagrees with terrain"));
    sim::tactical::LandTerrainInput input;
    input.width = source.width; input.height = source.height;
    // TED encoding constants, not bounds inferred from stock maps (asset-formats,
    // land terrain sample plane; WLEN-39). No floating point enters the simulation.
    input.spacing = Fixed::from_raw(20 * Fixed::scale);
    input.movement_masks = std::move(movement_masks);
    input.map_bounds = map_bounds;
    std::vector<std::int32_t> materials;
    for (const auto& material : source.materials) {
        std::int32_t surface = -1;
        for (const auto& field : material.fields) if (field.id == 0x14) {
            const auto value = word(field);
            if (!value) return Result::failure(invalid("land material surface is malformed"));
            surface = std::bit_cast<std::int32_t>(*value); // WLEN-32; custom integer values survive.
        }
        materials.push_back(surface);
    }
    input.heights.reserve(source.samples.size()); input.surfaces.reserve(source.samples.size());
    input.passability.reserve(source.samples.size());
    for (std::size_t i = 0; i < source.samples.size(); ++i) {
        const auto& sample = source.samples[i];
        if (sample.material_slot >= materials.size())
            return Result::failure(invalid("land material slot is outside the map table"));
        const auto height = Fixed::from_raw(static_cast<std::int64_t>(sample.height_sample) * 25 * (Fixed::scale / 512));
        input.heights.push_back(height); input.surfaces.push_back(materials[sample.material_slot]);
        std::uint32_t bits = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            bits |= std::to_integer<std::uint32_t>(map.raw_passability[4 * i + byte]) << (8U * byte);
        input.passability.push_back(bits);
    }
    for (const auto& record : map.water_records) if (record.chunk_path == "1/257/0") {
        std::optional<Fixed> elevation;
        std::optional<std::uint32_t> mode;
        for (const auto& field : record.fields) {
            if (field.id != 0x15 && field.id != 0x16) continue;
            const auto value = word(field);
            if (!value) return Result::failure(invalid("land water header is malformed"));
            if (field.id == 0x16) mode = *value;
            else {
                elevation = fixed_bits(*value);
                if (!elevation) return Result::failure(invalid("land water height is not representable"));
            }
        }
        if (mode && *mode != 0 && !elevation)
            return Result::failure(invalid("land water mode requires an elevation"));
        if (mode) input.water = sim::tactical::LandWaterPlane{elevation.value_or(Fixed{}), *mode};
    }
    return sim::tactical::LandTerrain::build(std::move(input));
}

core::Result<void> bind_land_terrain(sim::tactical::TacticalContent& content,
    sim::tactical::TacticalSetup& setup, const assets::Map& map,
    std::vector<sim::tactical::LandMovementMask> movement_masks,
    const std::optional<sim::tactical::LandBounds> map_bounds) {
    using sim::tactical::BattleDomain;
    if (setup.domain != content.domain) return core::Result<void>::failure(invalid("terrain binding domain mismatch"));
    if (setup.domain == BattleDomain::space) return core::Result<void>::success();
    if (content.land_terrain) return core::Result<void>::failure(invalid("land terrain is already bound"));
    auto terrain = load_land_terrain(map, std::move(movement_masks), map_bounds);
    if (!terrain) return core::Result<void>::failure(terrain.error());
    const auto digest = terrain.value().identity();
    std::vector<std::uint8_t> identity(setup.content_identity.begin(), setup.content_identity.end());
    constexpr std::string_view domain = "land_terrain";
    identity.insert(identity.end(), domain.begin(), domain.end());
    identity.insert(identity.end(), {1, 0, 0, 0});
    identity.insert(identity.end(), digest.begin(), digest.end());
    auto field = std::make_shared<const sim::tactical::LandTerrain>(std::move(terrain).value());
    setup.content_identity = core::sha256(identity);
    content.land_terrain = std::move(field);
    return core::Result<void>::success();
}
} // namespace eawr::skirmish
