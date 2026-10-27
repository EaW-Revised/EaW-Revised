#include "galactic_internal.hpp"

#include "eawr/core/sha256.hpp"
#include "../xml_internal.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace eawr::data::galactic {
namespace detail {
namespace {

[[nodiscard]] bool space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

} // namespace

std::string trimmed(const std::string_view text) { return trim(text); }

std::string lower(const std::string_view text) { return ascii_lower(text); }

std::vector<std::string> tokens(const std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char item : text) {
        if (item == ',' || space(item)) {
            if (!current.empty()) result.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(item);
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

Record::Record(std::string record_owner, std::vector<const Definition*> record_layers, std::vector<Issue>& issues)
    : owner_(std::move(record_owner)), layers_(std::move(record_layers)), issues_(&issues) {
    if (!layers_.empty() && layers_.front() != nullptr) file_ = layers_.front()->root.source.logical_path;
}

Record::Record(std::string record_owner, const XmlNode& root, std::string source_file, std::vector<Issue>& issues)
    : owner_(std::move(record_owner)), file_(std::move(source_file)), own_(Definition{}), issues_(&issues) {
    own_->id = owner_;
    own_->root = root;
    layers_.push_back(&*own_);
}

const XmlNode* Record::node(const std::string_view tag) const { return retail::single(layers_, tag); }

std::string Record::text(const std::string_view tag) const {
    const auto* found = node(tag);
    return found == nullptr ? std::string{} : trim(found->raw_text);
}

std::optional<bool> Record::boolean(const std::string_view tag) const {
    const auto* found = node(tag);
    return found == nullptr ? std::nullopt : retail::boolean(found->raw_text);
}

std::optional<Fixed> Record::parse_number(const XmlNode& at, const std::string_view raw, const std::string_view tag) const {
    if (tag == "Hyperspace_Speed_Factor" || tag == "Corruption_Hyperspace_Bonus"
        || tag == "Object_Max_Speed_Multiplier_Galactic" || tag == "MaximumFleetMovementDistance") {
        const auto parsed = retail::float_value(raw);
        return parsed ? std::optional(parsed->value) : std::nullopt;
    }
    const auto parsed = retail::leading_number(raw);
    if (!parsed) {
        issue(IssueKind::bad_value, &at, std::string(tag), trim(raw), "not a number in Q24 range");
        return std::nullopt;
    }
    if (parsed->trailing_text) {
        issue(IssueKind::trailing_text, &at, std::string(tag), trim(raw), "read as its leading number (AU-82)");
    }
    return parsed->value;
}

std::optional<Fixed> Record::number(const std::string_view tag) const {
    const auto* found = node(tag);
    return found == nullptr ? std::nullopt : parse_number(*found, found->raw_text, tag);
}

std::optional<std::int64_t> Record::whole(const std::string_view tag) const {
    const auto* found = node(tag);
    if (found == nullptr) return std::nullopt;
    const auto value = parse_number(*found, found->raw_text, tag);
    if (!value) return std::nullopt;
    if (value->raw() % Fixed::scale != 0) {
        issue(IssueKind::bad_value, found, std::string(tag), trim(found->raw_text), "not a whole number");
        return std::nullopt;
    }
    return value->raw() / Fixed::scale;
}

std::optional<Position> Record::position(const std::string_view tag) const {
    const auto* found = node(tag);
    if (found == nullptr) return std::nullopt;
    const auto row = tokens(found->raw_text);
    if (row.size() != 3) {
        issue(IssueKind::bad_value, found, std::string(tag), trim(found->raw_text), "expected x, y, z");
        return std::nullopt;
    }
    std::array<Fixed, 3> values{};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto value = parse_number(*found, row[index], tag);
        if (!value) return std::nullopt;
        values[index] = *value;
    }
    return Position{values[0], values[1], values[2]};
}

std::vector<const XmlNode*> Record::records(const std::string_view tag) const {
    return retail::list(layers_, tag, retail::ListRule::append);
}

std::vector<std::string> Record::names(const std::string_view tag) const {
    const auto* found = node(tag);
    return found == nullptr ? std::vector<std::string>{} : tokens(found->raw_text);
}

std::vector<FactionNumber> Record::faction_numbers(const std::string_view tag) const {
    std::vector<FactionNumber> result;
    for (const auto* at : records(tag)) {
        const auto row = tokens(at->raw_text);
        if (row.size() != 2) {
            issue(IssueKind::bad_value, at, std::string(tag), trim(at->raw_text), "expected faction, number");
            continue;
        }
        const auto value = parse_number(*at, row[1], tag);
        if (!value) continue;
        if (value->raw() % Fixed::scale != 0) {
            issue(IssueKind::bad_value, at, std::string(tag), trim(at->raw_text), "not a whole number");
            continue;
        }
        result.push_back({row[0], value->raw() / Fixed::scale});
    }
    return result;
}

std::vector<FactionName> Record::faction_names(const std::string_view tag) const {
    std::vector<FactionName> result;
    for (const auto* at : records(tag)) {
        const auto row = tokens(at->raw_text);
        if (row.size() != 2) {
            issue(IssueKind::bad_value, at, std::string(tag), trim(at->raw_text), "expected faction, name");
            continue;
        }
        result.push_back({row[0], row[1]});
    }
    return result;
}

std::vector<PlacedType> Record::placed_types(const std::string_view tag) const {
    std::vector<PlacedType> result;
    for (const auto* at : records(tag)) {
        const auto row = tokens(at->raw_text);
        if (row.size() != 3) {
            issue(IssueKind::bad_value, at, std::string(tag), trim(at->raw_text), "expected faction, location, type");
            continue;
        }
        result.push_back({row[0], row[1], row[2]});
    }
    return result;
}

void Record::unknown_tags() const {
    if (layers_.empty() || layers_.front() == nullptr) return;
    for (const auto& child : layers_.front()->root.children) {
        if (child.schema_status != SchemaStatus::unknown) continue;
        issue(IssueKind::unknown_tag, &child, child.name, trim(child.raw_text), "the tag is not known for this class");
    }
}

void Record::issue(const IssueKind kind, const XmlNode* at, std::string tag, std::string value, std::string reason) const {
    Issue row;
    row.kind = kind;
    row.file = at != nullptr ? at->source.logical_path : file_;
    row.line = at != nullptr ? at->source.line : 0;
    row.owner = owner_;
    row.tag = std::move(tag);
    row.value = std::move(value);
    row.reason = std::move(reason);
    issues_->push_back(std::move(row));
}

std::vector<const Definition*> layers_of(const Catalog& catalog, const Definition& definition,
    std::vector<Issue>& issues) {
    auto resolved = catalog.resolve(definition.id, definition.category);
    if (!resolved) {
        Issue row;
        row.kind = IssueKind::dangling_reference;
        row.file = definition.root.source.logical_path;
        row.line = definition.root.source.line;
        row.owner = definition.id;
        row.tag = "Variant_Of_Existing_Type";
        row.reason = resolved.error().message;
        issues.push_back(std::move(row));
        return {&definition};
    }
    std::vector<const Definition*> layers;
    for (const auto& name : resolved.value().chain) {
        const auto* layer = catalog.find(name, definition.category);
        if (layer != nullptr) layers.push_back(layer);
    }
    if (layers.empty()) layers.push_back(&definition);
    return layers;
}

void record_input(GalacticData& data, const SourceLocation& source, std::string sha256) {
    for (const auto& input : data.inputs) {
        if (input.logical_path == source.logical_path) return;
    }
    data.inputs.push_back({source.logical_path, source.source_id, source.layer_id, std::move(sha256)});
}

void catalog_input(const Catalog& catalog, GalacticData& data, const SourceLocation& source) {
    std::string sha256;
    for (const auto& file : catalog.registry_files()) {
        if (file.source && ascii_iequals(file.source->logical_path, source.logical_path) && file.input_sha256) {
            sha256 = *file.input_sha256;
            break;
        }
    }
    record_input(data, source, std::move(sha256));
}

} // namespace detail

namespace {

template <typename T>
[[nodiscard]] const T* find_named(const std::vector<T>& values, const std::string_view name) noexcept {
    for (const auto& value : values) {
        if (ascii_iequals(value.name, name)) return &value;
    }
    return nullptr;
}

// Little-endian, length-framed, as the unit tables' identity encoding.
class Encoder final {
public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void u32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void i64(const std::int64_t value) {
        const auto bits = static_cast<std::uint64_t>(value);
        for (unsigned shift = 0; shift < 64; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    void text(const std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const char item : value) bytes_.push_back(static_cast<std::uint8_t>(item));
    }
    void texts(const std::vector<std::string>& values) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) text(value);
    }
    void flag(const std::optional<bool>& value) { u8(!value ? 0 : (*value ? 2 : 1)); }
    void fixed(const std::optional<Fixed>& value) {
        u8(value ? 1 : 0);
        if (value) i64(value->raw());
    }
    void whole(const std::optional<std::int64_t>& value) {
        u8(value ? 1 : 0);
        if (value) i64(*value);
    }
    void position(const std::optional<Position>& value) {
        u8(value ? 1 : 0);
        if (!value) return;
        i64(value->x.raw());
        i64(value->y.raw());
        i64(value->z.raw());
    }
    template <typename T, typename F>
    void each(const std::vector<T>& values, F&& encode) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) encode(value);
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

void encode(Encoder& out, const std::vector<FactionNumber>& values) {
    out.each(values, [&](const FactionNumber& value) { out.text(value.faction); out.i64(value.value); });
}

void encode(Encoder& out, const std::vector<FactionName>& values) {
    out.each(values, [&](const FactionName& value) { out.text(value.faction); out.text(value.name); });
}

void encode(Encoder& out, const std::vector<PlacedType>& values) {
    out.each(values, [&](const PlacedType& value) {
        out.text(value.faction); out.text(value.location); out.text(value.type);
    });
}

} // namespace

std::string_view to_string(const IssueKind kind) noexcept {
    switch (kind) {
    case IssueKind::unknown_tag: return "unknown_tag";
    case IssueKind::bad_value: return "bad_value";
    case IssueKind::trailing_text: return "trailing_text";
    case IssueKind::dangling_reference: return "dangling_reference";
    case IssueKind::duplicate_record: return "duplicate_record";
    case IssueKind::missing_required: return "missing_required";
    }
    return "bad_value";
}

const Campaign* GalacticData::campaign(const std::string_view name) const noexcept { return find_named(campaigns, name); }
const Planet* GalacticData::planet(const std::string_view name) const noexcept { return find_named(planets, name); }
const Planet* GalacticData::galaxy_art_object(const std::string_view name) const noexcept {
    return find_named(galaxy_art_objects, name);
}
bool Planet::has_planetary_behavior() const noexcept {
    const auto contains = [](const auto& names) {
        return std::any_of(names.begin(), names.end(), [](const auto& name) { return ascii_iequals(name, "PLANET"); });
    };
    return contains(behaviors) || contains(galactic_behaviors);
}
const TradeRoute* GalacticData::trade_route(const std::string_view name) const noexcept {
    return find_named(trade_routes, name);
}
const Faction* GalacticData::faction(const std::string_view name) const noexcept { return find_named(factions, name); }

const Constant* GalacticData::constant(const std::string_view tag) const noexcept {
    for (const auto& value : constants) {
        if (ascii_iequals(value.tag, tag)) return &value;
    }
    return nullptr;
}

core::Result<GalacticData> load_galactic_data(const vfs::Vfs& filesystem, const Catalog& catalog) {
    GalacticData data;
    detail::load_campaigns(catalog, data);
    detail::load_planets(catalog, data);
    detail::load_factions(catalog, data);
    auto routes = detail::load_trade_routes(filesystem, data);
    if (!routes) return core::Result<GalacticData>::failure(routes.error());
    detail::load_constants(filesystem, data);
    detail::check_references(catalog, data);
    for (const auto& registry : catalog.registry_roots()) {
        if (registry.source && registry.input_sha256 && (registry.category == Category::campaign
                || registry.category == Category::faction || registry.category == Category::game_object)) {
            detail::record_input(data, *registry.source, *registry.input_sha256);
        }
    }
    std::sort(data.inputs.begin(), data.inputs.end(),
        [](const InputFile& left, const InputFile& right) { return left.logical_path < right.logical_path; });
    return core::Result<GalacticData>::success(std::move(data));
}

std::array<std::uint8_t, 32> content_identity(const GalacticData& data) {
    Encoder out;
    out.text("eawr-galactic-data-v5");
    out.each(data.campaigns, [&](const Campaign& campaign) {
        out.text(campaign.name);
        out.text(campaign.campaign_set);
        out.whole(campaign.sort_order);
        out.text(campaign.text_id);
        out.text(campaign.description_text);
        out.whole(campaign.minimum_custom_tech);
        out.whole(campaign.maximum_custom_tech);
        for (const auto& flag : {campaign.is_listed, campaign.is_multiplayer, campaign.is_quickmatch_allowed,
                 campaign.supports_custom_settings, campaign.show_completed_tab, campaign.tutorial,
                 campaign.is_story_campaign, campaign.planet_auto_reveal, campaign.is_autoresolve_allowed}) {
            out.flag(flag);
        }
        out.fixed(campaign.camera_shift_x);
        out.fixed(campaign.camera_shift_y);
        out.fixed(campaign.camera_distance);
        out.texts(campaign.locations);
        out.texts(campaign.planets);
        out.texts(campaign.trade_routes);
        encode(out, campaign.home_locations);
        out.text(campaign.starting_active_player);
        encode(out, campaign.starting_credits);
        encode(out, campaign.starting_tech_levels);
        encode(out, campaign.max_tech_levels);
        encode(out, campaign.starting_forces);
        encode(out, campaign.ai_player_control);
        encode(out, campaign.markup_filenames);
        out.text(campaign.rebel_story_name);
        out.text(campaign.empire_story_name);
        out.text(campaign.underworld_story_name);
        encode(out, campaign.story_names);
        out.texts(campaign.autoresolve_exclusion_locations);
        out.each(campaign.corruption_level_overrides, [&](const LocationLevel& value) {
            out.text(value.location); out.i64(value.level);
        });
        out.texts(campaign.human_victory_conditions);
        out.texts(campaign.ai_victory_conditions);
        out.texts(campaign.human_victory_planet_names);
        out.whole(campaign.ai_victory_cycle_limit);
        out.whole(campaign.human_victory_credit_target);
        out.whole(campaign.ai_victory_credit_target);
        out.whole(campaign.human_victory_cycle_limit);
        out.fixed(campaign.human_victory_galactic_control);
        out.fixed(campaign.ai_victory_galactic_control);
        out.texts(campaign.human_victory_faction_names);
        out.texts(campaign.ai_victory_faction_names);
        out.texts(campaign.human_victory_leader_names);
        out.texts(campaign.ai_victory_leader_names);
        out.texts(campaign.ai_victory_planet_names);
        encode(out, campaign.special_case_production);
    });
    const auto encode_planet = [&](const Planet& planet) {
        out.text(planet.name);
        out.position(planet.galactic_position);
        out.fixed(planet.galactic_influence_range);
        out.each(planet.system_spies, [&](const SystemSpyDefinition& spy) {
            out.text(spy.name); out.text(spy.activation_style);
            out.flag(spy.initially_enabled); out.flag(spy.causes_despawn);
            out.fixed(spy.duration); out.fixed(spy.influence_range); out.whole(spy.mask);
        });
        out.fixed(planet.political_control);
        out.text(planet.force_alignment); out.fixed(planet.force_strength);
        out.text(planet.political_faction);
        out.flag(planet.planet_retains_residual_influence);
        out.fixed(planet.planet_credit_value);
        out.fixed(planet.planet_destroyed_credit_value);
        out.whole(planet.max_space_base);
        out.whole(planet.special_structures_land);
        out.whole(planet.special_structures_space);
        out.whole(planet.additional_population_capacity);
        out.whole(planet.population_value);
        out.whole(planet.planet_capture_bonus_reward);
        out.flag(planet.planet_surface_accessible);
        out.flag(planet.planet_starbase_stealth);
        out.flag(planet.planet_groundbase_stealth);
        out.text(planet.galactic_model_name);
        out.fixed(planet.scale_factor);
        out.text(planet.destroyed_galactic_model_name);
        out.text(planet.space_tactical_map);
        out.text(planet.destroyed_space_tactical_map);
        out.text(planet.land_tactical_map);
        out.whole(planet.zoomed_terrain_index);
        out.texts(planet.behaviors);
        out.texts(planet.galactic_behaviors);
        out.flag(planet.in_background);
    };
    out.each(data.planets, encode_planet);
    out.each(data.galaxy_art_objects, encode_planet);
    out.each(data.trade_routes, [&](const TradeRoute& route) {
        out.text(route.name);
        out.text(route.point_a);
        out.text(route.point_b);
        out.fixed(route.hs_speed_factor);
        out.fixed(route.political_control_gain);
        out.fixed(route.credit_gain_factor);
        out.text(route.visible_line_name);
    });
    out.each(data.factions, [&](const Faction& faction) {
        out.text(faction.name);
        out.text(faction.force_alignment);
        out.flag(faction.is_playable);
        out.flag(faction.is_neutral);
        out.flag(faction.can_control_planets);
        out.text(faction.primary_enemy);
        out.text(faction.home_planet);
        out.text(faction.faction_leader);
        out.text(faction.faction_leader_company);
        out.texts(faction.multiplayer_campaign_heroes);
        out.whole(faction.displayed_tech_level_adjustment);
        out.fixed(faction.hyperspace_speed_factor);
        out.fixed(faction.maintenance_cost);
        out.flag(faction.easily_bribed);
        out.flag(faction.benefits_from_corruption);
        out.whole(faction.minimum_visible_base_level);
        out.whole(faction.base_population_capacity);
        out.flag(faction.ai_uses_galactic_pop_cap);
        out.texts(faction.allies); out.texts(faction.enemies);
        out.text(faction.basic_ai);
    });
    out.each(data.constants, [&](const Constant& constant) {
        out.text(constant.tag);
        out.text(constant.text);
        out.fixed(constant.number);
    });
    const auto bytes = out.take();
    return core::sha256(std::span<const std::uint8_t>(bytes));
}

} // namespace eawr::data::galactic
