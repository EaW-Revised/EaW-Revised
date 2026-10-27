#include "eawr/campaign/ai_selection_content.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/units/unit_tables.hpp"
#include "../script/foc/galactic_ai_selection_content.hpp"
#include "../script/foc/galactic_ai_freestore.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace eawr::campaign {
namespace {
namespace ai = script::foc::ai;
namespace gc = sim::galactic;
template<class T> T required(core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(value).value();
}
bool behaves(const units::GalacticType& type, std::string_view name) {
    return std::any_of(type.behavior.begin(), type.behavior.end(), [&](const auto& value) { return ai::upper_case(value) == name; });
}
std::string text(const data::EffectiveObject& object, std::string_view name) {
    const auto* value = object.value(name); if (!value) return {};
    data::tag_trace::used(value->value); return ai::upper_case(ai::trimmed(value->value.raw_text));
}
bool flag(const data::EffectiveObject& object, std::string_view name, bool omitted = false) {
    const auto value = text(object, name); if (value.empty()) return omitted;
    const auto parsed = data::retail::boolean(value);
    if (!parsed) throw std::runtime_error("invalid selection flag " + std::string(name));
    return *parsed;
}
}
FreestoreMemberQualifier freestore_member_qualifier(std::shared_ptr<const ai::GalacticSelectionContent> content) {
    return [content = std::move(content)](gc::TypeId type) -> core::Result<bool> {
        if (content) {
            const auto found = content->types.find(type);
            if (found != content->types.end() && found->second.original) {
                const auto& loaded = *found->second.original;
                if (loaded.ground_company_member && loaded.squadron_member)
                    return core::Result<bool>::success(*loaded.ground_company_member || *loaded.squadron_member);
            }
        }
        core::Diagnostic error; error.code = "galactic-ai.freestore-member";
        error.message = "current type requires completed loaded member flags (WGA-363/364)";
        if (content) {
            const auto found = content->types.find(type);
            error.message += ": type " + std::to_string(type) + (found == content->types.end() ? " is not in the selection content"
                : " " + found->second.name + (found->second.original ? " has no member flags" : " has no loaded original"));
        }
        return core::Result<bool>::failure(std::move(error));
    };
}
core::Result<std::vector<script::foc::galactic_ai::FreestoreFacts>> prepare_fresh_freestore_facts(
    const gc::GalacticSnapshot& snapshot, const ai::GalacticSelectionContent& content,
    const units::GalacticTypeTable& types, const sim::PartitionExecutor& executor, FreestoreMemberQualifier members,
    FreestoreCurrentTypeQualifier current_type) {
    namespace ga = script::foc::galactic_ai;
    using Result = core::Result<std::vector<ga::FreestoreFacts>>;
    try {
        if (!members) throw std::runtime_error("fresh freestore current-type member qualification is unavailable (#2725: completed parse-pass flags required)");
        if (!current_type) throw std::runtime_error("fresh freestore current representation resolver is unavailable (#2735)");
        struct Traits { ga::FreestoreType value; bool hero{}; };
        std::map<gc::TypeId, Traits> inventory; std::vector<ga::FreestoreType> traits;
        for (const auto& [id, authored] : content.types) {
            const auto* current = authored.original ? authored.original.get() : types.find(authored.name);
            if (!current) throw std::runtime_error("fresh freestore current type is absent");
            const auto has = [&](std::string_view name) {
                for (const auto* list : {&current->behavior, &current->space_behavior, &current->land_behavior})
                    if (std::find(list->begin(), list->end(), name) != list->end()) return true;
                return false;
            };
            Traits row; row.value.type = id; row.value.transport = has("TRANSPORT"); row.hero = authored.value.hero;
            if (has("FLEET")) row.value.kind = ga::FreestoreKind::fleet;
            else if (has("DUMMY_GROUND_STRUCTURE") || has("DUMMY_GROUND_BASE")) row.value.kind = ga::FreestoreKind::ground_structure;
            else if (has("DUMMY_ORBITAL_STRUCTURE") || has("DUMMY_STAR_BASE")) row.value.kind = ga::FreestoreKind::space_structure;
            inventory.emplace(id, row); traits.push_back(row.value);
        }
        const auto presence = required(ga::copy_freestore(snapshot, traits, executor));
        std::map<sim::EntityId, sim::EntityId> parents;
        std::map<sim::EntityId, bool> enroute;
        for (const auto& fleet : snapshot.fleets()) {
            enroute.emplace(fleet.id, fleet.motion.transit.has_value());
            for (const auto& member : fleet.members) parents.emplace(member.id, fleet.id);
        }
        if (snapshot.movement()) for (const auto& container : snapshot.movement()->contents)
            for (const auto& child : container.children) parents.emplace(child.id, container.container);
        std::vector<ga::FreestoreFacts> out; out.reserve(presence.objects.size());
        for (const auto& object : presence.objects) {
            if (!object.type) continue; // planets/fleet shells are not WGA-303 initial offers
            if (!inventory.contains(object.type)) throw std::runtime_error("fresh freestore original type is absent");
            const auto current = current_type(object);
            if (!current) throw std::runtime_error("fresh freestore current representation reader refused (#2735): " + current.error().message);
            const auto found = inventory.find(current.value());
            if (found == inventory.end()) throw std::runtime_error("fresh freestore actor has missing current-type traits");
            ga::FreestoreFacts row; row.object = object; row.object.kind = found->second.value.kind;
            row.object.transport = found->second.value.transport;
            // Current-representation transport safety cannot use a strategic
            // original-type census. Only independent WGA-308 early cases are known.
            row.object.safe = !row.object.transport || row.object.transit || row.object.directly_parented_to_planet
                ? std::optional(true) : std::nullopt;
            row.current_type = current.value();
            const auto member = members(row.current_type);
            if (!member) throw std::runtime_error("fresh freestore member reader refused (#2725): " + member.error().message);
            row.current_type_member = member.value(); row.hero = found->second.hero;
            if (object.directly_parented_to_planet) { row.parent = object.planet; row.parent_is_planet = true; }
            else if (const auto parent = parents.find(object.id); parent != parents.end()) {
                row.parent = parent->second; const auto moving = enroute.find(row.parent);
                row.enroute = moving != enroute.end() && moving->second;
            }
            // Constructor boundary only. Runtime notifications use update/offer,
            // owner_changed/lost; they never replay this constructor default.
            out.push_back(ga::ordinary_freestore_birth(std::move(row)));
        }
        return Result::success(std::move(out));
    } catch (const std::exception& exception) {
        core::Diagnostic diagnostic; diagnostic.code = "galactic-ai.freestore-facts"; diagnostic.message = exception.what();
        return Result::failure(std::move(diagnostic));
    }
}
core::Result<std::shared_ptr<const script::foc::ai::GalacticSelectionContent>> prepare_ai_selection_content(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const CampaignSetupPlan& plan) {
    using Result = core::Result<std::shared_ptr<const ai::GalacticSelectionContent>>;
    try {
        auto out = std::make_shared<ai::GalacticSelectionContent>();
        if (plan.type_bindings().empty()) throw std::runtime_error("selection content requires original bindings");
        units::LoadInput enums; enums.catalog = &catalog; enums.filesystem = &filesystem;
        for (const auto& binding : plan.type_bindings()) enums.types.push_back(binding.original_type);
        for (const auto& location : plan.locations()) if (location.planet) enums.types.push_back(location.original_type);
        const auto vocabulary = required(units::load_unit_tables(enums));
        for (const auto& row : vocabulary.categories) out->ai.categories.emplace(ai::upper_case(row.name), static_cast<std::uint64_t>(row.value));
        std::vector<FleetTypeBinding> bindings(plan.type_bindings().begin(), plan.type_bindings().end());
        // CRP-01: match the typed planet constructor's location-order bindings.
        // A name hash is not the type identity retained by the live census.
        auto planet_type = bindings.back().id;
        for (const auto& location : plan.locations()) if (location.planet) {
            if (planet_type == std::numeric_limits<gc::TypeId>::max())
                throw std::runtime_error("planet original type identity exhausted");
            bindings.push_back({++planet_type, location.original_type});
        }
        std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        if (std::any_of(bindings.begin(), bindings.end(), [](const auto& row) { return row.id == 0; })
            || std::adjacent_find(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.id == b.id; }) != bindings.end())
            throw std::runtime_error("original planet metadata identity collision");
        const auto all_types = units::galactic_type_table(catalog, true);
        std::map<std::string, gc::TypeId, std::less<>> identities;
        for (const auto& binding : bindings) identities.emplace(ai::upper_case(binding.original_type), binding.id);
        // CGS-07: a name that resolves to no bound type is a null reference; it is reported and left out.
        const auto identity = [&](std::string_view name, std::string_view where) -> std::optional<gc::TypeId> {
            const auto at = identities.find(ai::upper_case(name));
            if (at == identities.end()) {
                out->unresolved_names.push_back(std::string(where) + ": " + std::string(name));
                return std::nullopt;
            }
            return at->second;
        };
        const auto bits = [&](std::string_view names) {
            std::uint64_t result{};
            for (const auto& name : ai::split_names(names, ", |\t\r\n")) {
                const auto at = out->ai.categories.find(ai::upper_case(name));
                if (at == out->ai.categories.end()) throw std::runtime_error("unqualified selection category " + name);
                result |= at->second;
            }
            return result;
        };
        for (const auto& binding : bindings) {
            const auto source = required(units::resolve_initial_galactic_type(all_types, binding.original_type));
            const auto object = required(catalog.resolve(source.name, data::Category::game_object));
            ai::GalacticSelectionContentType row; row.name = ai::upper_case(source.name);
            row.company = !source.company_units.empty(); row.transport = behaves(source, "TRANSPORT");
            row.ground_base = behaves(source, "DUMMY_GROUND_BASE"); row.starbase = behaves(source, "DUMMY_STAR_BASE");
            row.craft = behaves(source, "SQUADRON_MEMBER"); row.team = flag(object, "Create_Team");
            row.original = std::make_shared<const units::GalacticType>(source);
            row.named_hero = flag(object, "Is_Named_Hero");
            row.space_escorts = ai::split_names(text(object, "Space_Escort_Unit_Types"), ", |\t\r\n");
            row.special_weapon = flag(object, "Is_Special_Weapon_In_Space");
            row.value.special_weapon = row.special_weapon;
            // WGA-183: the space-structure metric has its own mask; omitted
            // authoring keeps Capital rather than the ordinary CategoryMask.
            const auto garrison = text(object, "Garrison_Category");
            row.garrison_categories = bits(garrison.empty() ? "Capital" : garrison);
            row.value.hero = row.named_hero || flag(object, "Is_Generic_Hero") || row.special_weapon;
            row.value.structure = behaves(source, "DUMMY_GROUND_STRUCTURE") || behaves(source, "DUMMY_ORBITAL_STRUCTURE");
            row.value.channel = units::built_on_ground(source) ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
            row.hyperspace_capable = source.hyperspace_capable.value(); row.initially_locked = flag(object, "Build_Initially_Locked");
            row.hyperspace_speed = script::numeric::from_fixed(source.hyperspace_speed.value());
            row.survival_type = row.name; row.movement_class = text(object, "MovementClass"); row.lua_script = text(object, "Lua_Script");
            if (row.company) {
                const auto* member = types.find(source.company_units.front());
                if (!member) throw std::runtime_error("missing first company member");
                const auto original = required(catalog.resolve(member->name, data::Category::game_object));
                row.movement_class = text(original, "MovementClass");
                for (const auto& unit : source.company_units) { // WGA-489: any member unit's authored object script
                    const auto* found = types.find(unit); if (!found) continue;
                    if (!text(required(catalog.resolve(found->name, data::Category::game_object)), "Lua_Script").empty()) row.member_script = true;
                }
                if (!flag(original, "Create_Team")) row.survival_type = ai::upper_case(member->name);
                if (!source.company_transport.empty()) row.hyperspace_speed = script::numeric::from_fixed(required(units::resolve_initial_galactic_type(types, source.company_transport)).hyperspace_speed.value());
            }
            // WGEP-92/69, WGS-91 and WGA-289: only genuine constructor
            // omissions become zero. Malformed effective authoring refuses.
            const auto numeric = [&](const auto& value, std::string_view tag) {
                if (!value && !text(object, tag).empty()) throw std::runtime_error("invalid selection numeric " + std::string(tag));
            };
            numeric(source.population_value, "Population_Value"); numeric(source.tech_level, "Tech_Level");
            numeric(source.base_level, "Base_Level"); numeric(source.maintenance_cost, "Maintenance_Cost");
            if (source.tech_level.value_or(0) < 0 || source.base_level.value_or(0) < 0)
                throw std::runtime_error("negative selection level admission unqualified");
            row.value.cost = script::numeric::from_fixed(source.build_cost_credits.value_or(sim::math::Fixed{}));
            row.value.build_seconds = script::numeric::from_fixed(source.build_time_seconds.value_or(sim::math::Fixed{}));
            row.value.population = script::numeric::from_fixed(source.population_value.value_or(sim::math::Fixed{}));
            row.value.tech = static_cast<std::uint32_t>(std::max<std::int64_t>(source.tech_level.value_or(0), 0));
            for (const auto& name : source.required_orbiting_units)
                if (const auto id = identity(name, source.name + " Required_Orbiting_Units")) row.required_orbiting.push_back(*id);
            row.required_planets = source.required_planets;
            script::foc::AiType type; type.type_id = binding.id; type.name = row.name;
            const auto* tactical_type = vocabulary.find(source.name);
            if (!tactical_type) throw std::runtime_error("selection type lacks tactical behavior flags");
            type.targeting = tactical_type->targeting; type.unit_ai = tactical_type->unit_ai;
            for (const auto& category : source.category_mask) type.category_bits |= bits(category);
            type.combat_power = ai::to_single(script::numeric::from_fixed(source.ai_combat_power.value_or(sim::math::Fixed{})));
            type.tech_level = row.value.tech; type.base_level = static_cast<std::uint32_t>(std::max<std::int64_t>(source.base_level.value_or(0), 0));
            type.star_base = row.starbase; type.craft = row.craft; type.squadron = !source.squadron_units.empty(); type.space_special_weapon = row.special_weapon;
            out->ai.types.push_back(std::move(type)); out->types.emplace(binding.id, std::move(row));
        }
        // WGEP-149: ID lookup order cannot decide the first authored next-tech type.
        for (const auto& type : all_types.types) {
            const auto bound = identities.find(ai::upper_case(type.name));
            if (bound != identities.end()) out->registered_types.push_back(bound->second);
        }
        if (out->registered_types.size() != out->types.size())
            throw std::runtime_error("registered selection type order is incomplete");
        // PL-20: ai.types is the type table in load order (registry file, then place in the file); a
        // category team enumerates it in that order, not in binding-id order. Duplicate names
        // retain catalog winner order pending the duplicate-entry/name-lookup seam (legacy #3741).
        {
            std::map<std::string, std::uint64_t, std::less<>> table_order;
            for (const auto& binding : bindings) {
                const auto* definition = catalog.find(binding.original_type, data::Category::game_object);
                table_order.emplace(ai::upper_case(binding.original_type), definition == nullptr ? std::numeric_limits<std::uint64_t>::max()
                    : (static_cast<std::uint64_t>(definition->registry_order) << 32) | definition->definition_order);
            }
            std::stable_sort(out->ai.types.begin(), out->ai.types.end(),
                [&](const auto& a, const auto& b) { return table_order.at(a.name) < table_order.at(b.name); });
        }
        for (const auto& location : plan.locations()) if (location.planet) {
            const auto object = required(catalog.resolve(location.original_type, data::Category::game_object));
            ai::GalacticSelectionContentPlanet row; row.terrain = text(object, "Terrain");
            row.restricted_categories = bits(text(object, "Planet_Restricted_Unit_Categories"));
            for (const auto& name : ai::split_names(text(object, "Planet_Restricted_Unit_Types"), ", |\t\r\n"))
                if (const auto id = identity(name, location.original_type + " Planet_Restricted_Unit_Types")) row.restricted_types.push_back(*id);
            // WGA-272/279: copied base list, then each ordered occurrence
            // appends one pair. Omission is the qualified constructor-empty list.
            std::vector<const data::Definition*> layers;
            for (const auto& name : object.chain) {
                const auto* layer = catalog.find(name, data::Category::game_object);
                if (!layer) throw std::runtime_error("indigenous authoring layer unavailable");
                layers.push_back(layer);
            }
            for (const auto* value : data::retail::list(layers, "Potential_Indigenous_Power", data::retail::ListRule::append)) {
                data::tag_trace::used(*value);
                const auto tokens = ai::split_names(value->raw_text, " ,\t\r\n");
                if (tokens.size() < 2) continue;
                std::string_view number = tokens[1];
                if (!number.empty() && number.front() == '+') {
                    number.remove_prefix(1);
                    if (!number.empty() && number.front() == '-') number.remove_prefix(number.size());
                }
                std::int32_t power{};
                const auto decoded = std::from_chars(number.data(), number.data() + number.size(), power);
                if (decoded.ec == std::errc::result_out_of_range) throw std::runtime_error("indigenous power overflow admission unqualified");
                const auto potential_type = identity(tokens[0], location.original_type + " Potential_Indigenous_Power");
                if (!potential_type) continue;
                ai::GalacticIndigenousPotential potential; potential.type = *potential_type;
                const auto metrics = std::find_if(out->ai.types.begin(), out->ai.types.end(), [&](const auto& type) { return type.type_id == potential.type; });
                if (metrics == out->ai.types.end()) throw std::runtime_error("indigenous original categories unavailable");
                potential.categories = metrics->category_bits; potential.power = ai::to_single(ai::real(power));
                const auto original = required(units::resolve_initial_galactic_type(types, tokens[0]));
                for (const auto& player : plan.player_bindings())
                    if (std::any_of(original.affiliation.begin(), original.affiliation.end(), [&](const auto& faction) { return ai::upper_case(faction) == ai::upper_case(player.faction); }))
                        potential.affiliations.push_back(player.player.player_id);
                row.indigenous_potential.push_back(std::move(potential));
            }
            out->planets.emplace(location.planet->id, std::move(row));
        }
        for (auto& [id, type] : out->types) {
            (void)id;
            // WGA-483: an unresolved authored link is a null lookup, not a
            // failure of the complete selection content publication.
            if (type.starbase && !type.original->next_level_base.empty()) {
                const auto next = identities.find(ai::upper_case(type.original->next_level_base));
                if (next != identities.end()) type.next_base = next->second;
            }
        }
        for (const auto& binding : plan.player_bindings()) {
            const auto* faction = data.faction(binding.faction);
            if (!faction || !faction->hyperspace_speed_factor) throw std::runtime_error("selection requires faction hyperspace factor");
            out->faction_speed.emplace(binding.player.player_id, script::numeric::from_fixed(*faction->hyperspace_speed_factor));
            // WGA-481: the debug build's initial-base resolver names only
            // these two roots; later levels follow the type's authored chain.
            const auto faction_name = ai::upper_case(binding.faction);
            const auto first_base = faction_name == "REBEL" ? "REBEL_STAR_BASE_1"
                : faction_name == "EMPIRE" ? "EMPIRE_STAR_BASE_1" : "";
            if (const auto first = identities.find(first_base); first != identities.end())
                out->initial_starbase_types.emplace(binding.player.player_id, first->second);
        }
        const auto terrain = required(data::load_document(filesystem, "Data/XML/AITerrainEffectiveness.xml"));
        for (const auto& row : terrain.root.children) for (const auto& cell : row.children) {
            data::tag_trace::used(cell); const auto value = required(data::fixed_value(cell));
            out->terrain[ai::upper_case(row.name)][ai::upper_case(cell.name)] = script::numeric::from_fixed(value);
        }
        // The ignored similarly named XML tag is deliberately not read. These
        // two constructor-owned constants are qualified by WGA-250.
        out->technology_weight = ai::real(1);
        out->population_check = false;
        return Result::success(std::move(out));
    } catch (const std::exception& error) {
        core::Diagnostic diagnostic; diagnostic.code = "galactic-ai.selection-content"; diagnostic.message = error.what();
        return Result::failure(std::move(diagnostic));
    }
}
}
