#include "eawr/platform/campaign_scoring.hpp"

#include "eawr/core/text.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::platform {
namespace {
namespace lua = script::authoritative;
using Number = script::numeric::LuaNumber;
using Fixed = sim::math::Fixed;
using Void = core::Result<void>;
using Values = core::Result<lua::ValueList>;

constexpr std::uint32_t player_handle = 1, type_handle = 2, death_handle = 3, planet_handle = 4, planet_type_handle = 5;
constexpr std::uint64_t scoring_instance = 1;
// Bounded events per scheduler service, so a long campaign never meets the per-service
// instruction quota in one slice.
constexpr std::size_t events_per_service = 32;

core::Diagnostic error(std::string message) {
    return {.code = "EAWR-PLATFORM-SCORING", .message = std::move(message),
        .logical_path = {}, .line = {}, .column = {}, .source_id = {}};
}
lua::Value handle(const std::uint32_t kind, const std::uint64_t id) { return {lua::Handle{kind, id}}; }
Number numeric(const Fixed value) { return Number(value.raw()) / Number(Fixed::scale); }
lua::Value number(const Fixed value) { return lua::Value::number(numeric(value)); }
Values one(lua::Value value) { return Values::success({std::move(value)}); }
std::uint64_t id(const lua::ValueList& args) {
    const auto* value = args.empty() ? nullptr : std::get_if<lua::Handle>(&args.front().data);
    return value ? value->id : 0;
}
std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

struct TypeFacts {
    std::string name;
    Fixed score, build, tactical;
    Number power;
};
struct DeathFacts { sim::galactic::PlayerId owner{}; sim::galactic::TypeId type{}; };
struct PlanetFacts { sim::galactic::PlanetId planet{}; sim::galactic::PlayerId owner{}; };

bool authored_boolean(const data::EffectiveObject& object, std::string_view tag, bool fallback) {
    if (const auto* field = object.value(tag)) {
        data::tag_trace::used(field->value);
        return data::retail::boolean(core::trim(field->value.raw_text)).value_or(fallback);
    }
    return fallback;
}
std::optional<Fixed> authored_fixed(const data::EffectiveObject& object, std::string_view tag) {
    const auto* field = object.value(tag);
    if (!field) return std::nullopt;
    data::tag_trace::used(field->value);
    if (auto parsed = Fixed::from_decimal(core::trim(field->value.raw_text))) return parsed.value();
    return std::nullopt;
}

// WBF-45/46: the scoring type's cost and combat metrics; the debug-build type defaults both
// to zero. Squadron combat power follows the authored team-size clamp.
std::map<sim::galactic::TypeId, TypeFacts> type_facts(const CampaignScoringSource& source) {
    std::map<sim::galactic::TypeId, TypeFacts> out;
    for (const auto& [type_id, name] : source.type_names) {
        const auto* type = source.types->find(name);
        TypeFacts facts{name, {}, type && type->build_cost_credits ? *type->build_cost_credits : Fixed{}, {}, Number(0)};
        if (auto object = source.catalog->resolve(name, data::Category::game_object)) {
            const auto& effective = object.value();
            facts.score = authored_fixed(effective, "Score_Cost_Credits").value_or(Fixed{});
            facts.power = numeric(authored_fixed(effective, "AI_Combat_Power").value_or(Fixed{}));
            if (const auto build = authored_fixed(effective, "Build_Cost_Credits")) facts.build = *build;
            facts.tactical = authored_fixed(effective, "Tactical_Build_Cost_Multiplayer").value_or(Fixed{});
            if (type && !type->squadron_units.empty()) {
                const auto member = source.catalog->resolve(type->squadron_units.front(), data::Category::game_object);
                if (member && authored_boolean(member.value(), "Create_Team", false)) {
                    auto maximum = Number(10); // verified debug-build type default (WBF-46)
                    if (const auto size = authored_fixed(effective, "Max_Squad_Size"); size && size->raw() % Fixed::scale == 0)
                        maximum = numeric(*size);
                    facts.power = facts.power * std::clamp(maximum / Number(type->squadron_units.size()), Number(0), Number(1));
                }
            }
        }
        out.emplace(type_id, std::move(facts));
    }
    return out;
}

struct Host {
    std::optional<lua::ScriptScheduler> scheduler;
    std::map<sim::galactic::TypeId, TypeFacts> types;
    std::map<sim::galactic::PlayerId, std::string> factions;
    std::map<sim::galactic::PlanetId, std::string> planet_types;
    std::vector<DeathFacts> deaths;
    std::vector<PlanetFacts> planets;
    std::uint64_t sequence{};
    std::size_t queued{};

    Void event(const std::uint64_t tick, std::string name, lua::ValueList args,
        const lua::ScriptEvent::Kind kind = lua::ScriptEvent::Kind::call) {
        return scheduler->submit_event({.key = {tick, lua::first_simulation_producer, 1, sequence++}, .target = scoring_instance,
            .kind = kind, .name = std::move(name), .arguments = std::move(args), .parameter = {}});
    }
    Void service() {
        sim::InlineExecutor executor;
        lua::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
        const auto result = scheduler->service(executor, options);
        if (!result) return Void::failure(result.error());
        if (!result.value().diagnostics.empty()) {
            const auto& diagnostic = result.value().diagnostics.front();
            return Void::failure(error(diagnostic.code + ": " + diagnostic.message));
        }
        for (const auto& command : result.value().commands) {
            // The only scoring side effect of the stock module is its tactical fog policy, which a
            // campaign end never reads; refuse any other so a mod's effect is never dropped silently.
            if (command.verb != "results-fog-policy")
                return Void::failure(error("unsupported scoring-host side effect: " + command.verb));
        }
        if (!result.value().removed_instances.empty()) return Void::failure(error("mounted scoring instance stopped"));
        return Void::success();
    }
    Void post(std::string name, lua::ValueList args) {
        if (auto posted = event(scheduler->completed_tick() + 1U, std::move(name), std::move(args)); !posted) return posted;
        if (++queued < events_per_service) return Void::success();
        queued = 0;
        return service();
    }
    Void flush() {
        queued = 0;
        return service();
    }
};

Void bind_host(Host& host) {
    auto& scheduler = *host.scheduler;
    const auto bind = [&](const std::string_view name, lua::Binding binding) { return scheduler.register_binding(name, std::move(binding)); };
    for (const auto name : {"_ScriptMessage", "_OuputDebug"}) {
        if (auto bound = bind(name, [](lua::BindingContext&, const lua::ValueList&) { return Values::success({}); }); !bound) return bound;
    }
    if (auto bound = bind("GlobalValue.Set", [](lua::BindingContext& context, const lua::ValueList& args) {
        const auto* key = args.size() == 2 ? std::get_if<std::string>(&args[0].data) : nullptr;
        if (key == nullptr || *key != "Allow_AI_Controlled_Fog_Reveal" || !std::holds_alternative<Number>(args[1].data))
            return Values::failure(error("unsupported scoring GlobalValue.Set key or value"));
        context.issue_command("results-fog-policy", {args[1]});
        return Values::success({});
    }); !bound) return bound;
    if (auto bound = bind("StringCompare", [](lua::BindingContext&, const lua::ValueList& args) {
        if (args.size() != 2) return Values::failure(error("StringCompare expects two strings"));
        const auto* left = std::get_if<std::string>(&args[0].data);
        const auto* right = std::get_if<std::string>(&args[1].data);
        return one(lua::Value{left && right && *left == *right});
    }); !bound) return bound;
    const auto method = [&](const std::uint32_t kind, std::string_view name, lua::Binding binding) {
        return scheduler.register_method(kind, name, std::move(binding));
    };
    for (const auto kind : {player_handle, type_handle, death_handle, planet_handle, planet_type_handle}) {
        if (auto bound = method(kind, "Is_Valid", [](lua::BindingContext&, const lua::ValueList&) { return one(lua::Value{true}); }); !bound)
            return bound;
    }
    Host* state = &host;
    if (auto bound = method(player_handle, "Get_ID", [](lua::BindingContext&, const lua::ValueList& args) {
        return one(lua::Value::number(Number(id(args))));
    }); !bound) return bound;
    for (const auto name : {"Get_Name", "Get_Faction_Name"}) {
        if (auto bound = method(player_handle, name, [state](lua::BindingContext&, const lua::ValueList& args) {
            const auto found = state->factions.find(static_cast<sim::galactic::PlayerId>(id(args)));
            return one(lua::Value::text(found == state->factions.end() ? "NEUTRAL" : upper(found->second)));
        }); !bound) return bound;
    }
    for (const auto name : {"Get_Name", "Get_Score_Cost_Credits", "Get_Combat_Rating", "Get_Build_Cost", "Get_Tactical_Build_Cost"}) {
        if (auto bound = method(type_handle, name, [state, name](lua::BindingContext&, const lua::ValueList& args) {
            const auto found = state->types.find(id(args));
            // A type outside the mounted closure keeps the debug-build defaults (zero).
            static const TypeFacts unknown{"Unknown", {}, {}, {}, Number(0)};
            const auto& type = found == state->types.end() ? unknown : found->second;
            if (std::string_view(name) == "Get_Name") return one(lua::Value::text(type.name));
            if (std::string_view(name) == "Get_Combat_Rating") return one(lua::Value::number(type.power));
            const Fixed value = std::string_view(name) == "Get_Score_Cost_Credits" ? type.score
                : std::string_view(name) == "Get_Build_Cost" ? type.build : type.tactical;
            return one(number(value));
        }); !bound) return bound;
    }
    if (auto bound = method(planet_type_handle, "Get_Name", [state](lua::BindingContext&, const lua::ValueList& args) {
        const auto found = state->planet_types.find(id(args));
        return one(lua::Value::text(found == state->planet_types.end() ? "Unknown" : found->second));
    }); !bound) return bound;
    for (const auto name : {"Get_Owner", "Get_Game_Scoring_Type", "Get_Type"}) {
        if (auto bound = method(death_handle, name, [state, name](lua::BindingContext&, const lua::ValueList& args) {
            const auto index = id(args);
            if (index == 0 || index > state->deaths.size()) return Values::failure(error("invalid scoring death handle"));
            const auto& death = state->deaths[static_cast<std::size_t>(index - 1)];
            return one(std::string_view(name) == "Get_Owner" ? handle(player_handle, death.owner) : handle(type_handle, death.type));
        }); !bound) return bound;
    }
    for (const auto name : {"Get_Owner", "Get_Type"}) {
        if (auto bound = method(planet_handle, name, [state, name](lua::BindingContext&, const lua::ValueList& args) {
            const auto index = id(args);
            if (index == 0 || index > state->planets.size()) return Values::failure(error("invalid scoring planet handle"));
            const auto& planet = state->planets[static_cast<std::size_t>(index - 1)];
            return one(std::string_view(name) == "Get_Owner" ? handle(player_handle, planet.owner) : handle(planet_type_handle, planet.planet));
        }); !bound) return bound;
    }
    return Void::success();
}

} // namespace

core::Result<std::optional<CampaignScoringOutcome>> evaluate_campaign_scoring(
    const CampaignScoringSource& source, const sim::galactic::CampaignScoreLedger& ledger,
    const std::span<const CampaignScoringPlayer> players, const sim::galactic::PlayerId local) {
    using Result = core::Result<std::optional<CampaignScoringOutcome>>;
    using Absent = std::optional<CampaignScoringOutcome>;
    if (!source.files || !source.catalog || !source.types)
        return Result::failure(error("campaign scoring source is incomplete"));
    // WGP-111: the module is the one the authored constants name, in the effective layering.
    const auto constants = data::load_document(*source.files, "data/xml/gameconstants.xml");
    if (!constants) return Result::success(Absent{});
    std::string module;
    for (const auto& child : constants.value().root.children) {
        if (child.name == "Game_Scoring_Script_Name") {
            data::tag_trace::used(child);
            module = core::trim(child.raw_text);
        }
    }
    if (module.empty()) return Result::success(Absent{});
    if (module.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
        return Result::failure(error("Game_Scoring_Script_Name is not a module name"));

    lua::ModuleManifest manifest;
    auto libraries = source.files->enumerate("data/scripts/library/", ".lua");
    if (!libraries) return Result::failure(libraries.error());
    std::vector<std::string> paths;
    for (const auto& library : libraries.value()) paths.push_back(library.canonical_path);
    const auto module_path = "data/scripts/miscellaneous/" + module + ".lua";
    paths.push_back(module_path);
    for (const auto& path : paths) {
        const auto bytes = source.files->open(path);
        if (!bytes) {
            if (path == module_path) return Result::success(Absent{}); // named module is not authored
            return Result::failure(bytes.error());
        }
        std::string text(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), text.begin(), [](std::byte value) { return static_cast<char>(value); });
        if (auto added = manifest.add(path, std::move(text)); !added) return Result::failure(added.error());
    }
    // Own adapter only; every scoring equation remains in the mounted module. The control query
    // is the campaign (non-tactical) one: the galactic tables, WGP-97.
    const std::string adapter = "require(\"" + module + "\")\nEAWR_Result_Value = 0\n"
        "function EAWR_Query_Result(player, control)\n"
        "  EAWR_Result_Value = Get_Game_Stat_For_Control_ID(player, control, false)\nend\n";
    if (auto added = manifest.add("eawr/campaign_scoring_adapter.lua", adapter); !added) return Result::failure(added.error());
    auto created = lua::ScriptScheduler::create({.script_directories = {"data/scripts/library/", "data/scripts/miscellaneous/"}}, std::move(manifest));
    if (!created) return Result::failure(created.error());

    Host host;
    host.scheduler.emplace(std::move(created).value());
    host.types = type_facts(source);
    host.planet_types = source.planet_types;
    for (const auto& row : players) host.factions[row.player] = row.faction;
    if (auto bound = bind_host(host); !bound) return Result::failure(bound.error());
    if (auto instance = host.scheduler->create_instance(scoring_instance, "eawr/campaign_scoring_adapter.lua"); !instance)
        return Result::failure(instance.error());
    for (const auto& [name, args] : std::vector<std::pair<std::string, lua::ValueList>>{
        {"Base_Definitions", {}}, {"Game_Mode_Starting_Event", {lua::Value::text("Galactic"), lua::Value::text(source.campaign_name)}}}) {
        if (auto posted = host.event(1, name, args); !posted) return Result::failure(posted.error());
    }
    if (auto posted = host.event(1, "main", {}, lua::ScriptEvent::Kind::start_thread); !posted) return Result::failure(posted.error());
    if (auto initialized = host.service(); !initialized) return Result::failure(initialized.error());

    // WGP-99/98/101/102: replay the counters through the authored galactic event handlers.
    // The neutral player (id zero) stands in for the missing-id sentinel (WGP-104).
    for (const auto& [key, count] : ledger.builds) {
        host.planets.push_back({key.planet, key.owner});
        const auto planet = handle(planet_handle, host.planets.size());
        for (std::uint64_t i = 0; i < count; ++i) {
            if (auto posted = host.post("Galactic_Production_End_Event", {planet, handle(type_handle, key.type)}); !posted)
                return Result::failure(posted.error());
        }
    }
    for (const auto& [key, count] : ledger.deaths) {
        host.deaths.push_back({key.owner, key.type});
        const auto victim = handle(death_handle, host.deaths.size());
        for (std::uint64_t i = 0; i < count; ++i) {
            if (auto posted = host.post("Galactic_Unit_Destroyed_Event", {victim, handle(player_handle, key.killer)}); !posted)
                return Result::failure(posted.error());
        }
    }
    for (const auto& [key, count] : ledger.changes) {
        host.planets.push_back({key.planet, key.new_owner});
        const auto planet = handle(planet_handle, host.planets.size());
        for (std::uint64_t i = 0; i < count; ++i) {
            if (auto posted = host.post("Galactic_Planet_Faction_Change",
                {planet, handle(player_handle, key.new_owner), handle(player_handle, key.old_owner)}); !posted)
                return Result::failure(posted.error());
        }
    }
    for (const auto player : ledger.quits) {
        if (auto posted = host.post("Player_Quit_Event", {handle(player_handle, player)}); !posted) return Result::failure(posted.error());
    }
    if (auto flushed = host.flush(); !flushed) return Result::failure(flushed.error());

    const auto query = [&](std::string_view control) -> core::Result<lua::Value> {
        using Out = core::Result<lua::Value>;
        if (auto posted = host.event(host.scheduler->completed_tick() + 1U, "EAWR_Query_Result",
            {handle(player_handle, local), lua::Value::text(std::string(control))}); !posted) return Out::failure(posted.error());
        if (auto serviced = host.service(); !serviced) return Out::failure(serviced.error());
        const auto value = host.scheduler->read_global(scoring_instance, "EAWR_Result_Value");
        if (!value) return Out::failure(value.error());
        if (!value.value()) return Out::failure(error("mounted scoring query returned no value"));
        return Out::success(*value.value());
    };
    // WGP-97: a string title is a key; an absent or non-numeric efficiency stays absent.
    const auto efficiency = [&](std::string_view control) -> core::Result<std::optional<double>> {
        auto value = query(control);
        if (!value) return core::Result<std::optional<double>>::failure(value.error());
        if (const auto* numeric_value = std::get_if<Number>(&value.value().data))
            return core::Result<std::optional<double>>::success(std::bit_cast<double>(numeric_value->repr));
        return core::Result<std::optional<double>>::success(std::nullopt);
    };
    CampaignScoringOutcome outcome;
    const std::array controls{"IDC_MILITARY_EFFICIENCY_STATIC", "IDC_CONQUEST_EFFICIENCY_STATIC", "IDC_KILL_EFFICIENCY_STATIC"};
    for (std::size_t index = 0; index < controls.size(); ++index) {
        auto value = efficiency(controls[index]);
        if (!value) return Result::failure(value.error());
        outcome.efficiencies[index] = value.value();
    }
    auto title = query("IDC_TITLE_STATIC");
    if (!title) return Result::failure(title.error());
    if (const auto* text = std::get_if<std::string>(&title.value().data)) outcome.title_key = *text;
    return Result::success(Absent{std::move(outcome)});
}

} // namespace eawr::platform
