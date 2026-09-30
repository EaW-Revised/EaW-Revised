#include "scenario.hpp"

#include "json.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/math/math.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace sim_headless {
namespace {

namespace core = eawr::core;
namespace math = eawr::sim::math;
namespace tactical = eawr::sim::tactical;
using math::Fixed;

[[nodiscard]] core::Diagnostic problem(std::string message, const std::string& path = {}) {
    return core::Diagnostic{
        .code = "EAWR-SIM-CLI-0003",
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = path.empty() ? std::optional<std::string>{} : std::optional<std::string>{path},
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
}

template <typename T>
[[nodiscard]] core::Result<T> fail(std::string message, const std::string& path = {}) {
    return core::Result<T>::failure(problem(std::move(message), path));
}

// --- Scenario model --------------------------------------------------------------------------

struct Pin {
    std::string archive;
    std::string path;
    std::string sha256;
};

struct ScenarioUnit {
    std::string label;
    std::string type;
    std::string owner;
    math::Vec3 position;
    Fixed facing_degrees;
    bool at_start{true}; // spawn "start"; otherwise "event"
    bool observed{};     // spawn "observed": binds to a unit the session creates (#75)
    bool hold_fire{};    // staging "hold_fire": the recorder's Prevent_All_Fire (#536)
};

struct ScenarioHardpoint {
    std::string label;
    std::string unit;
    std::string hardpoint; // XML hardpoint name
};

struct ScenarioEvent {
    std::uint64_t tick{};
    std::string action;
    std::string unit;
    std::string target; // attack: scenario unit label
    math::Vec3 position;
    std::string ability;
    Fixed amount;          // damage
    std::string hardpoint; // damage: the XML hardpoint name, empty for the hull
    std::vector<std::string> with; // move: further units of the same command (#599, a group move)
};

struct Scenario {
    std::vector<Pin> content;
    std::vector<std::pair<std::string, std::string>> players; // label, faction
    std::vector<ScenarioUnit> units;
    std::vector<ScenarioHardpoint> hardpoints;
    std::vector<ScenarioEvent> events;
    std::uint64_t duration{};
    bool fog_revealed{}; // staging.fog "revealed"
};

[[nodiscard]] std::optional<Fixed> decimal(const Json* json) {
    if (json == nullptr || json->digits() == nullptr) return std::nullopt;
    auto value = Fixed::from_decimal(*json->digits());
    if (!value) return std::nullopt;
    return value.value();
}

[[nodiscard]] std::optional<std::uint64_t> natural(const Json* json) {
    const auto value = decimal(json);
    if (!value || value->raw() < 0 || value->raw() % Fixed::scale != 0) return std::nullopt;
    return static_cast<std::uint64_t>(value->trunc_to_integer());
}

[[nodiscard]] std::optional<math::Vec3> position(const Json* json) {
    if (json == nullptr || json->array() == nullptr || json->array()->size() != 3) return std::nullopt;
    const auto& items = *json->array();
    const auto x = decimal(&items[0]);
    const auto y = decimal(&items[1]);
    const auto z = decimal(&items[2]);
    if (!x || !y || !z) return std::nullopt;
    return math::Vec3{*x, *y, *z};
}

[[nodiscard]] std::string text_of(const Json* json) {
    return json != nullptr && json->text() != nullptr ? *json->text() : std::string();
}

[[nodiscard]] core::Result<Scenario> read_scenario(const std::string& bytes, const std::string& path) {
    auto parsed = JsonReader(bytes).document();
    if (!parsed) return fail<Scenario>("not a JSON document", path);
    const auto& root = *parsed;
    if (text_of(root.get("format")) != "eawr-scenario" || !natural(root.get("format_version"))
        || *natural(root.get("format_version")) != 1) {
        return fail<Scenario>("not an eawr-scenario version 1 file", path);
    }
    Scenario scenario;
    const auto duration = natural(root.get("duration_ticks"));
    if (!duration || *duration == 0 || *duration > tactical::max_ticks) {
        return fail<Scenario>("duration_ticks must be a positive tick count", path);
    }
    scenario.duration = *duration;
    const auto* content = root.get("content");
    if (content == nullptr || content->array() == nullptr) return fail<Scenario>("content pins are missing", path);
    for (const auto& pin : *content->array()) {
        scenario.content.push_back({text_of(pin.get("archive")), text_of(pin.get("path")), text_of(pin.get("sha256"))});
        if (scenario.content.back().path.empty() || scenario.content.back().sha256.size() != 64) {
            return fail<Scenario>("a content pin needs a path and a SHA-256", path);
        }
    }
    if (const auto* staging = root.get("staging"); staging != nullptr) {
        scenario.fog_revealed = text_of(staging->get("fog")) == "revealed";
    }
    const auto* players = root.get("players");
    if (players == nullptr || players->array() == nullptr) return fail<Scenario>("players are missing", path);
    for (const auto& player : *players->array()) {
        scenario.players.emplace_back(text_of(player.get("label")), text_of(player.get("faction")));
    }
    const auto* units = root.get("units");
    if (units == nullptr || units->array() == nullptr) return fail<Scenario>("units are missing", path);
    for (const auto& unit : *units->array()) {
        ScenarioUnit entry;
        entry.label = text_of(unit.get("label"));
        entry.type = text_of(unit.get("type"));
        entry.owner = text_of(unit.get("owner"));
        const auto at = position(unit.get("position"));
        const auto facing = decimal(unit.get("facing_degrees"));
        if (entry.label.empty() || entry.type.empty() || !at || !facing) {
            return fail<Scenario>("unit '" + entry.label + "' needs a label, type, position and facing", path);
        }
        const auto spawn = text_of(unit.get("spawn"));
        if (spawn != "start" && spawn != "event" && spawn != "observed") {
            return fail<Scenario>("unit '" + entry.label + "': spawn must be 'start', 'event' or 'observed'", path);
        }
        entry.at_start = spawn == "start";
        entry.observed = spawn == "observed";
        entry.position = *at;
        entry.facing_degrees = *facing;
        if (const auto* staging = unit.get("staging"); staging != nullptr && staging->array() != nullptr) {
            for (const auto& flag : *staging->array()) entry.hold_fire = entry.hold_fire || text_of(&flag) == "hold_fire";
        }
        scenario.units.push_back(std::move(entry));
    }
    if (const auto* hardpoints = root.get("hardpoints"); hardpoints != nullptr && hardpoints->array() != nullptr) {
        for (const auto& hardpoint : *hardpoints->array()) {
            ScenarioHardpoint entry{text_of(hardpoint.get("label")), text_of(hardpoint.get("unit")),
                text_of(hardpoint.get("hardpoint"))};
            if (entry.label.empty() || entry.unit.empty() || entry.hardpoint.empty()) {
                return fail<Scenario>("a hardpoint needs a label, unit and XML hardpoint name", path);
            }
            scenario.hardpoints.push_back(std::move(entry));
        }
    }
    const auto* events = root.get("events");
    if (events == nullptr || events->array() == nullptr) return fail<Scenario>("events are missing", path);
    for (const auto& event : *events->array()) {
        ScenarioEvent entry;
        const auto tick = natural(event.get("tick"));
        entry.action = text_of(event.get("action"));
        entry.unit = text_of(event.get("unit"));
        entry.target = text_of(event.get("target"));
        entry.ability = text_of(event.get("ability"));
        if (!tick || *tick == 0 || *tick >= scenario.duration) {
            return fail<Scenario>("an event tick must be inside the scenario", path);
        }
        entry.tick = *tick;
        if (entry.action == "move" || entry.action == "face") {
            const auto at = position(event.get("position"));
            if (!at) return fail<Scenario>(entry.action + " needs a position", path);
            entry.position = *at;
        }
        // #599: a move given to several units by one command (a player's group move).
        if (const auto* with = event.get("with"); with != nullptr) {
            if (entry.action != "move" || with->array() == nullptr) {
                return fail<Scenario>("only a move takes 'with', a list of unit labels", path);
            }
            for (const auto& label : *with->array()) {
                entry.with.push_back(text_of(&label));
                if (entry.with.back().empty()) return fail<Scenario>("'with' lists unit labels", path);
            }
        }
        // #452: an attack-move or guard of a point (`position`) or of a unit (`target`).
        if (entry.action == "attack_move" || entry.action == "guard") {
            if (entry.target.empty()) {
                const auto at = position(event.get("position"));
                if (!at) return fail<Scenario>(entry.action + " needs a position or a target unit label", path);
                entry.position = *at;
            }
        }
        if (entry.action == "damage") {
            const auto amount = decimal(event.get("amount"));
            if (!amount || amount->raw() < 0) return fail<Scenario>("damage needs a nonnegative amount", path);
            entry.amount = *amount;
            entry.hardpoint = text_of(event.get("hardpoint"));
        }
        if (entry.action == "attack" && entry.target.empty()) {
            return fail<Scenario>("attack needs a target unit label", path);
        }
        scenario.events.push_back(std::move(entry));
    }
    return core::Result<Scenario>::success(std::move(scenario));
}

[[nodiscard]] std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

// docs/traces.md: SHA-256 of the pins as `archive|path|sha256\n` lines in byte order.
[[nodiscard]] std::string pins_identity(const std::vector<Pin>& pins) {
    std::vector<std::string> lines;
    for (const auto& pin : pins) lines.push_back(pin.archive + "|" + pin.path + "|" + pin.sha256 + "\n");
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const auto& line : lines) joined += line;
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size()));
}

[[nodiscard]] Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

// Staging (docs/traces.md "Scenario files"): the recorder faces each unit at a point 10000
// units along its facing, at the scenario's height, from the height the unit's layer puts it
// at. The rotation is that yaw, then the pitch that raises the nose toward the point.
[[nodiscard]] core::Result<math::Quat> staged_rotation(const Fixed facing_degrees, const Fixed rise) {
    auto yaw_turns = math::divide(facing_degrees, whole(360));
    auto pitch = math::atan2_turn(rise, whole(10000));
    if (!yaw_turns || !pitch) return core::Result<math::Quat>::failure(problem("staged facing is out of range"));
    auto half_yaw = math::divide(yaw_turns.value(), whole(2));
    auto half_pitch = math::divide(math::Fixed::from_raw(-pitch.value().raw()), whole(2));
    if (!half_yaw || !half_pitch) return core::Result<math::Quat>::failure(problem("staged facing is out of range"));
    const math::Quat yaw{Fixed{}, Fixed{}, math::sin_turn(half_yaw.value()), math::cos_turn(half_yaw.value())};
    const math::Quat nose{Fixed{}, math::sin_turn(half_pitch.value()), Fixed{}, math::cos_turn(half_pitch.value())};
    auto composed = math::compose(yaw, nose);
    if (!composed) return core::Result<math::Quat>::failure(composed.error());
    return math::normalize(composed.value());
}

// The craft of a squadron staged at the start, appended to the setup after its container: each
// on its Squadron_Offsets slot turned by the company's yaw, as the skirmish start places them
// (src/skirmish/start.cpp). Craft are labelled `<label>.<n>` in member order.
[[nodiscard]] core::Result<tactical::Squadron> squadron_craft(const eawr::units::UnitType& type,
    const eawr::units::UnitTables& tables, const tactical::UnitState& company, const Fixed facing_degrees,
    tactical::TacticalSetup& setup, const std::string& label, std::map<eawr::sim::EntityId, std::string>& labels) {
    using Squad = core::Result<tactical::Squadron>;
    tactical::Squadron squadron;
    squadron.container = company.entity_id;
    const auto turns = math::divide(facing_degrees, whole(360));
    if (!turns) return Squad::failure(turns.error());
    const auto cos = math::cos_turn(turns.value());
    const auto sin = math::sin_turn(turns.value());
    std::size_t number = 0;
    for (const auto& member : type.members) {
        const auto* craft = tables.find(member.craft);
        if (craft == nullptr) return fail<tactical::Squadron>("craft type " + member.craft + " is not in the unit tables");
        const math::Vec3 offset = member.offset.value_or(math::Vec3{});
        const auto xc = math::multiply(offset.x, cos);
        const auto ys = math::multiply(offset.y, sin);
        const auto xs = math::multiply(offset.x, sin);
        const auto yc = math::multiply(offset.y, cos);
        if (!xc || !ys || !xs || !yc) return fail<tactical::Squadron>(type.id + " craft offset overflows");
        const auto along = math::subtract(xc.value(), ys.value());
        const auto across = math::add(xs.value(), yc.value());
        if (!along || !across) return fail<tactical::Squadron>(type.id + " craft offset overflows");
        const auto x = math::add(company.position.x, along.value());
        const auto y = math::add(company.position.y, across.value());
        const auto z = math::add(company.position.z, offset.z);
        if (!x || !y || !z) return fail<tactical::Squadron>(type.id + " craft offset overflows");
        tactical::UnitState unit;
        unit.entity_id = static_cast<eawr::sim::EntityId>(setup.units.size() + 1);
        unit.type_id = eawr::skirmish::type_id(craft->id);
        unit.owner = company.owner;
        unit.position = {x.value(), y.value(), z.value()};
        unit.rotation = company.rotation;
        setup.units.push_back(unit);
        labels.emplace(unit.entity_id, label + "." + std::to_string(++number));
        squadron.members.push_back(unit.entity_id);
    }
    return Squad::success(std::move(squadron));
}

} // namespace

core::Result<ScenarioRun> run_scenario(const std::filesystem::path& scenario_path,
    const std::filesystem::path& game_root, const std::size_t workers, const std::string& build_sha256) {
    const auto path = scenario_path.string();
    std::ifstream input(scenario_path, std::ios::binary);
    if (!input) return fail<ScenarioRun>("could not open the scenario", path);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto scenario_result = read_scenario(bytes, path);
    if (!scenario_result) return core::Result<ScenarioRun>::failure(scenario_result.error());
    const auto& scenario = scenario_result.value();

    // FoC over base EaW, read-only, as --skirmish m2 mounts it.
    std::vector<eawr::vfs::MountSpec> specs;
    std::vector<eawr::vfs::LayerRoot> roots;
    roots.emplace_back("expansion", game_root / "corruption" / "Data");
    roots.emplace_back("base", game_root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) return core::Result<ScenarioRun>::failure(chain.error());
    for (auto& manifest : chain.value()) specs.push_back(std::move(manifest.mount));
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) return core::Result<ScenarioRun>::failure(filesystem.error());
    // docs/traces.md: a producer checks the files it loads against the pins first.
    for (const auto& pin : scenario.content) {
        auto file = filesystem.value().open(lower(pin.path));
        if (!file) return core::Result<ScenarioRun>::failure(file.error());
        const auto digest = eawr::sim::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(file.value().data()), file.value().size()));
        if (digest != pin.sha256) {
            return fail<ScenarioRun>("installed " + pin.path + " has SHA-256 " + digest + ", the scenario pins "
                + pin.sha256, path);
        }
    }
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    if (!catalog) return core::Result<ScenarioRun>::failure(catalog.error());
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput unit_input;
    unit_input.catalog = &catalog.value().catalog;
    unit_input.filesystem = &filesystem.value();
    unit_input.model = access.model;
    auto tables = eawr::units::load_unit_tables(unit_input);
    if (!tables) return core::Result<ScenarioRun>::failure(tables.error());
    // A scenario type outside the M2 fleet (the TIE Defender of S-01 to S-03) is loaded on top
    // of the pinned list; a scenario of pinned types keeps the pinned tables and their identity.
    std::vector<std::string> extra;
    for (const auto& unit : scenario.units) {
        if (tables.value().find(unit.type) == nullptr
            && std::find(extra.begin(), extra.end(), unit.type) == extra.end()) {
            extra.push_back(unit.type);
        }
    }
    if (!extra.empty()) {
        for (const auto type : eawr::units::pinned_m2_types()) unit_input.types.emplace_back(type);
        unit_input.types.insert(unit_input.types.end(), extra.begin(), extra.end());
        tables = eawr::units::load_unit_tables(unit_input);
        if (!tables) return core::Result<ScenarioRun>::failure(tables.error());
    }
    auto sensors = eawr::skirmish::sensor_table(tables.value());
    if (scenario.fog_revealed) {
        // The recorder reveals the map for every player: every scenario unit type sees the whole
        // map, so fog never hides a unit from targeting (space-targeting R-08).
        std::vector<tactical::TypeId> types;
        for (const auto& unit : scenario.units) {
            types.push_back(eawr::skirmish::type_id(unit.type));
            const auto* type = tables.value().find(unit.type);
            if (type == nullptr) continue;
            for (const auto& member : type->members) types.push_back(eawr::skirmish::type_id(member.craft)); // #561
        }
        sensors = eawr::skirmish::revealed_sensor_table(types);
    }
    auto durability = eawr::units::durability_table(tables.value());
    if (!durability) return core::Result<ScenarioRun>::failure(durability.error());
    auto motion = eawr::units::motion_table(tables.value());
    if (!motion) return core::Result<ScenarioRun>::failure(motion.error());
    auto combat = eawr::units::combat_table(tables.value());
    if (!combat) return core::Result<ScenarioRun>::failure(combat.error());
    // hold_fire (the recorder's Prevent_All_Fire): the held type's weapons reach nothing and may
    // fire at no category. It is a type-wide change, so every unit of a held type must hold.
    for (const auto& unit : scenario.units) {
        if (!unit.hold_fire) continue;
        for (const auto& other : scenario.units) {
            if (!other.hold_fire && lower(other.type) == lower(unit.type)) {
                return fail<ScenarioRun>("hold_fire on '" + unit.label + "' needs every " + unit.type + " to hold fire", path);
            }
        }
        for (auto& profile : combat.value().profiles) {
            if (profile.type_id != eawr::skirmish::type_id(unit.type)) continue;
            for (auto& weapon : profile.weapons) {
                weapon.range = Fixed{};
                weapon.category_restrictions = ~std::uint64_t{0};
            }
            profile.max_attack_distance = Fixed{};
        }
    }

    tactical::TacticalSetup setup;
    setup.content_identity = eawr::units::content_identity(tables.value());
    std::map<std::string, tactical::PlayerId> player_ids;
    for (const auto& [label, faction] : scenario.players) {
        const auto id = static_cast<tactical::PlayerId>(setup.players.size() + 1);
        player_ids.emplace(label, id);
        setup.players.push_back({id, id, eawr::skirmish::faction_id(faction), tactical::player_flag_commandable});
    }
    // A traced object: a unit, or a hardpoint under test (`weapon` is its combat weapon slot).
    struct Traced {
        std::string label;
        std::string unit; // the unit's label
        eawr::sim::EntityId id{};
        std::optional<Fixed> shield;
        std::optional<std::uint32_t> hardpoint;
        std::size_t weapon{};
        // #561: a squadron container, which the session gives no durability, writes its authored
        // Tactical_Health when its type has one (docs/traces.md); a container without one writes
        // the sums over its live craft (#457), as for its shield.
        std::optional<Fixed> authored_hull;
    };
    std::vector<Traced> traced;
    std::map<std::string, std::pair<eawr::sim::EntityId, tactical::PlayerId>> unit_ids;
    std::map<std::string, tactical::UnitState> pending_spawns;
    std::map<eawr::sim::EntityId, std::string> labels;
    // An observed slot binds once, in declaration order, to the first unlabelled live unit of its
    // type and owner in creation (stable ID) order, as the recorder binds its slots (#75).
    struct Observed {
        std::string label;
        tactical::TypeId type_id{};
        tactical::PlayerId owner{};
        bool bound{};
    };
    std::vector<Observed> observed;
    for (const auto& unit : scenario.units) {
        const auto owner = player_ids.find(unit.owner);
        const auto* type = tables.value().find(unit.type);
        if (owner == player_ids.end() || type == nullptr) {
            return fail<ScenarioRun>("unit '" + unit.label + "' names an unknown owner or unit type", path);
        }
        tactical::UnitState state;
        state.type_id = eawr::skirmish::type_id(unit.type);
        state.owner = owner->second;
        state.position = unit.position;
        // The unit's Space_Layer puts it Layer_Z_Adjust above the requested height.
        const auto adjust = type->movement.layer_z_adjust.value_or(Fixed{});
        auto staged_z = math::add(unit.position.z, adjust);
        if (!staged_z) return core::Result<ScenarioRun>::failure(staged_z.error());
        state.position.z = staged_z.value();
        auto rotation = staged_rotation(unit.facing_degrees, Fixed::from_raw(-adjust.raw()));
        if (!rotation) return core::Result<ScenarioRun>::failure(rotation.error());
        state.rotation = rotation.value();
        eawr::sim::EntityId id{};
        if (type->kind == eawr::units::UnitKind::squadron && !unit.at_start && !unit.observed) {
            return fail<ScenarioRun>("squadron '" + unit.label + "' must spawn at the start", path);
        }
        if (unit.observed) {
            observed.push_back({unit.label, state.type_id, state.owner, false}); // bound once created
        } else if (unit.at_start) {
            id = static_cast<eawr::sim::EntityId>(setup.units.size() + 1);
            state.entity_id = id;
            setup.units.push_back(state);
            labels.emplace(id, unit.label);
            if (type->kind == eawr::units::UnitKind::squadron) {
                // A squadron is its team container and its craft, each on its Squadron_Offsets slot
                // turned by the company's yaw, as the skirmish start places them (#75, #536).
                auto squadron = squadron_craft(*type, tables.value(), state, unit.facing_degrees, setup, unit.label, labels);
                if (!squadron) return core::Result<ScenarioRun>::failure(squadron.error());
                setup.squadrons.push_back(std::move(squadron).value());
            }
        } else {
            pending_spawns.emplace(unit.label, state); // staged at its spawn event
        }
        unit_ids.emplace(unit.label, std::pair{id, owner->second});
        const bool container = type->kind == eawr::units::UnitKind::squadron;
        traced.push_back({unit.label, unit.label, id, type->shield_points, std::nullopt, 0,
            container ? type->hull : std::nullopt});
    }
    // A hardpoint under test is a weapon slot of its unit's combat profile.
    for (const auto& hardpoint : scenario.hardpoints) {
        const auto unit = std::find_if(scenario.units.begin(), scenario.units.end(),
            [&](const ScenarioUnit& entry) { return entry.label == hardpoint.unit; });
        const auto* type = unit != scenario.units.end() ? tables.value().find(unit->type) : nullptr;
        if (type == nullptr) return fail<ScenarioRun>("hardpoint '" + hardpoint.label + "' names an unknown unit", path);
        const auto named = std::find_if(type->hardpoints.begin(), type->hardpoints.end(),
            [&](const eawr::units::Hardpoint& entry) { return lower(entry.id) == lower(hardpoint.hardpoint); });
        const auto* profile = combat.value().find(eawr::skirmish::type_id(unit->type));
        if (named == type->hardpoints.end() || profile == nullptr) {
            return fail<ScenarioRun>("hardpoint '" + hardpoint.label + "' is not a hardpoint of " + unit->type, path);
        }
        const auto index = static_cast<std::uint32_t>(named - type->hardpoints.begin());
        const auto slot = std::find_if(profile->weapons.begin(), profile->weapons.end(),
            [&](const tactical::WeaponProfile& weapon) { return weapon.hardpoint == index; });
        if (slot == profile->weapons.end()) {
            return fail<ScenarioRun>("hardpoint '" + hardpoint.label + "' is not a weapon", path);
        }
        traced.push_back({hardpoint.label, hardpoint.unit, unit_ids.at(hardpoint.unit).first, std::nullopt, index,
            static_cast<std::size_t>(slot - profile->weapons.begin()), std::nullopt});
    }
    std::sort(traced.begin(), traced.end(), [](const Traced& a, const Traced& b) { return a.label < b.label; });

    // #76: the recorder quick-loads the map, so no player is human (AB-41).
    auto abilities = eawr::units::ability_table(tables.value());
    if (!abilities) return core::Result<ScenarioRun>::failure(abilities.error());
    auto session_result = tactical::TacticalSession::create(setup, sensors, durability.value(), motion.value(),
        std::nullopt, combat.value(), tactical::VictoryRules{}, abilities.value());
    if (!session_result) return core::Result<ScenarioRun>::failure(session_result.error());
    auto session = std::move(session_result).value();
    ScenarioRun run;
    std::map<tactical::PlayerId, std::uint64_t> sequence;
    // Spawns and removals are staged between ticks (TacticalSession::stage_spawn), so the unit is
    // alive, or gone, from the event's tick on, as in the recordings.
    std::multimap<std::uint64_t, const ScenarioEvent*> staged;
    // Orders are submitted once their tick's spawns are staged, so an order can name a unit, or
    // an attack target, spawned at or before its tick. A tick-T command runs in the step after
    // tick T either way, so an order between start units runs as if it were submitted up front.
    std::vector<const ScenarioEvent*> orders;
    for (const auto& event : scenario.events) {
        if (!unit_ids.contains(event.unit)) return fail<ScenarioRun>("event names an unknown unit '" + event.unit + "'", path);
        if (event.action == "spawn" || event.action == "remove") {
            if ((event.action == "spawn") != pending_spawns.contains(event.unit)) {
                return fail<ScenarioRun>("spawn events are for spawn 'event' units only ('" + event.unit + "')", path);
            }
            staged.emplace(event.tick, &event);
            continue;
        }
        if ((event.action == "attack" || !event.target.empty()) && !unit_ids.contains(event.target)) {
            return fail<ScenarioRun>(event.action + " names an unknown target '" + event.target + "'", path);
        }
        for (const auto& label : event.with) {
            if (!unit_ids.contains(label)) return fail<ScenarioRun>("move names an unknown unit '" + label + "'", path);
        }
        orders.push_back(&event);
    }
    std::size_t next_order = 0;
    const auto submit_orders = [&](const std::uint64_t completed) -> core::Result<void> {
        for (; next_order < orders.size() && orders[next_order]->tick <= completed; ++next_order) {
            const auto& event = *orders[next_order];
            const auto& unit = unit_ids.at(event.unit);
            if (unit.first == 0) {
                return fail<void>("an order names '" + event.unit + "' before its spawn event", path);
            }
            tactical::PlayerCommand command;
            command.key = {event.tick, unit.second, sequence[unit.second]++};
            command.units = {unit.first};
            for (const auto& label : event.with) {
                const auto other = unit_ids.at(label).first;
                if (other == 0) return fail<void>("a move names '" + label + "' before its spawn event", path);
                command.units.push_back(other);
            }
            if (event.action == "move") {
                command.payload = tactical::MovePayload{event.position};
            } else if (event.action == "face") {
                command.payload = tactical::FacePayload{event.position};
            } else if (event.action == "stop") {
                command.payload = tactical::StopPayload{};
            } else if (event.action == "attack") {
                const auto target = unit_ids.at(event.target).first;
                if (target == 0) {
                    return fail<void>("attack names target '" + event.target + "' before its spawn event", path);
                }
                command.payload = tactical::AttackPayload{target};
            } else if (event.action == "attack_move" || event.action == "guard") {
                eawr::sim::EntityId target = eawr::sim::invalid_entity_id;
                if (!event.target.empty()) {
                    target = unit_ids.at(event.target).first;
                    if (target == 0) {
                        return fail<void>(event.action + " names target '" + event.target + "' before its spawn event", path);
                    }
                }
                if (event.action == "guard") {
                    command.payload = tactical::GuardPayload{event.position, target};
                } else {
                    command.payload = tactical::AttackMovePayload{event.position, target};
                }
            } else if (event.action == "damage") {
                // The retail Lua Take_Damage(amount[, hardpoint]) as the scripted-damage command (HD-30).
                const auto staged_unit = std::find_if(scenario.units.begin(), scenario.units.end(),
                    [&](const ScenarioUnit& entry) { return entry.label == event.unit; });
                const auto* type = tables.value().find(staged_unit->type);
                auto hardpoint = tactical::hull_target;
                if (!event.hardpoint.empty()) {
                    const auto named = std::find_if(type->hardpoints.begin(), type->hardpoints.end(),
                        [&](const eawr::units::Hardpoint& entry) { return lower(entry.id) == lower(event.hardpoint); });
                    if (named == type->hardpoints.end()) {
                        return fail<void>("damage names hardpoint '" + event.hardpoint + "', not one of " + staged_unit->type, path);
                    }
                    hardpoint = static_cast<std::uint32_t>(named - type->hardpoints.begin());
                }
                command.payload = tactical::DamagePayload{event.amount, hardpoint};
            } else if (event.action == "ability") {
                // The recorder's Lua Activate_Ability(name, true) as an ability command (#76, AB-10).
                const auto kind = tactical::ability_kind(event.ability);
                if (kind == tactical::AbilityKind::none) {
                    run.warnings.push_back("tick " + std::to_string(event.tick) + ": ability " + event.ability + " on "
                        + event.unit + " is not modelled (space-abilities.md AB-03) and was skipped");
                    continue;
                }
                tactical::AbilityPayload ability{kind, tactical::AbilityAction::activate};
                // #561: a targeted ability (ION_CANNON_SHOT, space-abilities AB-61) at a scenario unit.
                if (!event.target.empty()) {
                    ability.target = unit_ids.at(event.target).first;
                    if (ability.target == 0) {
                        return fail<void>("ability names target '" + event.target + "' before its spawn event", path);
                    }
                }
                command.payload = ability;
            } else if (event.action == "ability_probe") {
                continue; // a read-only probe of the recorder
            } else {
                return fail<void>("event action '" + event.action + "' is not staged by sim_headless yet", path);
            }
            auto submitted = session.submit(command);
            if (!submitted) return submitted;
        }
        return core::Result<void>::success();
    };

    std::ostringstream trace;
    std::ostringstream hashes;
    trace << "tick,object,field,value\n";
    hashes << "tick,sha256\n";
    const auto rows = [&](const std::uint64_t tick) -> core::Result<void> {
        const auto units = session.units();
        const auto snapshot = session.snapshot();
        for (const auto& object : traced) {
            const auto id = unit_ids.at(object.unit).first;
            const auto found = std::find_if(units.begin(), units.end(),
                [&](const tactical::UnitState& unit) { return id != 0 && unit.entity_id == id; });
            const bool alive = found != units.end();
            if (object.hardpoint) {
                // A hardpoint row on every tick its unit is alive: the shots the tick fired and its
                // target (an attack order's, else its opportunity target).
                if (!alive) continue;
                std::uint64_t shots = 0;
                for (const auto& event : snapshot->combat_events()) {
                    shots += event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == id
                        && event.weapon == *object.hardpoint;
                }
                std::string target;
                if (const auto state = session.combat_state(id)) {
                    auto target_id = state->direct ? state->attack_target : eawr::sim::EntityId{};
                    if (target_id == 0) target_id = state->weapons[object.weapon].opportunity.target;
                    if (target_id != 0) {
                        const auto label = labels.find(target_id);
                        target = label != labels.end() ? label->second : "entity." + std::to_string(target_id);
                    }
                }
                trace << tick << ',' << object.label << ",shots," << shots << '\n';
                trace << tick << ',' << object.label << ",target," << target << '\n';
                continue;
            }
            trace << tick << ',' << object.label << ",alive," << (alive ? 1 : 0) << '\n';
            if (!alive) continue;
            const auto matrix = math::to_matrix(found->rotation, found->position);
            if (!matrix) return core::Result<void>::failure(matrix.error());
            const auto& m = matrix.value().rows;
            trace << tick << ',' << object.label << ",fwd.x," << m[0][0].raw() << '\n'
                  << tick << ',' << object.label << ",fwd.y," << m[1][0].raw() << '\n'
                  << tick << ',' << object.label << ",fwd.z," << m[2][0].raw() << '\n';
            // #457 (project): a squadron's team container has no health of its own; unless its type
            // authors one (`authored_hull`, #561), its rows carry the sums over its live craft, so the
            // trace stays complete for every unit.
            const auto squadron = std::find_if(session.squadrons().begin(), session.squadrons().end(),
                [&](const tactical::Squadron& entry) { return entry.container == id; });
            const bool container = squadron != session.squadrons().end();
            std::int64_t crew_hull = 0;
            std::int64_t crew_shield = 0;
            if (container) {
                for (const auto member : squadron->members) {
                    if (const auto health = session.durability_state(member)) {
                        crew_hull += health->hull.raw();
                        crew_shield += health->shields.raw();
                    }
                }
            }
            if (const auto health = session.durability_state(id)) {
                trace << tick << ',' << object.label << ",hull," << health->hull.raw() << '\n';
            } else if (object.authored_hull) {
                trace << tick << ',' << object.label << ",hull," << object.authored_hull->raw() << '\n';
            } else if (container) {
                trace << tick << ',' << object.label << ",hull," << crew_hull << '\n';
            }
            trace << tick << ',' << object.label << ",pos.x," << found->position.x.raw() << '\n'
                  << tick << ',' << object.label << ",pos.y," << found->position.y.raw() << '\n'
                  << tick << ',' << object.label << ",pos.z," << found->position.z.raw() << '\n';
            // A shielded unit writes the session's shield (#74); a type the session gives no shield
            // writes its authored Shield_Points.
            if (object.shield) {
                const auto health = session.durability_state(id);
                const auto* profile = session.durability().find(found->type_id);
                const bool modelled = session.durability().damage && health && profile != nullptr
                    && profile->max_shields.raw() > 0;
                trace << tick << ',' << object.label << ",shield,"
                      << (modelled ? health->shields.raw() : object.shield->raw()) << '\n';
            } else if (container) {
                trace << tick << ',' << object.label << ",shield," << crew_shield << '\n';
            }
        }
        return core::Result<void>::success();
    };
    // The combat log (#536): names by label, a craft `<squadron>.<n>`, else `entity.<id>`;
    // hardpoints by their XML name. Health rows are written when the value changes.
    std::ostringstream combat_log;
    combat_log << "tick,kind,object,part,other,value\n";
    std::map<tactical::TypeId, const eawr::units::UnitType*> types_by_id;
    for (const auto& type : tables.value().units) types_by_id.emplace(eawr::skirmish::type_id(type.id), &type);
    std::map<std::pair<eawr::sim::EntityId, std::string>, std::int64_t> last_health;
    std::map<eawr::sim::EntityId, tactical::TypeId> live_before;
    const auto name_of = [&](const eawr::sim::EntityId id) {
        const auto label = labels.find(id);
        return label != labels.end() ? label->second : "entity." + std::to_string(id);
    };
    // A weapon's index names its hardpoint or the object weapon; a hit's, its hardpoint or the hull.
    const auto part_of = [&](const tactical::TypeId type, const std::uint32_t index, const char* none) -> std::string {
        if (index == tactical::no_hardpoint) return none;
        const auto found = types_by_id.find(type);
        if (found == types_by_id.end() || index >= found->second->hardpoints.size()) return "hp." + std::to_string(index);
        return found->second->hardpoints[index].id;
    };
    const auto decimal_text = [](const Fixed value) {
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(4);
        text << static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale);
        return text.str();
    };
    const auto combat_rows = [&](const std::uint64_t tick) {
        std::map<eawr::sim::EntityId, tactical::TypeId> live;
        for (const auto& unit : session.units()) live.emplace(unit.entity_id, unit.type_id);
        for (const auto& [id, type] : live_before) {
            if (!live.contains(id)) combat_log << tick << ",destroyed," << name_of(id) << ",,,\n";
        }
        const auto type_of = [&](const eawr::sim::EntityId id) {
            const auto found = live.find(id);
            if (found != live.end()) return found->second;
            const auto before = live_before.find(id);
            return before != live_before.end() ? before->second : tactical::TypeId{};
        };
        for (const auto& event : session.snapshot()->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired) {
                combat_log << tick << ",fired," << name_of(event.shooter) << ','
                           << part_of(type_of(event.shooter), event.weapon, "object") << ',' << name_of(event.target) << ",1\n";
            } else if (event.kind == tactical::CombatEventKind::projectile_hit) {
                combat_log << tick << ",hit," << name_of(event.target) << ','
                           << part_of(type_of(event.target), event.target_hardpoint, "hull") << ',' << name_of(event.shooter)
                           << '/' << part_of(type_of(event.shooter), event.weapon, "object") << ',' << event.outcome << '\n';
            }
        }
        for (const auto& [id, type] : live) {
            const auto health = session.durability_state(id);
            if (!health) continue;
            const auto note = [&](const std::string& part, const Fixed value) {
                auto [slot, inserted] = last_health.try_emplace({id, part}, value.raw());
                if (!inserted && slot->second == value.raw()) return;
                slot->second = value.raw();
                combat_log << tick << ",health," << name_of(id) << ',' << part << ",," << decimal_text(value) << '\n';
            };
            note("hull", health->hull);
            note("shield", health->shields);
            for (std::size_t index = 0; index < health->hardpoints.size(); ++index) {
                note(part_of(type, static_cast<std::uint32_t>(index), "hull"), health->hardpoints[index]);
            }
        }
        live_before = std::move(live);
    };
    const auto bind_observed = [&]() {
        if (observed.empty()) return;
        for (const auto& unit : session.units()) {
            // A staged squadron's craft carry `<squadron>.<n>`; a slot of that name traces one (#457).
            if (const auto named = labels.find(unit.entity_id); named != labels.end()) {
                for (auto& slot : observed) {
                    if (slot.bound || slot.label != named->second || slot.type_id != unit.type_id
                        || slot.owner != unit.owner) continue;
                    slot.bound = true;
                    unit_ids.at(slot.label).first = unit.entity_id;
                    break;
                }
                continue;
            }
            for (auto& slot : observed) {
                if (slot.bound || slot.type_id != unit.type_id || slot.owner != unit.owner) continue;
                slot.bound = true;
                unit_ids.at(slot.label).first = unit.entity_id;
                labels.emplace(unit.entity_id, slot.label);
                break;
            }
        }
    };
    bind_observed();
    auto first = rows(0);
    combat_rows(0);
    if (!first) return core::Result<ScenarioRun>::failure(first.error());
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    while (session.completed_tick() + 1 < scenario.duration) {
        auto tick = session.step(executor);
        if (!tick) return core::Result<ScenarioRun>::failure(tick.error());
        const auto completed = tick.value().completed_tick;
        for (auto [event, end] = staged.equal_range(completed); event != end; ++event) {
            const auto& action = *event->second;
            auto& entry = unit_ids.at(action.unit);
            if (action.action == "spawn") {
                auto spawned = session.stage_spawn(pending_spawns.at(action.unit));
                if (!spawned) return core::Result<ScenarioRun>::failure(spawned.error());
                entry.first = spawned.value();
                labels.emplace(spawned.value(), action.unit);
            } else {
                auto removed = session.stage_remove(entry.first);
                if (!removed) return core::Result<ScenarioRun>::failure(removed.error());
            }
            run.staged = true;
        }
        bind_observed();
        auto submitted = submit_orders(completed);
        if (!submitted) return core::Result<ScenarioRun>::failure(submitted.error());
        // The hash row is the stepped state; a staged change shows in the next row.
        hashes << completed << ',' << tick.value().state_sha256 << '\n';
        auto written = rows(completed);
        combat_rows(completed);
        if (!written) return core::Result<ScenarioRun>::failure(written.error());
    }
    run.trace_csv = trace.str();
    run.combat_csv = combat_log.str();
    run.hashes_csv = hashes.str();
    std::ostringstream header;
    header << "{\n"
           << "  \"format\": \"eawr-trace\",\n"
           << "  \"format_version\": 1,\n"
           << "  \"source\": \"remake\",\n"
           << "  \"content_identity\": \"" << pins_identity(scenario.content) << "\",\n"
           << "  \"tick_seconds\": {\"numerator\": " << tactical::tick_numerator
           << ", \"denominator\": " << tactical::tick_denominator << "},\n"
           << "  \"build_identity\": {\"kind\": \"executable-sha256\", \"value\": \"" << build_sha256 << "\"},\n"
           << "  \"scenario_sha256\": \""
           << eawr::sim::sha256_hex(std::span<const std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()))
           << "\"\n"
           << "}\n";
    run.trace_header = header.str();
    auto replay = session.record();
    auto encoded = tactical::write_replay(replay);
    if (!encoded) return core::Result<ScenarioRun>::failure(encoded.error());
    run.replay = std::move(encoded).value();
    return core::Result<ScenarioRun>::success(std::move(run));
}

} // namespace sim_headless
