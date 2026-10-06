#include "scenario_internal.hpp"

namespace sim_headless::scenario_detail {

[[nodiscard]] core::Diagnostic problem(std::string message, const std::string& path) {
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
        if (const auto* initial = unit.get("apply_initial_pose"); initial != nullptr) {
            if (initial->kind != Json::Kind::boolean || (initial->flag && !entry.observed)) {
                return fail<Scenario>("apply_initial_pose is a boolean for observed units only", path);
            }
            entry.apply_initial_pose = initial->flag;
        }
        entry.position = *at;
        entry.facing_degrees = *facing;
        if (const auto* staging = unit.get("staging"); staging != nullptr && staging->array() != nullptr) {
            for (const auto& flag : *staging->array()) {
                entry.hold_fire = entry.hold_fire || text_of(&flag) == "hold_fire";
                entry.invulnerable = entry.invulnerable || text_of(&flag) == "invulnerable";
            }
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

core::Result<ScenarioRun> build_scenario(const std::filesystem::path& scenario_path,
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
    // DG-40: scenario-only table content leaves ordinary skirmish/replay tables unchanged.
    // Like hold_fire, a table override must agree for every occurrence of the affected type.
    std::map<tactical::TypeId, bool> protection;
    for (const auto& unit : scenario.units) {
        std::vector<std::string> affected{unit.type};
        const auto* type = tables.value().find(unit.type);
        if (type != nullptr) {
            for (const auto& member : type->members) affected.push_back(member.craft);
        }
        for (const auto& name : affected) {
            const auto id = eawr::skirmish::type_id(name);
            const auto [found, inserted] = protection.emplace(id, unit.invulnerable);
            if (!inserted && found->second != unit.invulnerable) {
                return fail<ScenarioRun>("invulnerable on '" + unit.label
                    + "' needs every " + name + " to agree, including squadron members", path);
            }
        }
    }
    for (auto& profile : durability.value().profiles) {
        const auto found = protection.find(profile.type_id);
        if (found != protection.end()) profile.scenario_invulnerable = found->second;
    }
    return execute_scenario(scenario, path, bytes, tables, sensors, durability, motion, combat, workers, build_sha256);
}

} // namespace sim_headless::scenario_detail
