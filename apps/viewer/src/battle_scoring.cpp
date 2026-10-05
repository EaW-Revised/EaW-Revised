#include "battle_scoring.hpp"

#include "eawr/data/tag_trace.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <sstream>
#include <utility>

namespace eawr::presentation::godot_backend {
namespace {
namespace lua = script::authoritative;
using Number = script::numeric::LuaNumber;
using Fixed = sim::math::Fixed;
using Void = core::Result<void>;
using Values = core::Result<lua::ValueList>;
constexpr std::uint32_t player_handle = 1, type_handle = 2, loss_handle = 3;
constexpr std::uint64_t scoring_instance = 1;

core::Diagnostic error(std::string message) {
    return {.code = "EAWR-VIEWER-SCORING", .message = std::move(message)};
}
std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}
lua::Value handle(const std::uint32_t kind, const std::uint64_t id) { return {lua::Handle{kind, id}}; }
Number numeric(const Fixed value) { return Number(value.raw()) / Number(Fixed::scale); }
lua::Value number(const Fixed value) { return lua::Value::number(numeric(value)); }
Values one(lua::Value value) { return Values::success({std::move(value)}); }
std::uint64_t id(const lua::ValueList& args) {
    const auto* value = args.empty() ? nullptr : std::get_if<lua::Handle>(&args.front().data);
    return value ? value->id : 0;
}
}

struct BattleScoring::Impl {
    struct Type {
        std::string name;
        Fixed score, build, tactical;
        Number power;
    };
    std::optional<lua::ScriptScheduler> scheduler;
    std::map<sim::tactical::TypeId, Type> types;
    std::map<sim::tactical::PlayerId, std::string> factions;
    std::vector<sim::tactical::BattleLoss> losses;
    std::size_t delivered_losses{}, delivered_productions{};
    std::map<sim::tactical::PlayerId, bool> delivered_quits;
    sim::tactical::PlayerId neutral{};
    std::uint64_t sequence{}, pump_count{}, last_pump{};
    Number service_rate{10};
    Number allow_ai_fog_reveal{1}; // isolated scoring-host state; never an AI decision input

    Void event(const std::uint64_t tick, std::string name, lua::ValueList args,
        const lua::ScriptEvent::Kind kind = lua::ScriptEvent::Kind::call) {
        return scheduler->submit_event({.key = {tick, lua::first_simulation_producer, 1, sequence++}, .target = scoring_instance,
            .kind = kind, .name = std::move(name), .arguments = std::move(args)});
    }
    Void service() {
        sim::InlineExecutor executor;
        const auto result = scheduler->service(executor, {.host_paced = [](std::uint64_t) { return true; }});
        if (!result) return Void::failure(result.error());
        for (const auto& diagnostic : result.value().diagnostics)
            return Void::failure(error(diagnostic.code + ": " + diagnostic.message));
        for (const auto& command : result.value().commands) {
            if (command.verb != "results-fog-policy" || command.arguments.size() != 1)
                return Void::failure(error("unsupported scoring-host side effect: " + command.verb));
            const auto* value = std::get_if<Number>(&command.arguments.front().data);
            if (value == nullptr) return Void::failure(error("scoring fog policy is not numeric"));
            allow_ai_fog_reveal = *value;
        }
        if (!result.value().removed_instances.empty()) return Void::failure(error("mounted scoring instance stopped"));
        return Void::success();
    }
};

BattleScoring::BattleScoring(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BattleScoring::~BattleScoring() = default;

core::Result<std::unique_ptr<BattleScoring>> BattleScoring::create(const vfs::Vfs& files,
    const data::Catalog& catalog, const units::UnitTables& tables,
    std::map<sim::tactical::PlayerId, std::string> factions, std::string map) {
    using Result = core::Result<std::unique_ptr<BattleScoring>>;
    auto constants = data::load_document(files, "data/xml/gameconstants.xml");
    if (!constants) return Result::failure(constants.error());
    std::string module;
    for (const auto& child : constants.value().root.children) {
        if (child.name == "Game_Scoring_Script_Name") {
            data::tag_trace::used(child);
            module = trim(child.raw_text);
        }
    }
    if (module.empty() || module.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
        return Result::failure(error("Game_Scoring_Script_Name is absent or is not a module name"));
    lua::ModuleManifest manifest;
    auto libraries = files.enumerate("data/scripts/library/", ".lua");
    if (!libraries) return Result::failure(libraries.error());
    std::vector<std::string> paths;
    for (const auto& library : libraries.value()) paths.push_back(library.canonical_path);
    paths.push_back("data/scripts/miscellaneous/" + module + ".lua");
    for (const auto& path : paths) {
        const auto bytes = files.open(path);
        if (!bytes) return Result::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), text.begin(), [](std::byte value) { return static_cast<char>(value); });
        if (auto added = manifest.add(path, std::move(text)); !added) return Result::failure(added.error());
    }
    // Own adapter only; every scoring equation remains in the mounted module.
    const std::string adapter = "require(\"" + module + "\")\nEAWR_Result_Value = 0\n"
        "function EAWR_Query_Result(player, control)\n"
        "  EAWR_Result_Value = Get_Game_Stat_For_Control_ID(player, control, true)\nend\n";
    if (auto added = manifest.add("eawr/results_adapter.lua", adapter); !added) return Result::failure(added.error());
    auto created = lua::ScriptScheduler::create({.script_directories = {"data/scripts/library/", "data/scripts/miscellaneous/"}}, std::move(manifest));
    if (!created) return Result::failure(created.error());
    auto state = std::make_unique<Impl>();
    state->scheduler.emplace(std::move(created).value());
    state->factions = std::move(factions);
    for (const auto& unit : tables.units) {
        // WBF-45/46: the debug-build type defaults cost and combat power to zero.
        Impl::Type type{unit.id, unit.score_cost_credits.value_or(Fixed{}), {},
            unit.production.build_cost_multiplayer.value_or(Fixed{}), numeric(unit.score_combat_power.value_or(Fixed{}))};
        if (unit.kind == units::UnitKind::squadron && !unit.members.empty()) {
            const auto member = catalog.resolve(unit.members.front().craft, data::Category::game_object);
            if (!member) return Result::failure(member.error());
            bool create_team = false; // WBF-46: verified debug-build type default.
            if (const auto* field = member.value().value("Create_Team")) {
                data::tag_trace::used(field->value);
                auto flag = trim(field->value.raw_text);
                std::transform(flag.begin(), flag.end(), flag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (flag == "true" || flag == "yes" || flag == "1") create_team = true;
                else if (flag != "false" && flag != "no" && flag != "0")
                    return Result::failure(error("scoring Create_Team is not a boolean"));
            }
            if (create_team) {
                // WBF-46: only a team-creating first member activates the authored size clamp.
                auto maximum = Number(10); // verified debug-build type default
                const auto object = catalog.resolve(unit.id, data::Category::game_object);
                if (!object) return Result::failure(object.error());
                if (const auto* field = object.value().value("Max_Squad_Size")) {
                    data::tag_trace::used(field->value);
                    const auto parsed = Fixed::from_decimal(trim(field->value.raw_text));
                    if (!parsed) return Result::failure(parsed.error());
                    if (parsed.value().raw() % Fixed::scale != 0)
                        return Result::failure(error("scoring Max_Squad_Size is not an integer"));
                    maximum = numeric(parsed.value());
                }
                type.power = type.power * std::clamp(maximum / Number(unit.members.size()), Number(0), Number(1));
            }
        }
        if (auto object = catalog.resolve(unit.id, data::Category::game_object)) {
            if (const auto* build = object.value().value("Build_Cost_Credits")) {
                data::tag_trace::used(build->value);
                if (auto parsed = Fixed::from_decimal(trim(build->value.raw_text))) type.build = parsed.value();
                else return Result::failure(parsed.error());
            }
        }
        state->types.emplace(skirmish::type_id(unit.id), std::move(type));
    }
    Impl* host = state.get();
    auto& scheduler = *state->scheduler;
    const auto bind = [&](const std::string_view name, lua::Binding binding) { return scheduler.register_binding(name, std::move(binding)); };
    for (const auto name : {"_ScriptMessage", "_OuputDebug"}) {
        auto bound = bind(name, [](lua::BindingContext&, const lua::ValueList&) { return Values::success({}); });
        if (!bound) return Result::failure(bound.error());
    }
    if (auto bound = bind("GlobalValue.Set", [](lua::BindingContext& context, const lua::ValueList& args) {
        const auto* key = args.size() == 2 ? std::get_if<std::string>(&args[0].data) : nullptr;
        if (key == nullptr || *key != "Allow_AI_Controlled_Fog_Reveal" || !std::holds_alternative<Number>(args[1].data))
            return Values::failure(error("unsupported scoring GlobalValue.Set key or value"));
        context.issue_command("results-fog-policy", {args[1]});
        return Values::success({});
    }); !bound) return Result::failure(bound.error());
    if (auto bound = bind("StringCompare", [](lua::BindingContext&, const lua::ValueList& args) {
        if (args.size() != 2) return Values::failure(error("StringCompare expects two strings"));
        const auto* left = std::get_if<std::string>(&args[0].data);
        const auto* right = std::get_if<std::string>(&args[1].data);
        return one(lua::Value{left && right && *left == *right});
    }); !bound) return Result::failure(bound.error());
    const auto method = [&](const std::uint32_t kind, std::string_view name, lua::Binding binding) {
        return scheduler.register_method(kind, name, std::move(binding));
    };
    for (const auto kind : {player_handle, type_handle, loss_handle}) {
        if (auto bound = method(kind, "Is_Valid", [](lua::BindingContext&, const lua::ValueList&) { return one(lua::Value{true}); }); !bound)
            return Result::failure(bound.error());
    }
    if (auto bound = method(player_handle, "Get_ID", [](lua::BindingContext&, const lua::ValueList& args) {
        return one(lua::Value::number(Number(id(args))));
    }); !bound) return Result::failure(bound.error());
    for (const auto name : {"Get_Name", "Get_Faction_Name"}) {
        if (auto bound = method(player_handle, name, [host](lua::BindingContext&, const lua::ValueList& args) {
            const auto found = host->factions.find(static_cast<sim::tactical::PlayerId>(id(args)));
            std::string faction = found == host->factions.end() ? "NEUTRAL" : found->second;
            std::transform(faction.begin(), faction.end(), faction.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return one(lua::Value::text(std::move(faction)));
        }); !bound) return Result::failure(bound.error());
    }
    for (const auto name : {"Get_Name", "Get_Score_Cost_Credits", "Get_Combat_Rating", "Get_Build_Cost", "Get_Tactical_Build_Cost"}) {
        if (auto bound = method(type_handle, name, [host, name](lua::BindingContext&, const lua::ValueList& args) {
            const auto found = host->types.find(id(args));
            if (found == host->types.end()) return Values::failure(error("scoring type is outside the loaded unit closure"));
            const auto& type = found->second;
            if (std::string_view(name) == "Get_Name") return one(lua::Value::text(type.name));
            if (std::string_view(name) == "Get_Combat_Rating") return one(lua::Value::number(type.power));
            const Fixed value = std::string_view(name) == "Get_Score_Cost_Credits" ? type.score
                : std::string_view(name) == "Get_Build_Cost" ? type.build : type.tactical;
            return one(number(value));
        }); !bound) return Result::failure(bound.error());
    }
    for (const auto name : {"Get_Owner", "Get_Game_Scoring_Type"}) {
        if (auto bound = method(loss_handle, name, [host, name](lua::BindingContext&, const lua::ValueList& args) {
            const auto index = id(args);
            if (index == 0 || index > host->losses.size()) return Values::failure(error("invalid scoring loss handle"));
            const auto& loss = host->losses[static_cast<std::size_t>(index - 1)];
            return one(std::string_view(name) == "Get_Owner" ? handle(player_handle, loss.owner) : handle(type_handle, loss.type));
        }); !bound) return Result::failure(bound.error());
    }
    if (auto instance = scheduler.create_instance(scoring_instance, "eawr/results_adapter.lua"); !instance) return Result::failure(instance.error());
    for (const auto& [name, args] : std::vector<std::pair<std::string, lua::ValueList>>{
        {"Base_Definitions", {}}, {"Game_Mode_Starting_Event", {lua::Value::text("Space"), lua::Value::text(std::move(map))}}}) {
        if (auto posted = state->event(1, name, args); !posted) return Result::failure(posted.error());
    }
    if (auto posted = state->event(1, "main", {}, lua::ScriptEvent::Kind::start_thread); !posted) return Result::failure(posted.error());
    if (auto initialized = state->service(); !initialized) return Result::failure(initialized.error());
    if (const auto rate = scheduler.read_global(scoring_instance, "ServiceRate"); rate && rate.value()) {
        if (const auto* number_value = std::get_if<Number>(&rate.value()->data)) state->service_rate = *number_value;
    }
    return Result::success(std::unique_ptr<BattleScoring>(new BattleScoring(std::move(state))));
}

Void BattleScoring::observe(const sim::tactical::TacticalSnapshot& snapshot) {
    auto& state = *impl_;
    for (const auto& player : snapshot.players()) if (player.neutral) state.neutral = player.player_id;
    const auto losses = snapshot.losses();
    if (losses.size() < state.losses.size()) return Void::failure(error("scoring journal moved backwards"));
    state.losses.insert(state.losses.end(), losses.begin() + static_cast<std::ptrdiff_t>(state.losses.size()), losses.end());
    while (state.scheduler->completed_tick() < snapshot.completed_tick() + 1U) {
        const auto script_tick = state.scheduler->completed_tick() + 1U;
        const auto world_tick = script_tick - 2U;
        while (state.delivered_losses < state.losses.size() && state.losses[state.delivered_losses].tick <= world_tick) {
            const auto index = state.delivered_losses++;
            const auto& loss = state.losses[index];
            const auto killer = loss.killer != 0 ? loss.killer : state.neutral;
            for (std::uint64_t count = 0; count < loss.count; ++count) {
                if (auto posted = state.event(script_tick, "Tactical_Unit_Destroyed_Event",
                    {handle(loss_handle, index + 1U), handle(player_handle, killer)}); !posted) return posted;
            }
        }
        const auto productions = snapshot.productions();
        while (state.delivered_productions < productions.size() && productions[state.delivered_productions].tick <= world_tick) {
            const auto& production = productions[state.delivered_productions++];
            if (auto posted = state.event(script_tick, "Tactical_Production_End_Event",
                {handle(type_handle, production.type), handle(player_handle, production.owner), lua::Value{}}); !posted) return posted;
        }
        for (const auto& quit : snapshot.quits()) {
            if (quit.tick > world_tick || state.delivered_quits.contains(quit.player)) continue;
            if (auto posted = state.event(script_tick, "Player_Quit_Event", {handle(player_handle, quit.player)}); !posted) return posted;
            state.delivered_quits.emplace(quit.player, true);
        }
        // WBF-46: strict elapsed-time comparison, not every render frame or every tick.
        if (Number(script_tick - 1U - state.last_pump) / Number(30) > state.service_rate) {
            if (auto posted = state.event(script_tick, {}, {}, lua::ScriptEvent::Kind::pump); !posted) return posted;
            state.last_pump = script_tick - 1U;
            ++state.pump_count;
        }
        if (auto serviced = state.service(); !serviced) return serviced;
    }
    return Void::success();
}

core::Result<std::string> BattleScoring::query(const sim::tactical::PlayerId player, const std::string_view control) {
    using Result = core::Result<std::string>;
    auto& state = *impl_;
    if (auto posted = state.event(state.scheduler->completed_tick() + 1U, "EAWR_Query_Result",
        {handle(player_handle, player), lua::Value::text(std::string(control))}); !posted) return Result::failure(posted.error());
    if (auto serviced = state.service(); !serviced) return Result::failure(serviced.error());
    const auto value = state.scheduler->read_global(scoring_instance, "EAWR_Result_Value");
    if (!value) return Result::failure(value.error());
    if (value.value()) {
        if (const auto* text = std::get_if<std::string>(&value.value()->data)) return Result::success(*text);
        if (const auto* numeric = std::get_if<Number>(&value.value()->data)) {
            // Presentation conversion of the existing binary64 profile; no sim arithmetic.
            std::ostringstream output;
            output << std::bit_cast<double>(numeric->repr);
            return Result::success(output.str());
        }
    }
    return Result::failure(error("mounted scoring query returned no number or text"));
}

std::uint64_t BattleScoring::pumps() const noexcept { return impl_->pump_count; }
double BattleScoring::combat_rating(const sim::tactical::TypeId type) const noexcept {
    const auto found = impl_->types.find(type);
    return found == impl_->types.end() ? 0.0 : std::bit_cast<double>(found->second.power.repr);
}
} // namespace eawr::presentation::godot_backend
