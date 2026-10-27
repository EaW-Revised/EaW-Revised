#include "live_session_view.hpp"
#include "live_session_internal.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/scene/scene.hpp"
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace eawr::presentation::godot_backend {
namespace {
using namespace godot;
using sim::math::Fixed;
namespace tactical = sim::tactical;

double number(const Variant& value) {
    if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT)
        throw std::runtime_error("land bindings: expected a number");
    const double result = value;
    if (!std::isfinite(result)) throw std::runtime_error("land bindings: non-finite number");
    return result;
}
std::uint32_t integer(const Variant& value) {
    const double result = number(value);
    if (result < 0 || result > std::numeric_limits<std::uint32_t>::max() || std::floor(result) != result)
        throw std::runtime_error("land bindings: expected an unsigned integer");
    return static_cast<std::uint32_t>(result);
}
Fixed fixed(const Variant& value) {
    const double result = number(value) * Fixed::scale;
    if (result < 0 || result > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2))
        throw std::runtime_error("land bindings: physical value outside range");
    return Fixed::from_raw(static_cast<std::int64_t>(std::llround(result)));
}
Dictionary dictionary(const Variant& value) {
    if (value.get_type() != Variant::DICTIONARY) throw std::runtime_error("land bindings: expected an object");
    return value;
}
Array array(const Variant& value, int size = -1) {
    if (value.get_type() != Variant::ARRAY) throw std::runtime_error("land bindings: expected an array");
    const Array result = value;
    if (size >= 0 && result.size() != size) throw std::runtime_error("land bindings: incorrect array size");
    return result;
}
std::string string(const Variant& value) {
    if (value.get_type() != Variant::STRING) throw std::runtime_error("land bindings: expected a string");
    return String(value).utf8().get_data();
}

// R2/R3 and LR3-10 are explicit unverified gates. Physical locomotion,
// footprints and formation spacing always come from the authored L2b adapter.
skirmish::LandContentInputs read_bindings(const std::filesystem::path& path) {
    std::error_code code;
    const auto bytes = std::filesystem::file_size(path, code);
    if (code || bytes > 8 * 1024 * 1024) throw std::runtime_error("cannot read land bindings within byte limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read land bindings");
    std::string text((std::istreambuf_iterator<char>(input)), {});
    Ref<JSON> json; json.instantiate();
    if (json->parse(String::utf8(text.c_str())) != OK) throw std::runtime_error("invalid land bindings JSON");
    const auto root = dictionary(json->get_data());
    if (string(root["format"]) != "eawr-land-inputs-v1") throw std::runtime_error("unknown land bindings format");
    skirmish::LandContentInputs result;
    const auto masks = dictionary(root["unverified_passability_bits"]);
    const auto mask_keys = masks.keys();
    for (int64_t i = 0; i < mask_keys.size(); ++i) {
        const auto key = mask_keys[i];
        result.unverified_passability_bits.push_back({string(key), integer(masks[key])});
    }
    const auto collisions = dictionary(root["collision_class_order"]);
    const auto collision_keys = collisions.keys();
    for (int64_t i = 0; i < collision_keys.size(); ++i) {
        const auto key = collision_keys[i];
        result.collision_class_order.push_back({string(key), integer(collisions[key])});
    }
    const auto contacts = array(root["contacts"], 4);
    result.contacts = {integer(contacts[0]), integer(contacts[1]), fixed(contacts[2]), fixed(contacts[3])};
    const auto capacities = array(root["capacities"]);
    for (int64_t i = 0; i < capacities.size(); ++i) {
        const auto row = array(capacities[i], 2);
        if (!result.last_known_capacity.emplace(integer(row[0]), integer(row[1])).second)
            throw std::runtime_error("duplicate land capacity player");
    }
    const auto occupations = array(root["occupations"]);
    for (int64_t i = 0; i < occupations.size(); ++i) {
        const auto row = dictionary(occupations[i]);
        tactical::LandOccupation occupation;
        occupation.object = integer(row["object"]);
        occupation.primary_passability = integer(row["primary_passability"]);
        occupation.secondary_passability = integer(row["secondary_passability"]);
        const auto primary = array(row["primary_cells"]), secondary = array(row["secondary_cells"]);
        for (int64_t cell = 0; cell < primary.size(); ++cell) occupation.primary_cells.push_back(integer(primary[cell]));
        for (int64_t cell = 0; cell < secondary.size(); ++cell) occupation.secondary_cells.push_back(integer(secondary[cell]));
        result.occupations.push_back(std::move(occupation));
    }
    return result;
}

} // namespace

bool LiveSessionView::prepare_land(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    std::string_view map_path, std::string& failure) {
    try {
        if (options_.fixture != "skirmish" || options_.lan || options_.borrowed_battle)
            throw std::runtime_error("land live view requires a standalone skirmish start");
        if (options_.skirmish.map && live_session_detail::lower_path(*options_.skirmish.map) != live_session_detail::lower_path(map_path))
            throw std::runtime_error("land skirmish map and viewed map differ");
        if (!options_.skirmish.slots) throw std::runtime_error("land live view requires explicit human --eawr-skirmish-slots");
        skirmish::Fixture fixture;
        fixture.map = map_path; fixture.slots = *options_.skirmish.slots;
        fixture.seed = options_.skirmish.seed.value_or(0); fixture.match = options_.skirmish.match;
        if (std::any_of(fixture.slots.begin(), fixture.slots.end(), [](const auto& slot) { return !slot.human; }))
            throw std::runtime_error("land live view P1 requires human lobby slots");
        units::UnitTables empty;
        auto discovered = skirmish::read_land_start_inputs(fixture, filesystem, catalog, empty);
        if (!discovered) throw std::runtime_error(core::format_diagnostic(discovered.error()));
        fixture.map_sha256 = discovered.value().common.map_sha256;
        scene::VfsAssetCache cache(filesystem);
        units::LoadInput load;
        load.domain = tactical::BattleDomain::land; load.catalog = &catalog; load.filesystem = &filesystem;
        load.model = cache.access().model;
        std::set<std::string> requested;
        for (const auto& slot : fixture.slots) {
            const auto faction = std::find_if(discovered.value().factions.begin(), discovered.value().factions.end(),
                [&](const auto& row) { return row.faction == slot.faction; });
            if (faction != discovered.value().factions.end()) requested.insert(faction->forces.begin(), faction->forces.end());
        }
        if (requested.empty()) throw std::runtime_error("selected land factions have no authored starting companies");
        load.types.assign(requested.begin(), requested.end());
        auto tables = units::load_unit_tables(load);
        if (!tables) throw std::runtime_error(core::format_diagnostic(tables.error()));
        for (const auto& company : tables.value().land_companies)
            for (const auto& member : company.members) requested.insert(member.type);
        load.types.assign(requested.begin(), requested.end());
        tables = units::load_unit_tables(load);
        if (!tables) throw std::runtime_error(core::format_diagnostic(tables.error()));
        auto inputs = skirmish::read_land_start_inputs(fixture, filesystem, catalog, tables.value());
        if (!inputs) throw std::runtime_error(core::format_diagnostic(inputs.error()));
        // No bindings file (menu start): the authored adapter supplies the gates; the file is an override.
        skirmish::LandContentInputs bindings;
        if (options_.land_bindings.empty()) {
            auto authored = skirmish::authored_land_content_inputs(tables.value());
            if (!authored) throw std::runtime_error(core::format_diagnostic(authored.error()));
            bindings = std::move(authored.value());
        } else bindings = read_bindings(options_.land_bindings);
        auto start = skirmish::build_land_start(fixture, inputs.value(), bindings);
        if (!start) throw std::runtime_error(core::format_diagnostic(start.error()));
        start_ = std::move(start.value().start);
        setup_ = start_->setup;
        land_content_ = std::move(start.value().content);
        content_.emplace(); // Land P1 has no combat, fog, economy UI or AI services.
        economy_ = land_content_->economy; victory_ = land_content_->victory;
        tables_ = std::move(tables).value();
        player_ = options_.player.value_or(fixture.slots.front().slot);
        if (std::none_of(setup_->players.begin(), setup_->players.end(), [&](const auto& p) { return p.player_id == player_; }))
            throw std::runtime_error("land local player is not in the start");
        for (const auto& player : setup_->players) team_of_player_[player.player_id] = player.team_id;
        options_.reveal = true; // Land fog presentation belongs to L6 P2.
        shield_flash_constants_ = presentation_constants::load_flash(filesystem);
        return true;
    } catch (const std::exception& error) { failure = error.what(); return false; }
}

void LiveSessionView::bind_land_roster(const platform::LiveSession& session) {
    const auto zero = session.frame().latest;
    for (const auto& instance : zero->instances()) owner_of_entity_[instance.entity_id] = instance.owner;
    for (const auto& [member, root] : session.land_roster()) {
        squadron_of_[member] = root;
        squadron_members_[root].push_back(member);
        const auto instances = zero->instances();
        const auto found = std::lower_bound(instances.begin(), instances.end(), member,
            [](const auto& instance, auto id) { return instance.entity_id < id; });
        if (found == instances.end() || found->entity_id != member) continue;
        const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
            [&](const auto& row) { return skirmish::type_id(row.id) == found->type_id; });
        if (type == tables_->units.end()) continue;
        BattleScene::PlacedUnit placed;
        placed.object_id = type->id; placed.live_entity = member;
        placed.position = {live_session_detail::to_float(found->fixed_transform.rows[0][3]), live_session_detail::to_float(found->fixed_transform.rows[1][3]),
            live_session_detail::to_float(found->fixed_transform.rows[2][3])};
        placed.yaw_degrees = static_cast<float>(live::instance_yaw_degrees(found->fixed_transform));
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& p) { return p.player.player_id == found->owner; });
        if (owner != start_->players.end() && owner->colour) placed.team_colour = owner->colour->rgb;
        ship_of_entity_[member] = placed_ships_.size();
        placed_ships_.push_back(std::move(placed));
    }
}

} // namespace eawr::presentation::godot_backend
