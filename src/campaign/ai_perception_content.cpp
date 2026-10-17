#include "eawr/campaign/ai_perception_content.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/units/galactic_types.hpp"
#include "../script/foc/galactic_ai_selection_content.hpp"
#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <syncstream>

namespace eawr::campaign {
namespace {
namespace ai = script::foc::ai;
template<class T> T required(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
}
core::Result<std::shared_ptr<const script::foc::ai::GalacticPerceptionSetup>> prepare_ai_perception_content(
    const vfs::Vfs& filesystem, const data::galactic::GalacticData& data, const CampaignSetupPlan& plan,
    const script::foc::ai::GalacticSelectionContent& content, std::string_view difficulty) {
    using Result = core::Result<std::shared_ptr<const ai::GalacticPerceptionSetup>>;
    try {
        auto out = std::make_shared<ai::GalacticPerceptionSetup>();
        const auto* campaign = data.campaign(plan.campaign_name());
        if (!campaign) throw std::runtime_error("stock perception campaign unavailable");
        out->is_story_campaign = campaign->is_story_campaign.value_or(false); // WGS-10 / WGA-423.
        out->seed = static_cast<std::uint32_t>(plan.galaxy().seed); out->types = content.ai.types;
        const auto* radius = data.constant("MaximumFleetMovementDistance");
        if (!radius || !radius->number) throw std::runtime_error("stock force neighbourhood radius unavailable");
        out->maximum_fleet_distance = script::numeric::from_fixed(*radius->number);
        // WGA-475: absent or zero leaves TargetPoliticalControl unavailable instead of dividing.
        if (const auto* control = data.constant("MaximumPoliticalControl"); control && control->number && control->number->raw())
            out->maximum_political_control = script::numeric::from_fixed(*control->number);
        const auto* fog = data.constant("AIUsesFogOfWarGalactic");
        const auto uses_fog = fog ? data::retail::boolean(fog->text) : std::nullopt;
        if (!uses_fog) throw std::runtime_error("authored galactic AI fog flag unavailable");
        out->uses_fog = *uses_fog;
        const auto constants = required(data::load_document(filesystem, "Data/XML/GameConstants.xml"));
        const auto maximum = [&](std::string_view tag) {
            const auto row = std::find_if(constants.root.children.begin(), constants.root.children.end(),
                [&](const auto& value) { return value.name == tag; });
            if (row == constants.root.children.end()) throw std::runtime_error("stock base maximum unavailable");
            const auto value = script::numeric::from_fixed(required(data::fixed_value(*row)));
            if (!(ai::Real{} < value)) throw std::runtime_error("stock base maximum nonpositive");
            return value;
        };
        out->maximum_ground_level = maximum("MaximumGroundbaseLevel");
        out->maximum_space_level = maximum("MaximumStarbaseLevel");
        const auto slots = std::find_if(constants.root.children.begin(), constants.root.children.end(),
            [](const auto& row) { return row.name == "MaximumSpecialStructures"; });
        if (slots == constants.root.children.end()) throw std::runtime_error("stock structure normalization unavailable");
        const auto maximum_slots = script::numeric::from_fixed(required(data::fixed_value(*slots)));
        if (!(ai::Real{} < maximum_slots)) throw std::runtime_error("stock structure normalization nonpositive");
        ai::Real largest_income{};
        for (const auto& location : plan.locations()) if (location.planet) {
            const auto* planet = data.planet(location.original_type);
            if (!planet || !planet->planet_credit_value) throw std::runtime_error("stock planet income unavailable");
            largest_income = std::max(largest_income, script::numeric::from_fixed(*planet->planet_credit_value));
        }
        if (!(ai::Real{} < largest_income)) throw std::runtime_error("stock income normalization nonpositive");
        for (const auto& binding : plan.player_bindings()) {
            const auto* faction = data.faction(binding.faction);
            if (!faction) throw std::runtime_error("stock perception faction unavailable");
            const auto seat = std::find_if(plan.galaxy().seats.begin(), plan.galaxy().seats.end(),
                [&](const auto& row) { return row.player == binding.player.player_id; });
            const bool human = seat != plan.galaxy().seats.end() && seat->kind == sim::galactic::SeatKind::human;
            const bool ai_player = seat != plan.galaxy().seats.end() && seat->kind == sim::galactic::SeatKind::ai;
            ai::GalacticPlayerFacts row{binding.player.player_id, binding.player.player_id, binding.faction,
                ai::upper_case(difficulty), faction->is_neutral.value_or(false), ai_player, human, 0};
            for (const auto& home : plan.homes()) if (home.player == row.player && home.planet) row.home = *home.planet;
            if (!faction->maintenance_cost && !row.neutral) throw std::runtime_error("stock faction maintenance unavailable");
            out->faction_maintenance.emplace(row.player, row.neutral ? ai::Real{}
                : script::numeric::from_fixed(*faction->maintenance_cost));
            out->players.push_back(std::move(row));
        }
        for (const auto& location : plan.locations()) if (location.planet) {
            const auto* planet = data.planet(location.original_type);
            const auto authored = content.planets.find(location.planet->id);
            if (authored == content.planets.end()) throw std::runtime_error("stock indigenous authoring unavailable");
            const auto original = std::find_if(content.types.begin(), content.types.end(), [&](const auto& row) {
                return row.second.name == ai::upper_case(location.original_type);
            });
            if (original == content.types.end()) throw std::runtime_error("stock planet constructor identity unavailable");
            out->planets.push_back({location.planet->id, location.original_type, original->first,
                ai::to_single(script::numeric::from_fixed(*planet->planet_credit_value) / largest_income),
                planet->special_structures_land ? std::optional{ai::to_single(ai::real(*planet->special_structures_land) / maximum_slots)} : std::nullopt,
                planet->special_structures_land ? std::optional{ai::real(std::min<std::int64_t>(*planet->special_structures_land, 5)) / *out->maximum_ground_level} : std::nullopt,
                {}, authored->second.indigenous_potential, planet->planet_retains_residual_influence.value_or(true)});
        }
        const auto identity = [&](std::string_view name) {
            for (const auto& [id, original] : content.types)
                if (original.name == ai::upper_case(name)) return id;
            throw std::runtime_error("stock force spawn original identity unavailable");
        };
        if (content.registered_types.size() != content.types.size())
            throw std::runtime_error("registered economy type order is incomplete");
        for (const auto id : content.registered_types) {
            const auto& original_value = content.types.at(id);
            const auto* type = original_value.original.get();
            const auto original = content.types.find(id);
            const auto metrics = std::find_if(content.ai.types.begin(), content.ai.types.end(),
                [&](const auto& row) { return row.type_id == id; });
            if (!type || original == content.types.end() || metrics == content.ai.types.end())
                throw std::runtime_error("stock force original metrics unavailable");
            ai::GalacticForceType force; force.type = id; force.categories = metrics->category_bits;
            force.power = metrics->combat_power; force.transport = original->second.transport;
            force.space_weapon = original->second.special_weapon;
            force.space_structure_categories = original->second.garrison_categories;
            force.object_traits = ai::GalacticObjectTraits{original->second.value.hero, original->second.named_hero};
            for (std::size_t tech = 0; tech < force.starting_spawns.size(); ++tech)
                for (const auto& spawn : type->starting_spawned_units[tech]) force.starting_spawns[tech].push_back({identity(spawn.type), spawn.count});
            const auto behaves = [&](std::string_view name) {
                return std::any_of(type->behavior.begin(), type->behavior.end(),
                    [&](const auto& value) { return ai::upper_case(value) == name; });
            };
            ai::GalacticEconomyType economy; economy.type = id;
            economy.generic_price = ai::to_single(ai::real(ai::truncate(original->second.value.cost)));
            economy.generic_seconds = original->second.value.build_seconds;
            // WGA-291: constructor omissions after resolved inheritance are zero.
            economy.maintenance = script::numeric::from_fixed(type->maintenance_cost.value_or(sim::math::Fixed{}));
            economy.affiliations = type->affiliation; economy.tech_level = original->second.value.tech;
            economy.advances_tech = static_cast<std::uint32_t>(type->build_advances_tech_level.value_or(0));
            economy.initially_locked = original->second.initially_locked;
            economy.ground_unit = behaves("DUMMY_GROUND_COMPANY");
            economy.space_unit = behaves("DUMMY_STARSHIP") || behaves("DUMMY_SPACE_FIGHTER_SQUADRON");
            economy.ground_structure = behaves("DUMMY_GROUND_STRUCTURE");
            economy.space_structure = behaves("DUMMY_ORBITAL_STRUCTURE");
            economy.built_on_ground = original->second.value.channel == sim::galactic::ProductionDomain::ground;
            out->economy_types.push_back(std::move(economy));
            out->force_types.push_back(std::move(force));
        }
        out->economy_types_complete = true;
        // WGA-426..428/464: discover authored hint sets; no stock filename list.
        std::map<std::string, std::map<std::string, ai::Real, std::less<>>, std::less<>> sets;
        const auto log = [&](std::string text) {
            // WGA-426/427: recoverable authoring failures remain visible at preparation.
            core::Diagnostic diagnostic;
            diagnostic.code = "galactic-ai.markup-content";
            diagnostic.severity = core::Severity::warning;
            diagnostic.message = text;
            std::osyncstream(std::clog) << core::format_diagnostic(diagnostic) << '\n';
            out->markup_diagnostics.push_back(std::move(text));
        };
        const auto number = [&](const data::XmlNode& node) {
            const auto value = script::numeric::decimal::parse_prefix(node.raw_text).value;
            if (!script::numeric::binary64::is_finite(value)) { log("nonfinite markup hint: " + node.name); return ai::Real{}; }
            return ai::Real::from_repr(value);
        };
        std::set<std::string> vocabulary;
        const auto tokens = data::load_document(filesystem, "data/xml/enum/perceptiontokentype.xml");
        if (tokens) for (const auto& token : tokens.value().root.children) vocabulary.insert(ai::upper_case(token.name));
        else log("unreadable perception token vocabulary");
        const auto files = filesystem.enumerate("data/xml/ai/hintsets", ".xml");
        if (files) for (const auto& asset : files.value()) {
            const auto document = data::load_document(filesystem, asset.canonical_path);
            if (!document) { log("unreadable hint set: " + asset.canonical_path); continue; }
            for (const auto& set : document.value().root.children) {
                std::map<std::string, ai::Real, std::less<>> hints;
                for (const auto& hint : set.children) hints.try_emplace(ai::upper_case(hint.name), number(hint));
                for (const auto& [name, value] : hints) { static_cast<void>(value); vocabulary.insert(name); }
                if (hints.empty()) log("empty hint set: " + set.name);
                if (!sets.try_emplace(ai::upper_case(set.name), std::move(hints)).second) log("duplicate hint set: " + set.name);
            }
        }
        for (const auto& player : out->players) {
            const auto authored = std::find_if(campaign->markup_filenames.begin(), campaign->markup_filenames.end(),
                [&](const auto& row) { return ai::upper_case(row.faction) == ai::upper_case(player.faction); });
            if (authored == campaign->markup_filenames.end()) continue;
            const auto path = "data/xml/ai/galacticmarkup/" + authored->name + ".xml";
            const auto document = data::load_document(filesystem, path);
            if (!document) { log("unreadable galactic markup: " + path); continue; }
            const auto planets = std::find_if(document.value().root.children.begin(), document.value().root.children.end(),
                [](const auto& row) { return ai::upper_case(row.name) == "PLANETS"; });
            if (planets == document.value().root.children.end()) { log("galactic markup has no Planets: " + path); continue; }
            const auto attribute = std::find_if(planets->attributes.begin(), planets->attributes.end(),
                [](const auto& row) { return ai::upper_case(row.name) == "HINTSET"; });
            if (attribute == planets->attributes.end() || attribute->value.empty()) { log("galactic markup has no HintSet: " + path); continue; }
            ai::GalacticMarkupHints selected; selected.set = ai::upper_case(attribute->value);
            if (sets.contains(selected.set)) selected.defaults = sets.at(selected.set);
            for (const auto& planet : planets->children) {
                const auto name = ai::upper_case(planet.name);
                if (selected.planets.contains(name)) { log("duplicate markup planet: " + name); continue; }
                auto& hints = selected.planets[name];
                if (planet.children.empty()) log("empty markup planet: " + name);
                for (const auto& hint : planet.children) {
                    const auto token = ai::upper_case(hint.name);
                    if (!vocabulary.contains(token)) { log("unknown markup hint: " + token); continue; }
                    if (!hints.try_emplace(token, number(hint)).second) log("duplicate markup hint: " + token);
                }
            }
            out->markup_hints.emplace(player.player, std::move(selected));
        }
        return Result::success(std::move(out));
    } catch (const std::exception& error) {
        core::Diagnostic diagnostic; diagnostic.code = "galactic-ai.perception-content"; diagnostic.message = error.what();
        return Result::failure(std::move(diagnostic));
    }
}
}
