// P2-02 (#65) scan of the pinned M2 fleet: loads the FoC unit tables from an
// installation read-only and lists each selected unit with every unresolved
// field or reference, the notes and the replay content identity. Nothing from
// the installation is written; the optional JSON report goes where --report
// points (keep it under the ignored out/).

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using eawr::units::Fixed;

struct Arguments final {
    std::filesystem::path game_root;
    std::filesystem::path report;
    bool notes{};
    bool hardpoints{};
    bool movement{};
    bool all_types{};
    bool combat{};
};

void usage() {
    std::cerr << "usage: unit_scan --game-root <EaW install> [--report <out.json>] [--notes] [--hardpoints] [--movement] [--all-types] [--combat]\n"
                 "  FoC profile: <root>/corruption/Data over <root>/GameData/Data.\n";
}

std::optional<Arguments> arguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--game-root" && index + 1 < argc) {
            result.game_root = argv[++index];
        } else if (flag == "--report" && index + 1 < argc) {
            result.report = argv[++index];
        } else if (flag == "--notes") {
            result.notes = true;
        } else if (flag == "--hardpoints") {
            result.hardpoints = true;
        } else if (flag == "--movement") {
            result.movement = true;
        } else if (flag == "--all-types") {
            result.all_types = true;
        } else if (flag == "--combat") {
            result.combat = true;
        } else {
            return std::nullopt;
        }
    }
    if (result.game_root.empty()) return std::nullopt;
    return result;
}

std::string decimal(const Fixed value) {
    const std::int64_t raw = value.raw();
    const bool negative = raw < 0;
    const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(raw + 1)) + 1U : static_cast<std::uint64_t>(raw);
    std::uint64_t whole = magnitude >> 24U;
    std::uint64_t fraction = ((magnitude & 0xffffffU) * 1'000'000U + 0x800000U) >> 24U;
    if (fraction == 1'000'000U) {
        ++whole;
        fraction = 0;
    }
    std::string text = std::to_string(fraction);
    text = std::string(6 - text.size(), '0') + text;
    while (!text.empty() && text.back() == '0') text.pop_back();
    return (negative ? "-" : "") + std::to_string(whole) + (text.empty() ? "" : "." + text);
}

std::string decimal(const std::optional<Fixed>& value) {
    return value ? decimal(*value) : std::string("-");
}

std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        default:
            if (character < 0x20U || character >= 0x7fU) {
                output << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

std::string hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

// Owners whose rows belong to a unit: the unit, its hardpoints, the
// projectiles its weapons fire and its targeting priority set.
std::set<std::string> owners(const eawr::units::UnitType& unit) {
    std::set<std::string> result{unit.id};
    if (unit.weapon) result.insert(unit.weapon->projectile);
    for (const auto& hardpoint : unit.hardpoints) {
        result.insert(hardpoint.id);
        if (hardpoint.weapon) result.insert(hardpoint.weapon->projectile);
    }
    return result;
}

std::string summary(const eawr::units::UnitType& unit) {
    std::ostringstream output;
    output << unit.id << " [" << eawr::units::to_string(unit.kind) << (unit.affiliation.empty() ? "" : ", " + unit.affiliation)
           << "]";
    if (unit.kind == eawr::units::UnitKind::squadron) {
        output << " members " << unit.members.size();
        if (!unit.members.empty()) output << " x " << unit.members.front().craft;
    } else {
        std::size_t targetable = 0;
        std::size_t positioned = 0;
        for (const auto& hardpoint : unit.hardpoints) {
            targetable += hardpoint.targetable ? 1U : 0U;
            positioned += hardpoint.attachment.position || hardpoint.fire_a.position ? 1U : 0U;
        }
        output << " hull " << decimal(unit.hull) << " shield " << decimal(unit.shield_points) << " power "
               << decimal(unit.ai_combat_power) << " sensor " << decimal(eawr::units::sensor_range(unit)) << " hardpoints " << unit.hardpoints.size() << " (targetable "
               << targetable << ", positioned " << positioned << ")";
    }
    for (const auto& ability : unit.abilities) output << " ability " << ability.type;
    if (unit.spawner) {
        output << " spawns";
        for (const auto& entry : unit.spawner->starting) output << " " << entry.count << "x" << entry.squadron;
        output << " after " << decimal(unit.spawner->delay_seconds) << "s (reserves unused)";
    }
    if (!unit.lua_script.empty()) output << " lua " << unit.lua_script;
    return output.str();
}

std::string position(const eawr::units::BonePoint& point) {
    if (point.bone.empty()) return "-";
    if (!point.position) return point.bone + " unresolved";
    const auto& value = *point.position;
    std::string facing;
    if (point.axes) {
        const auto& x = (*point.axes)[0];
        facing = " x-axis (" + decimal(x.x) + ", " + decimal(x.y) + ", " + decimal(x.z) + ")";
    }
    return point.bone + " (" + decimal(value.x) + ", " + decimal(value.y) + ", " + decimal(value.z) + ")" + facing +
           (point.from_attached_model ? " via attached model" : "");
}

void write_rows(std::ostream& output, const std::vector<eawr::units::Unresolved>& rows) {
    output << '[';
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        output << (index == 0 ? "" : ",") << "\n    {\"owner\": " << json(row.owner) << ", \"field\": " << json(row.field)
               << ", \"value\": " << json(row.value) << ", \"reason\": " << json(row.reason) << '}';
    }
    output << (rows.empty() ? "]" : "\n  ]");
}

} // namespace

int main(const int argc, char** argv) {
    const auto args = arguments(argc, argv);
    if (!args) {
        usage();
        return 2;
    }
    const std::filesystem::path base = args->game_root / "GameData" / "Data";
    const std::filesystem::path expansion = args->game_root / "corruption" / "Data";
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, root] : {std::pair{std::string("expansion"), expansion}, std::pair{std::string("base"), base}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root);
        if (!manifest) {
            std::cerr << eawr::core::format_diagnostic(manifest.error()) << '\n';
            return 1;
        }
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) {
        std::cerr << eawr::core::format_diagnostic(filesystem.error()) << '\n';
        return 1;
    }
    auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    if (!loaded) {
        std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
        return 1;
    }
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &loaded.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    input.digest = access.sha256;
    if (args->all_types) {
        // Every effective game object that authors HardPoints, instead of the pinned M2 fleet.
        for (const auto& definition : loaded.value().catalog.definitions()) {
            if (!definition.winner || definition.category != eawr::data::Category::game_object) continue;
            auto effective = loaded.value().catalog.resolve(definition.id);
            if (!effective) continue;
            const auto* hardpoints = effective.value().value("HardPoints");
            if (hardpoints != nullptr && !hardpoints->value.raw_text.empty()) input.types.push_back(definition.id);
        }
    }
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) {
        std::cerr << eawr::core::format_diagnostic(tables.error()) << '\n';
        return 1;
    }
    const auto& value = tables.value();
    const auto identity = hex(eawr::units::content_identity(value));

    std::set<std::size_t> printed;
    std::cout << "units " << value.units.size() << ", projectiles " << value.projectiles.size() << ", priority sets "
              << value.priority_sets.size() << ", damage/armor rows " << value.constants.damage_to_armor.size()
              << ", unresolved " << value.unresolved.size() << ", notes " << value.notes.size() << '\n';
    for (const auto& unit : value.units) {
        std::cout << summary(unit) << '\n';
        if (args->hardpoints) {
            for (const auto& hardpoint : unit.hardpoints) {
                std::cout << "  " << hardpoint.id << ' ' << eawr::units::to_string(hardpoint.type) << " at "
                          << position(hardpoint.attachment) << ", fire " << position(hardpoint.fire_a) << " / "
                          << position(hardpoint.fire_b) << '\n';
            }
            for (const auto& bone : unit.target_bones) std::cout << "  target " << position(bone) << '\n';
        }
        if (args->movement && unit.kind != eawr::units::UnitKind::squadron) {
            const auto& move = unit.movement;
            std::cout << "  movement max_speed " << decimal(move.max_speed) << " min_speed " << decimal(move.min_speed)
                      << " rate_of_turn " << decimal(move.max_rate_of_turn) << " rate_of_roll "
                      << decimal(move.max_rate_of_roll) << " bank " << decimal(move.bank_turn_angle) << " thrust "
                      << decimal(move.max_thrust) << " lift " << decimal(move.max_lift) << " accel "
                      << decimal(move.acceleration) << " decel " << decimal(move.deceleration) << " layer "
                      << (move.space_layer.empty() ? "-" : move.space_layer) << " z_adjust "
                      << decimal(move.layer_z_adjust) << '\n';
            for (const auto& ability : unit.abilities) {
                std::cout << "  ability " << ability.type << " expires " << decimal(ability.expiration_seconds)
                          << "s recharge " << decimal(ability.recharge_seconds) << 's';
                for (const auto& modifier : ability.modifiers) {
                    std::cout << ' ' << modifier.modifier << ' ' << decimal(modifier.value);
                }
                std::cout << '\n';
            }
        }
        const auto mine = owners(unit);
        for (std::size_t index = 0; index < value.unresolved.size(); ++index) {
            const auto& row = value.unresolved[index];
            if (!mine.contains(row.owner)) continue;
            printed.insert(index);
            std::cout << "  unresolved " << row.owner << " " << row.field << (row.value.empty() ? "" : " '" + row.value + "'")
                      << ": " << row.reason << '\n';
        }
    }
    for (std::size_t index = 0; index < value.unresolved.size(); ++index) {
        if (printed.contains(index)) continue;
        const auto& row = value.unresolved[index];
        std::cout << "unresolved " << row.owner << " " << row.field << (row.value.empty() ? "" : " '" + row.value + "'")
                  << ": " << row.reason << '\n';
    }
    if (args->notes) {
        for (const auto& row : value.notes) {
            std::cout << "note " << row.owner << " " << row.field << (row.value.empty() ? "" : " '" + row.value + "'")
                      << ": " << row.reason << '\n';
        }
    }
    if (args->movement) {
        const auto motion = eawr::units::motion_table(value);
        if (!motion) {
            std::cout << "motion table: " << motion.error().message << '\n';
        } else {
            std::cout << "motion rules arc " << decimal(motion.value().rules.arc_degrees) << " deg expansion "
                      << decimal(motion.value().rules.expansion_distance) << '\n';
            for (const auto& unit : value.units) {
                const auto* profile = motion.value().find(eawr::assets::object_type_crc(unit.id));
                if (profile == nullptr) continue;
                std::cout << "motion " << unit.id << " speed " << decimal(profile->max_speed) << " accel "
                          << decimal(profile->acceleration) << " decel " << decimal(profile->deceleration) << " turn "
                          << decimal(profile->rate_of_turn) << " deg slowdown " << decimal(profile->turn_in_place_slowdown)
                          << '\n';
            }
        }
    }
    if (args->combat) {
        // Each weapon's fire frame as combat_table builds it (per-axis normalized; the +90 degree
        // turn permutes components exactly and changes no dot product), its distance from
        // orthonormal against frame_tolerance_raw, then validate_combat on the whole table.
        std::size_t frames = 0;
        std::int64_t worst_length = 0;
        std::int64_t worst_skew = 0;
        for (const auto& unit : value.units) {
            for (const auto& hardpoint : unit.hardpoints) {
                if (!hardpoint.weapon) continue;
                const auto& bone = hardpoint.fire_a.position ? hardpoint.fire_a : hardpoint.attachment;
                if (!bone.axes) continue;
                std::array<eawr::sim::math::Vec3, 3> axes{};
                bool usable = true;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    auto normal = eawr::sim::math::normalize((*bone.axes)[axis]);
                    usable = usable && normal;
                    if (normal) axes[axis] = normal.value();
                }
                if (!usable) continue;
                ++frames;
                std::int64_t length = 0;
                std::int64_t skew = 0;
                for (std::size_t left = 0; left < 3; ++left) {
                    for (std::size_t right = left; right < 3; ++right) {
                        const auto product = eawr::sim::math::dot(axes[left], axes[right]);
                        const std::int64_t error = product ? std::llabs(product.value().raw() - (left == right ? Fixed::scale : 0))
                                                   : std::int64_t{Fixed::scale};
                        auto& worst = left == right ? length : skew;
                        worst = std::max(worst, error);
                    }
                }
                worst_length = std::max(worst_length, length);
                worst_skew = std::max(worst_skew, skew);
                if (std::max(length, skew) > eawr::sim::tactical::frame_tolerance_raw) {
                    std::cout << "fire frame outside tolerance: " << unit.id << ' ' << hardpoint.id << ' ' << bone.bone
                              << " raw error unit length " << length << ", orthogonality " << skew << '\n';
                }
            }
        }
        std::cout << "fire frames " << frames << ", worst raw error unit length " << worst_length << ", orthogonality "
                  << worst_skew << " (tolerance " << eawr::sim::tactical::frame_tolerance_raw << ")\n";
        const auto combat = eawr::units::combat_table(value);
        std::cout << "combat table: "
                  << (combat ? "valid, " + std::to_string(combat.value().profiles.size()) + " profiles" : combat.error().message)
                  << '\n';
    }
    std::cout << "content identity " << identity << '\n';

    if (!args->report.empty()) {
        std::ofstream output(args->report, std::ios::binary);
        output << "{\n  \"schema\": \"eawr-unit-scan-v1\",\n  \"profile\": \"foc\",\n  \"content_identity\": " << json(identity)
               << ",\n  \"units\": [";
        for (std::size_t index = 0; index < value.units.size(); ++index) {
            output << (index == 0 ? "" : ",") << "\n    {\"id\": " << json(value.units[index].id)
                   << ", \"summary\": " << json(summary(value.units[index])) << '}';
        }
        output << "\n  ],\n  \"unresolved\": ";
        write_rows(output, value.unresolved);
        output << ",\n  \"notes\": ";
        write_rows(output, value.notes);
        output << ",\n  \"inputs\": [";
        for (std::size_t index = 0; index < value.inputs.size(); ++index) {
            const auto& file = value.inputs[index];
            output << (index == 0 ? "" : ",") << "\n    {\"path\": " << json(file.logical_path) << ", \"source\": "
                   << json(file.source_id) << ", \"layer\": " << json(file.layer_id) << ", \"sha256\": " << json(file.sha256)
                   << '}';
        }
        output << "\n  ]\n}\n";
        if (!output) {
            std::cerr << "cannot write " << args->report.string() << '\n';
            return 1;
        }
    }
    return 0;
}
