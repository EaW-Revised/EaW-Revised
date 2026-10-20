#include "eawr/units/galactic_types.hpp"
#include "eawr/core/text.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <limits>
#include <functional>
#include <span>
#include <utility>

namespace eawr::units {
namespace {

using Fixed = sim::math::Fixed;
using IssueKind = data::galactic::IssueKind;

bool equal(const std::string_view left, const std::string_view right) noexcept {
    return core::iequals(left, right);
}

bool space(const char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim(std::string_view text) {
    return core::trim(text);
}

std::vector<std::string> names(const std::string_view text, const bool category_mask = false) {
    std::vector<std::string> result;
    std::string word;
    for (const char c : text) {
        if (space(c) || c == ',' || (category_mask && c == '|')) {
            if (!word.empty()) result.push_back(std::move(word));
            word.clear();
        } else word += c;
    }
    if (!word.empty()) result.push_back(std::move(word));
    return result;
}

std::vector<std::string> composition(data::retail::Layers layers, const std::string_view tag) {
    std::vector<std::string> result;
    for (auto remaining = layers.size(); remaining > 0; --remaining) {
        bool first = true;
        for (const auto& child : layers[remaining - 1]->root.children) {
            if (!equal(child.name, tag) || data::retail::is_placeholder(child.raw_text)) continue;
            const auto tokens = names(child.raw_text);
            if (tokens.empty()) continue;
            data::tag_trace::used(child);
            if (remaining != layers.size() && first) result.clear();
            first = false;
            result.insert(result.end(), tokens.begin(), tokens.end());
        }
    }
    return result;
}

std::vector<const data::XmlNode*> ability_blocks(data::retail::Layers layers) {
    std::vector<const data::XmlNode*> result;
    for (auto remaining = layers.size(); remaining > 0; --remaining) {
        for (const auto& child : layers[remaining - 1]->root.children) {
            if (!equal(child.name, "Abilities")) continue;
            // WAR-84: an authored variant block clears first, even if it has no abilities.
            if (remaining != layers.size()) result.clear();
            result.push_back(&child);
        }
    }
    for (const auto* node : result) data::tag_trace::used(node);
    return result;
}

template <typename T> struct Field { std::string_view tag; T GalacticType::*member; };

constexpr Field<std::optional<Fixed>> numbers[] = {
    {"Hyperspace_Speed", &GalacticType::hyperspace_speed}, {"Population_Value", &GalacticType::population_value},
    {"Political_Control", &GalacticType::political_control}, {"Cheap_Bribe_Cost", &GalacticType::cheap_bribe_cost},
    {"Expensive_Bribe_Cost", &GalacticType::expensive_bribe_cost}, {"Build_Cost_Credits", &GalacticType::build_cost_credits},
    {"Build_Time_Seconds", &GalacticType::build_time_seconds}, {"Maintenance_Cost", &GalacticType::maintenance_cost},
    {"AI_Combat_Power", &GalacticType::ai_combat_power}};
constexpr Field<std::optional<std::int64_t>> counts[] = {
    {"Build_Advances_Tech_Level", &GalacticType::build_advances_tech_level},
    {"Squadron_Capacity", &GalacticType::squadron_capacity}, {"Tech_Level", &GalacticType::tech_level},
    {"Required_Ground_Base_Level", &GalacticType::required_ground_base_level},
    {"Required_Star_Base_Level", &GalacticType::required_star_base_level}, {"Base_Level", &GalacticType::base_level},
    {"Additional_Population_Capacity", &GalacticType::additional_population_capacity},
    {"Build_Limit_Current_Per_Player", &GalacticType::build_limit_current_per_player},
    {"Build_Limit_Lifetime_Per_Player", &GalacticType::build_limit_lifetime_per_player}};
constexpr Field<std::optional<bool>> flags[] = {
    {"Is_Stealth_Company", &GalacticType::stealth_company},
    {"Can_Hyperspace_Without_Activating_Ability", &GalacticType::can_hyperspace_without_ability},
    {"Hyperspace", &GalacticType::hyperspace_capable},
    {"Always_Spawn_In_Orbit", &GalacticType::always_spawn_in_orbit},
    {"Stay_In_Transport_During_Ground_Battle", &GalacticType::stays_in_transport},
    {"Build_Initially_Locked", &GalacticType::build_initially_locked},
    {"Build_Requires_Initial_Placement", &GalacticType::build_requires_initial_placement},
    {"Build_Time_Reduced_By_Multiple_Factories", &GalacticType::build_time_reduced_by_multiple_factories},
    {"Is_Named_Hero", &GalacticType::named_hero}, {"Is_Super_Weapon_Killer", &GalacticType::superweapon_killer},
    {"Can_Participate_In_Space_Battle", &GalacticType::can_participate_in_space}};
constexpr Field<std::string> texts[] = {
    {"Is_Sellable", &GalacticType::sellable},
    {"Company_Transport_Unit", &GalacticType::company_transport},
    {"Create_Additional_Object", &GalacticType::additional_creation_type},
    {"Next_Level_Base", &GalacticType::next_level_base}, {"Political_Faction", &GalacticType::political_faction},
    {"Required_Special_Structures", &GalacticType::required_special_structures},
    {"Unique_Space_Unit", &GalacticType::unique_space_unit}, {"Unique_Ground_Unit", &GalacticType::unique_ground_unit},
    {"Land_Bomber_Type", &GalacticType::land_bomber_type}};
constexpr Field<std::vector<std::string>> lists[] = {
    {"Required_Orbiting_Units", &GalacticType::required_orbiting_units}, {"Required_Planets", &GalacticType::required_planets},
    {"Affiliation", &GalacticType::affiliation},
    {"CategoryMask", &GalacticType::category_mask}, {"SpaceBehavior", &GalacticType::space_behavior},
    {"LandBehavior", &GalacticType::land_behavior}, {"Behavior", &GalacticType::behavior}};

void issue(GalacticTypeTable& table, const GalacticType& type, const data::XmlNode& node,
    const IssueKind kind, const std::string_view reason) {
    table.issues.push_back({kind, node.source.logical_path, node.source.line, type.name, node.name,
        trim(node.raw_text), std::string(reason)});
}

std::optional<Fixed> number(GalacticTypeTable& table, const GalacticType& type, const data::XmlNode& node) {
    const auto value = data::retail::leading_number(node.raw_text);
    if (!value) {
        issue(table, type, node, IssueKind::bad_value, "not a number in Q24 range");
        return std::nullopt;
    }
    if (value->trailing_text) issue(table, type, node, IssueKind::trailing_text, "read as its leading number (AU-82)");
    return value->value;
}

class Encoder {
public:
    void byte(const std::uint8_t value) { bytes.push_back(value); }
    void integer(const std::int64_t value) {
        const auto bits = static_cast<std::uint64_t>(value);
        for (unsigned shift = 0; shift < 64; shift += 8) byte(static_cast<std::uint8_t>(bits >> shift));
    }
    void text(const std::string_view value) { integer(static_cast<std::int64_t>(value.size())); for (const char c : value) byte(static_cast<std::uint8_t>(c)); }
    void encode(const std::optional<Fixed>& value) { byte(value ? 1 : 0); if (value) integer(value->raw()); }
    void encode(const std::optional<std::int64_t>& value) { byte(value ? 1 : 0); if (value) integer(*value); }
    void encode(const std::optional<bool>& value) { byte(!value ? 0 : (*value ? 2 : 1)); }
    void encode(const std::string& value) { text(value); }
    void encode(const std::vector<std::string>& value) { integer(static_cast<std::int64_t>(value.size())); for (const auto& item : value) text(item); }
    template <typename T, std::size_t N> void fields(const GalacticType& type, const Field<T> (&descriptors)[N]) {
        for (const auto& field : descriptors) encode(type.*field.member);
    }
    std::vector<std::uint8_t> bytes;
};

} // namespace

bool built_on_ground(const GalacticType& type) noexcept {
    return type.build_cost_credits.value_or(Fixed{}).raw() >= Fixed::scale
        && (type.required_ground_base_level.value_or(0) != 0
            || std::any_of(type.behavior.begin(), type.behavior.end(), [](const auto& role) {
                return equal(role, "DUMMY_GROUND_STRUCTURE");
            }));
}

const GalacticType* GalacticTypeTable::find(const std::string_view name) const noexcept {
    for (const auto& type : types) if (equal(type.name, name)) return &type;
    return nullptr;
}
core::Result<GalacticType> resolve_initial_galactic_type(const GalacticTypeTable& table,
    const std::string_view name) {
    const auto* original = table.find(name);
    const auto fail = [&](const std::string& message) {
        core::Diagnostic error; error.code = "EAWR-SIM-0501"; error.message = std::string(name) + ": " + message;
        if (original) { error.logical_path = original->file; error.line = original->line; }
        return core::Result<GalacticType>::failure(std::move(error));
    };
    if (!original) return fail("missing original galactic type");
    for (const auto& issue : table.issues) if (equal(issue.owner, original->name) && issue.kind == IssueKind::bad_value
        && ((!original->hyperspace_speed && issue.tag == "Hyperspace_Speed")
            || (!original->political_control && issue.tag == "Political_Control")
            || (!original->squadron_capacity && issue.tag == "Squadron_Capacity")
            || (!original->build_cost_credits && issue.tag == "Build_Cost_Credits")
            || (!original->maintenance_cost && issue.tag == "Maintenance_Cost")
            || (!original->build_time_seconds && issue.tag == "Build_Time_Seconds")
            || (!original->maintenance_cost && issue.tag == "Maintenance_Cost")
            || (!original->build_advances_tech_level && issue.tag == "Build_Advances_Tech_Level")
            || (!original->ai_combat_power && issue.tag == "AI_Combat_Power")))
        return fail("invalid authored " + issue.tag + " cannot use an omission default");
    auto type = *original;
    if ((type.hyperspace_speed && type.hyperspace_speed->raw() < 0)
        || (type.squadron_capacity && (*type.squadron_capacity < 0
            || static_cast<std::uint64_t>(*type.squadron_capacity) > std::numeric_limits<std::uint32_t>::max())))
        return fail("negative speed or unbounded squadron capacity");
    if (!type.hyperspace_speed) type.hyperspace_speed = Fixed{};
    if (!type.political_control) type.political_control = Fixed{};
    if (!type.squadron_capacity) type.squadron_capacity = 0;
    if (!type.hyperspace_capable) type.hyperspace_capable = false;
    if (!type.always_spawn_in_orbit) type.always_spawn_in_orbit = false;
    // WGM-100/WGEP-92 omission defaults belong to movement and production
    // consumers. Retain authored absence in content-bearing original facts.
    if (type.build_advances_tech_level && (*type.build_advances_tech_level < 0
            || *type.build_advances_tech_level > std::numeric_limits<std::int32_t>::max()))
        return fail("invalid unsigned technology target");
    return core::Result<GalacticType>::success(std::move(type));
}

GalacticTypeTable galactic_type_table(const data::Catalog& catalog, const bool include_planets) {
    GalacticTypeTable table;
    for (const auto& definition : catalog.definitions()) {
        if (definition.category != data::Category::game_object || !definition.active || !definition.namespace_winner
            || (!include_planets && equal(definition.type_name, "Planet"))) continue;
        GalacticType type;
        type.name = definition.id;
        type.object_class = definition.type_name;
        type.file = definition.root.source.logical_path;
        type.line = definition.root.source.line;
        const auto members = catalog.member_flags(definition.id);
        if (members) {
            type.ground_company_member = members.value().ground_company_member;
            type.squadron_member = members.value().squadron_member;
        } else {
            table.issues.push_back({IssueKind::bad_value, type.file, type.line,
                type.name, "member-parse-timeline", {}, members.error().message});
        }
        std::vector<const data::Definition*> layers;
        const auto resolved = catalog.resolve(definition.id, data::Category::game_object);
        if (resolved) {
            for (const auto& name : resolved.value().chain) {
                if (const auto* layer = catalog.find(name, data::Category::game_object)) layers.push_back(layer);
            }
        } else {
            table.issues.push_back({IssueKind::dangling_reference, definition.root.source.logical_path,
                definition.root.source.line, type.name, "Variant_Of_Existing_Type", {}, resolved.error().message});
        }
        if (layers.empty()) layers.push_back(&definition);
        bool relevant = type.ground_company_member == true || type.squadron_member == true;
        const auto node = [&](const std::string_view tag) {
            const auto* found = data::retail::single(layers, tag);
            relevant = relevant || found != nullptr;
            return found;
        };
        for (const auto& field : numbers) if (const auto* at = node(field.tag)) type.*field.member = number(table, type, *at);
        if (const auto* at = node("Space_Override_Population_Value"))
            type.space_override_population_value = number(table, type, *at);
        for (const auto& field : counts) {
            const auto* at = node(field.tag);
            if (!at) continue;
            const auto parsed = number(table, type, *at);
            if (!parsed) continue;
            if (parsed->raw() % Fixed::scale != 0) issue(table, type, *at, IssueKind::bad_value, "not a whole number");
            else type.*field.member = parsed->raw() / Fixed::scale;
        }
        for (const auto& field : flags) if (const auto* at = node(field.tag)) type.*field.member = data::retail::boolean(at->raw_text);
        for (const auto& field : texts) if (const auto* at = node(field.tag)) type.*field.member = trim(at->raw_text);
        for (const auto& field : lists) if (const auto* at = node(field.tag))
            type.*field.member = names(at->raw_text, field.tag == "CategoryMask");
        // WAR-84: the first nonempty variant occurrence clears inherited members.
        for (const auto& [tag, destination] : {std::pair{"Squadron_Units", &type.squadron_units},
                 std::pair{"Company_Units", &type.company_units}}) {
            *destination = composition(layers, tag);
            relevant = relevant || !destination->empty();
        }
        for (std::size_t tech = 0; tech < type.starting_spawned_units.size(); ++tech) {
            const auto tag = "Starting_Spawned_Units_Tech_" + std::to_string(tech);
            for (const auto* at : data::retail::list(layers, tag, data::retail::ListRule::append)) {
                relevant = true;
                const auto row = names(at->raw_text);
                const auto count = row.size() >= 2 ? data::retail::leading_number(row[1]) : std::nullopt;
                if (!count || count->value.raw() % Fixed::scale != 0
                    || count->value.trunc_to_integer() < std::numeric_limits<std::int32_t>::min()
                    || count->value.trunc_to_integer() > std::numeric_limits<std::int32_t>::max()) {
                    issue(table, type, *at, IssueKind::bad_value, "expected spawned type and whole count");
                    continue;
                }
                if (row.size() > 2 || count->trailing_text)
                    issue(table, type, *at, IssueKind::trailing_text, "read the first type/count pair (AU-82)");
                type.starting_spawned_units[tech].push_back({row[0], static_cast<std::int32_t>(count->value.trunc_to_integer())});
            }
        }
        for (const auto* abilities : ability_blocks(layers)) {
            for (const auto& ability : abilities->children) {
                if (equal(ability.name, "Galactic_Stealth_Ability")) {
                    relevant = true; GalacticInitialStealth initial;
                    for (const auto& child : ability.children) {
                        if (data::retail::is_placeholder(child.raw_text)) continue;
                        if (equal(child.name, "Initially_Enabled")) {
                            data::tag_trace::used(child); initial.initially_enabled = data::retail::boolean(child.raw_text);
                            if (!initial.initially_enabled) table.issues.push_back({IssueKind::bad_value, child.source.logical_path,
                                child.source.line, type.name, "Abilities/Galactic_Stealth_Ability/Initially_Enabled",
                                trim(child.raw_text), "invalid initial stealth enabled flag"});
                        } else if (equal(child.name, "Activation_Style")) {
                            data::tag_trace::used(child); initial.activation_style = trim(child.raw_text);
                        } else if (equal(child.name, "Evade_Detection_Chance")) {
                            data::tag_trace::used(child); initial.evade_chance = number(table, type, child);
                        }
                    }
                    type.initial_stealth.push_back(std::move(initial));
                }
                if (!equal(ability.name, "Combat_Bonus_Ability") && !equal(ability.name, "Enhance_Defense_Ability")) continue;
                relevant = true;
                GalacticStrengthBonus bonus;
                bonus.defense = equal(ability.name, "Enhance_Defense_Ability");
                for (const auto& child : ability.children) {
                    if (data::retail::is_placeholder(child.raw_text)) continue;
                    if (equal(child.name, "Unit_Strength_Category") || equal(child.name, "Health_Bonus_Percentage")
                        || equal(child.name, "Damage_Bonus_Percentage") || equal(child.name, "Enhancement_Percentage"))
                        data::tag_trace::used(child);
                    if (equal(child.name, "Unit_Strength_Category")) bonus.categories = names(child.raw_text, true);
                    else if (equal(child.name, "Health_Bonus_Percentage")) bonus.health = number(table, type, child);
                    else if (equal(child.name, "Damage_Bonus_Percentage")) bonus.damage = number(table, type, child);
                    else if (equal(child.name, "Enhancement_Percentage")) bonus.enhancement = number(table, type, child);
                }
                type.strength_bonuses.push_back(std::move(bonus));
            }
        }
        if (!relevant) continue;
        for (const auto& child : definition.root.children) {
            if (child.schema_status == data::SchemaStatus::unknown) issue(table, type, child, IssueKind::unknown_tag, "the tag is not known for this class");
        }
        table.types.push_back(std::move(type));
    }
    // Validate references without constructing any tactical types or hard-coding faction names.
    for (auto& type : table.types) {
        const auto reference = [&](const std::string_view tag, const std::string& name, const data::Category category) {
            if (name.empty() || catalog.find(name, category)) return;
            table.issues.push_back({IssueKind::dangling_reference, type.file, type.line, type.name, std::string(tag), name, "unresolved type or faction"});
        };
        reference("Next_Level_Base", type.next_level_base, data::Category::game_object);
        reference("Company_Transport_Unit", type.company_transport, data::Category::game_object);
        reference("Create_Additional_Object", type.additional_creation_type, data::Category::game_object);
        reference("Political_Faction", type.political_faction, data::Category::faction);
        for (auto* name : {&type.unique_space_unit, &type.unique_ground_unit, &type.land_bomber_type})
            if (equal(*name, "None")) name->clear(); // WAR-83: an explicit null reference
        reference("Unique_Space_Unit", type.unique_space_unit, data::Category::game_object);
        reference("Unique_Ground_Unit", type.unique_ground_unit, data::Category::game_object);
        reference("Land_Bomber_Type", type.land_bomber_type, data::Category::game_object);
        for (auto* name : {&type.unique_space_unit, &type.unique_ground_unit, &type.land_bomber_type})
            if (!name->empty() && !catalog.find(*name, data::Category::game_object)) name->clear(); // WAR-83: unresolved stays null
        for (std::size_t tech = 0; tech < type.starting_spawned_units.size(); ++tech)
            for (const auto& entry : type.starting_spawned_units[tech])
                reference("Starting_Spawned_Units_Tech_" + std::to_string(tech), entry.type, data::Category::game_object);
        for (const auto& name : type.affiliation) reference("Affiliation", name, data::Category::faction);
        for (const auto& [tag, values] : {std::pair{"Required_Orbiting_Units", &type.required_orbiting_units},
                 std::pair{"Required_Planets", &type.required_planets}, std::pair{"Squadron_Units", &type.squadron_units},
                 std::pair{"Company_Units", &type.company_units}}) {
            for (const auto& name : *values) reference(tag, name, data::Category::game_object);
        }
    }
    // WAR-82: company and then squadron post-load strength replace the containing
    // type's authored power/mask. Resolve dependencies first without storage-order effects.
    std::vector<std::uint8_t> ready(table.types.size());
    const auto binding_issue = [&](const GalacticType& type, const std::string_view tag, const std::string_view reason) {
        table.issues.push_back({IssueKind::bad_value, type.file, type.line, type.name, std::string(tag), {}, std::string(reason)});
    };
    std::function<bool(std::size_t)> bind = [&](const std::size_t index) {
        auto& type = table.types[index];
        if (ready[index] == 2) return true;
        if (ready[index] == 1) { binding_issue(type, "Company_Units", "cyclic container strength dependency"); return false; }
        ready[index] = 1;
        bool valid = true;
        for (const auto* members : {&type.company_units, &type.squadron_units}) {
            if (members->empty()) continue;
            Fixed power;
            std::vector<std::string> categories;
            for (const auto& name : *members) {
                const auto* member = table.find(name);
                if (!member) { valid = false; continue; }
                const auto member_index = static_cast<std::size_t>(member - table.types.data());
                if (!bind(member_index)) { valid = false; continue; }
                const auto sum = sim::math::add(power, member->ai_combat_power.value_or(Fixed{}));
                if (!sum) { binding_issue(type, "AI_Combat_Power", "container power overflows Q24"); valid = false; continue; }
                power = sum.value();
                for (const auto& category : member->category_mask)
                    if (std::none_of(categories.begin(), categories.end(), [&](const auto& value) { return equal(value, category); }))
                        categories.push_back(category);
            }
            type.ai_combat_power = valid ? std::optional<Fixed>{power} : std::nullopt;
            type.category_mask = std::move(categories);
            // WR-85: squadron post-load capability is the last authored craft's
            // resolved value, replacing even an explicit squadron Hyperspace tag.
            if (valid && members == &type.squadron_units)
                type.hyperspace_capable = table.find(members->back())->hyperspace_capable.value_or(false);
        }
        ready[index] = 2;
        return valid;
    };
    for (std::size_t index = 0; index < table.types.size(); ++index) static_cast<void>(bind(index));
    return table;
}

std::array<std::uint8_t, 32> galactic_content_identity(const GalacticTypeTable& table) {
    Encoder out;
    out.text("eawr-galactic-types-v6");
    out.integer(static_cast<std::int64_t>(table.types.size()));
    for (const auto& type : table.types) {
        out.text(type.name);
        out.text(type.object_class);
        out.fields(type, numbers);
        out.fields(type, counts);
        out.fields(type, flags);
        out.fields(type, texts);
        out.fields(type, lists);
        out.encode(type.squadron_units);
        out.encode(type.company_units);
        out.encode(type.ground_company_member);
        out.encode(type.squadron_member);
        for (const auto& row : type.starting_spawned_units) {
            out.integer(static_cast<std::int64_t>(row.size()));
            for (const auto& entry : row) { out.text(entry.type); out.integer(entry.count); }
        }
        out.integer(static_cast<std::int64_t>(type.strength_bonuses.size()));
        for (const auto& bonus : type.strength_bonuses) {
            out.encode(bonus.categories); out.encode(bonus.health); out.encode(bonus.damage);
            out.encode(bonus.enhancement); out.byte(bonus.defense ? 1 : 0);
        }
        // Authored stealth contributes a nonempty extension in this domain.
        if (!type.initial_stealth.empty()) {
            out.integer(static_cast<std::int64_t>(type.initial_stealth.size()));
            for (const auto& ability : type.initial_stealth) {
                out.encode(ability.initially_enabled); out.text(ability.activation_style);
                out.encode(ability.evade_chance);
            }
        }
        if (type.space_override_population_value) {
            out.text("space-population-override-v1");
            out.encode(type.space_override_population_value);
        }
    }
    return core::sha256(std::span<const std::uint8_t>(out.bytes));
}

GalacticEconomicTable galactic_economic_table(const data::Catalog& catalog) {
    GalacticEconomicTable result;
    for (const auto& definition : catalog.definitions()) {
        if (definition.category != data::Category::game_object || !definition.active || !definition.namespace_winner) continue;
        std::vector<const data::Definition*> layers;
        const auto resolved = catalog.resolve(definition.id, data::Category::game_object);
        if (resolved) for (const auto& name : resolved.value().chain)
            if (const auto* at = catalog.find(name, data::Category::game_object)) layers.push_back(at);
        if (layers.empty()) layers.push_back(&definition);
        GalacticEconomicType type; type.name = definition.id; type.object_class = definition.type_name;
        if (equal(definition.type_name, "Planet"))
            type.system_spies = data::galactic::system_spy_definitions(catalog, definition, result.issues);
        type.mining_colony = false; // WGS-90.
        GalacticType context; context.name = type.name;
        GalacticTypeTable diagnostics;
        // WGEP-166/168: effective ability blocks and authored source names. A repeated
        // name replaces its source's percentage rather than adding another modifier.
        for (const auto* block : ability_blocks(layers)) for (const auto& ability : block->children) {
            if (!equal(ability.name, "Reduce_Technology_Price_Ability")) continue;
            GalacticTechnologyPriceAbility technology;
            data::tag_trace::used_attribute(ability, "Name");
            for (const auto& attribute : ability.attributes)
                if (equal(attribute.name, "Name")) technology.name = attribute.value;
            for (const auto& child : ability.children) {
                if (data::retail::is_placeholder(child.raw_text)) continue;
                if (equal(child.name, "Initially_Enabled")) {
                    data::tag_trace::used(child);
                    technology.initially_enabled = data::retail::boolean(child.raw_text);
                    if (!technology.initially_enabled) issue(diagnostics, context, child, IssueKind::bad_value, "expected technology reduction enabled flag");
                } else if (equal(child.name, "Activation_Style")) {
                    data::tag_trace::used(child); technology.activation_style = trim(child.raw_text);
                } else if (equal(child.name, "Price_Reduction_Percentage") && !data::retail::is_placeholder(child.raw_text)) {
                    data::tag_trace::used(child); technology.reduction = number(diagnostics, context, child);
                }
            }
            const auto previous = std::find_if(type.technology_prices.begin(), type.technology_prices.end(),
                [&](const auto& row) { return row.name == technology.name; });
            if (previous == type.technology_prices.end()) type.technology_prices.push_back(std::move(technology));
            else *previous = std::move(technology);
        }
        if (const auto* at = data::retail::single(layers, "Planet_Is_Mining_Colony")) {
            type.mining_colony = data::retail::boolean(at->raw_text);
            if (!type.mining_colony) issue(diagnostics, context, *at, IssueKind::bad_value, "expected authored mining colony flag");
        }
        for (const auto* block : ability_blocks(layers)) for (const auto& ability : block->children) {
            const bool mine = equal(ability.name, "Planet_Income_Bonus_Ability");
            const bool gamble = equal(ability.name, "Planet_Income_Gambling_Ability");
            const bool price = equal(ability.name, "Reduce_Production_Price_Ability");
            const bool time = equal(ability.name, "Reduce_Production_Time_Ability");
            const bool sabotage = equal(ability.name, "Galactic_Sabotage_Ability");
            if (!mine && !gamble && !price && !time && !sabotage) continue;
            type.mining = type.mining || mine; type.gambling = type.gambling || gamble;
            // WGEP-94: authored values overlay the specific ability constructors.
            if (mine) { type.mining_percentage = Fixed{}; type.mining_absolute = Fixed{}; type.mining_multiplier = Fixed::from_raw(Fixed::scale); }
            if (gamble) { type.gambling_chance = Fixed{}; type.gambling_minimum = Fixed{}; type.gambling_maximum = Fixed{}; type.gambling_interval = Fixed{}; }
            GalacticReductionAbility reduction; reduction.price = price;
            // WGEP-93: the two specific constructors initialize these values.
            // Parsing a malformed present value still clears it and refuses.
            reduction.category = 0; reduction.reduction = Fixed{}; reduction.galaxy_wide = false;
            for (const auto& child : ability.children) {
                if ((mine || gamble) && equal(child.name, "Initially_Enabled")) {
                    data::tag_trace::used(child);
                    auto& activation = mine ? type.mining_activation : type.gambling_activation;
                    activation.initially_enabled = data::retail::boolean(child.raw_text);
                    if (!activation.initially_enabled) issue(diagnostics, context, child, IssueKind::bad_value, "expected economic enabled flag");
                    continue;
                }
                if ((mine || gamble) && equal(child.name, "Activation_Style")) {
                    data::tag_trace::used(child);
                    auto& activation = mine ? type.mining_activation : type.gambling_activation;
                    activation.activation_style = trim(child.raw_text); continue;
                }
                const auto list = [&](std::string_view tag, auto& target, bool categories = false) {
                    if (!equal(child.name, tag)) return false;
                    data::tag_trace::used(child); target = names(child.raw_text, categories); return true;
                };
                const auto value = [&](std::string_view tag, auto& target) {
                    if (!equal(child.name, tag) || data::retail::is_placeholder(child.raw_text)) return false;
                    data::tag_trace::used(child); target = number(diagnostics, context, child); return true;
                };
                if (mine) {
                    if (value("Percentage_Income_Modifier", type.mining_percentage)
                        || value("Absolute_Income_Modifier", type.mining_absolute)
                        || value("Additional_Multiplier_On_Mining_Colonies", type.mining_multiplier)) continue;
                } else if (gamble) {
                    if (value("Chance_To_Win", type.gambling_chance) || value("Min_Income_Modifier", type.gambling_minimum)
                        || value("Max_Income_Modifier", type.gambling_maximum) || value("Evaluation_Interval_In_Secs", type.gambling_interval)) continue;
                } else if (sabotage) {
                    // WGEP-163/165: the only sabotage tags this consumer applies; the other sabotage tags keep their rows.
                    if (equal(child.name, "Can_Halt_Credit_Production")) {
                        data::tag_trace::used(child); type.credit_halt = data::retail::boolean(child.raw_text);
                        if (!type.credit_halt) issue(diagnostics, context, child, IssueKind::bad_value, "expected credit-halt flag");
                    } else (void)value("Duration_Of_Credit_Halt", type.credit_halt_seconds);
                } else {
                    if (equal(child.name, "Initially_Enabled")) {
                        data::tag_trace::used(child); reduction.initially_enabled = data::retail::boolean(child.raw_text);
                        if (!reduction.initially_enabled) issue(diagnostics, context, child, IssueKind::bad_value, "expected reduction enabled flag");
                        continue;
                    }
                    if (equal(child.name, "Activation_Style")) {
                        data::tag_trace::used(child); reduction.activation_style = trim(child.raw_text);
                        continue;
                    }
                    if (list("Applicable_Unit_Categories", reduction.included_categories, true)
                        || list("Excluded_Unit_Categories", reduction.excluded_categories, true)
                        || list("Applicable_Unit_Types", reduction.included_types)
                        || list("Excluded_Unit_Types", reduction.excluded_types)) continue;
                    if (equal(child.name, "Stacking_Category")) {
                        data::tag_trace::used(child); const auto n = number(diagnostics, context, child);
                        if (n && n->raw() % Fixed::scale == 0) reduction.category = n->trunc_to_integer();
                        else issue(diagnostics, context, child, IssueKind::bad_value, "expected whole stacking category");
                    } else if (equal(child.name, "Galaxy_Wide")) {
                        data::tag_trace::used(child); reduction.galaxy_wide = data::retail::boolean(child.raw_text);
                        if (!reduction.galaxy_wide) issue(diagnostics, context, child, IssueKind::bad_value, "expected reduction galaxy-wide flag");
                    } else (void)value(price ? "Price_Reduction_Percentage" : "Time_Reduction_Percentage", reduction.reduction);
                }
            }
            if (price || time) type.reductions.push_back(std::move(reduction));
        }
        result.issues.insert(result.issues.end(), diagnostics.issues.begin(), diagnostics.issues.end());
        if (type.mining_colony || type.mining || type.gambling || !type.reductions.empty() || type.credit_halt.value_or(false)) result.types.push_back(std::move(type));
    }
    return result;
}

} // namespace eawr::units
