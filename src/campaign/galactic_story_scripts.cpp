#include "eawr/campaign/galactic_story_scripts.hpp"
#include "eawr/vfs/vfs.hpp"

#include "galactic_story_random.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "../script/foc/story_plot_bindings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace eawr::campaign {
namespace {

namespace auth = script::authoritative;
namespace story = sim::galactic::story;

constexpr std::string_view library_prefix = "data/scripts/library/";
constexpr std::string_view story_prefix = "data/scripts/story/";
constexpr std::string_view story_directory = "Data/Scripts/Story/";

// Staged by the bindings below, committed in command order by GalacticStoryLink::commit.
constexpr std::string_view verb_story_event = "galactic.story_event";            // name, has object ("1"/"0"), planet name
constexpr std::string_view verb_story_flag_clear = "galactic.story_flag_clear";  // player, planet, name (decimal text)

constexpr std::string_view player_module = "EAWR/Story/Players.lua";
// WST-90: validate in Lua before crossing the host value boundary. Non-string graphs and functions never need conversion.
constexpr std::string_view player_source = R"LUA(
local lookup = EAWR_Story_Player.Lookup
local kind = type
function Find_Player(...)
    if arg.n ~= 1 or kind(arg[1]) ~= "string" then return lookup() end
    return lookup(arg[1])
end
)LUA";

core::Diagnostic problem(std::string message) {
    core::Diagnostic out;
    out.code = std::string(auth::codes::invalid_request);
    out.message = std::move(message);
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& character : out) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return out;
}

std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return std::string(text);
}

std::string upper_text(std::string_view text) {
    std::string out(text);
    for (char& character : out) if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    return out;
}

std::optional<std::uint64_t> number_text(const auth::Value& value) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (!text) return std::nullopt;
    std::uint64_t out{};
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()) return std::nullopt;
    return out;
}

} // namespace

struct GalacticStoryLink::Impl {
    script::foc::StoryPlotLinkHolder plots; // Get_Story_Plot's names and the plot-edit sink
    std::unique_ptr<GalacticStorySink> sink;
};

GalacticStoryLink::GalacticStoryLink() : impl_(std::make_unique<Impl>()) {}
GalacticStoryLink::~GalacticStoryLink() = default;

void GalacticStoryLink::bind(std::span<const story::SubPlotDef> definitions, std::unique_ptr<GalacticStorySink> sink) {
    impl_->sink = std::move(sink);
    auto* target = impl_->sink.get();
    impl_->plots.link = script::foc::StoryPlotLink::from(definitions,
        [target](const story::PlotCommand& command) { return target->plot(command); });
}

core::Result<void> GalacticStoryLink::commit(auth::ServiceReport& report) const {
    namespace foc = script::foc;
    auto* sink = impl_->sink.get();
    core::Result<void> failure = core::Result<void>::success();
    const auto note = [&](std::string object, std::string message) {
        core::LoadNote row;
        row.code = "EAWR-LOAD-1400";
        row.object = std::move(object);
        row.action = core::LoadAction::runtime_degraded;
        row.phase = core::LoadPhase::runtime;
        row.diagnostic_code = std::string(auth::codes::unsupported_api);
        row.message = std::move(message);
        report.load_notes.push_back(std::move(row));
    };
    std::erase_if(report.commands, [&](const auth::ScriptCommand& command) {
        const auto& args = command.arguments;
        if (command.verb == foc::verb_story_plot) {
            if (auto why = foc::detail::commit_story_plot(impl_->plots.link.get(), command); !why.empty())
                note("Get_Story_Plot:" + (args.size() > 1 && std::holds_alternative<std::string>(args[1].data)
                    ? std::get<std::string>(args[1].data) : std::string{}), "story plot request not applied: " + why);
            return true;
        }
        if (command.verb == verb_story_event) {
            const auto* name = args.size() == 3 ? std::get_if<std::string>(&args[0].data) : nullptr;
            const auto* has_object = args.size() == 3 ? std::get_if<std::string>(&args[1].data) : nullptr;
            const auto* planet = args.size() == 3 ? std::get_if<std::string>(&args[2].data) : nullptr;
            if (!name || !has_object || !planet) { note("Story_Event", "malformed story event request"); return true; }
            if (!sink) { note("Story_Event:" + *name, "story event not delivered: no plot engine is hosted"); return true; }
            story::TextNotice notice;
            notice.kind = story::Text::ai_notification;
            notice.text = *name;
            if (*has_object == "1") notice.planet = *planet;
            if (auto done = sink->notify(notice); !done && failure) failure = core::Result<void>::failure(done.error());
            return true;
        }
        if (command.verb == verb_story_flag_clear) {
            const auto player = args.size() == 3 ? number_text(args[0]) : std::nullopt;
            const auto planet = args.size() == 3 ? number_text(args[1]) : std::nullopt;
            const auto* name = args.size() == 3 ? std::get_if<std::string>(&args[2].data) : nullptr;
            if (!player || !planet || !name) { note("Check_Story_Flag", "malformed flag request"); return true; }
            if (sink) sink->clear_flag(static_cast<story::Player>(*player), *planet, *name);
            return true;
        }
        return false;
    });
    return failure;
}

core::Result<std::shared_ptr<const GalacticStoryScripts>> GalacticStoryScripts::load(
    const vfs::Vfs& files, std::span<const story::SubPlotDef> subplots) {
    using Result = core::Result<std::shared_ptr<const GalacticStoryScripts>>;
    auto out = std::make_shared<GalacticStoryScripts>();
    // Nothing to pin when no sub-plot names a script: the story scheduler stays empty, as before this host.
    if (std::none_of(subplots.begin(), subplots.end(), [](const auto& sub) { return !sub.script.empty(); }))
        return Result::success(std::move(out));
    auto assets = files.enumerate("data/scripts", ".lua");
    if (!assets) return Result::failure(assets.error());
    for (const auto& asset : assets.value()) {
        const auto& path = asset.canonical_path;
        if (!path.starts_with(library_prefix) && !path.starts_with(story_prefix)) continue;
        if (out->modules_.contains(path)) continue;
        auto bytes = files.open(path);
        if (!bytes) return Result::failure(bytes.error());
        std::string text(bytes.value().size(), '\0');
        if (!text.empty()) std::memcpy(text.data(), bytes.value().data(), bytes.value().size());
        out->modules_.emplace(path, std::move(text));
    }
    const auto names_symbol = [](const std::string& text, std::string_view symbol) {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (auto at = text.find(symbol); at != std::string::npos; at = text.find(symbol, at + 1)) {
            const auto end = at + symbol.size();
            if ((at == 0 || !word(text[at - 1])) && (end >= text.size() || !word(text[end]))) return true;
        }
        return false;
    };
    for (const auto& [path, text] : out->modules_)
        if (names_symbol(text, "Spawn_Unit") || names_symbol(text, "Prevent_AI_Usage")) { out->uses_immediate_effects_ = true; break; }
    std::uint64_t ordinal = 0;
    for (const auto& sub : subplots) {
        ++ordinal;
        if (sub.script.empty()) continue;
        Entry entry;
        entry.subplot = sub.name;
        entry.script = trimmed(sub.script);
        entry.instance = first_story_script_instance + ordinal;
        std::string path = std::string(story_directory) + entry.script;
        if (lower(path).ends_with(".lua") == false) path += ".lua";
        if (out->modules_.contains(lower(path))) entry.module = std::move(path);
        else out->notes_.push_back(sub.name + ": Lua_Script " + entry.script + " is not a Story module of the mounted data; the sub-plot runs no Lua (WST-53)");
        out->entries_.push_back(std::move(entry));
    }
    return Result::success(std::move(out));
}

const GalacticStoryScripts::Entry* GalacticStoryScripts::find(std::string_view subplot) const noexcept {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.subplot == subplot; });
    return found == entries_.end() ? nullptr : &*found;
}

std::string GalacticStoryScripts::fingerprint() const {
    std::string text = "eawr-galactic-story-scripts-v2";
    const auto field = [&](std::string_view value) { text += std::to_string(value.size()) + ":"; text += value; };
    for (const auto& [path, bytes] : modules_) {
        field(path);
        const auto digest = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        field(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
    }
    for (const auto& entry : entries_) { field(entry.subplot); field(entry.script); field(entry.module); field(std::to_string(entry.instance)); }
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

core::Result<auth::ScriptScheduler> create_galactic_story_scheduler(const GalacticStoryScripts& content, auth::SessionConfig base,
    bool multiplayer, const GalacticStoryPlayers& players, auth::GalacticSnapshotReader reader,
    auth::GlobalValueReader globals, const std::shared_ptr<GalacticStoryLink>& link_in) {
    using Result = core::Result<auth::ScriptScheduler>;
    const auto link = link_in ? link_in : std::make_shared<GalacticStoryLink>();
    // Only scripted campaigns change anything: a story scheduler without scripts keeps its previous identity and hash.
    if (content.entries().empty()) return auth::ScriptScheduler::create(std::move(base), {});
    base.script_directories = {"Data/Scripts/Library/", std::string(story_directory)};
    auth::ModuleManifest manifest;
    for (const auto& [path, bytes] : content.modules())
        if (auto added = manifest.add(path, bytes); !added) return Result::failure(added.error());
    if (auto added = manifest.add(story_random::distribution_module, std::string(story_random::distribution_source)); !added) return Result::failure(added.error());
    if (auto added = manifest.add(player_module, std::string(player_source)); !added) return Result::failure(added.error());
    auto scheduler = auth::ScriptScheduler::create(std::move(base), std::move(manifest));
    if (!scheduler) return scheduler;
    if (auto added = auth::register_shared_global_values(scheduler.value(), std::move(globals)); !added)
        return Result::failure(added.error());
    // RGC-190..199: the weighted list and the two random calls. Everything else the stock and mod story starts need
    // that this host does not implement faults by name below.
    if (auto added = story_random::register_story_random(scheduler.value(), multiplayer); !added) return Result::failure(added.error());
    // WST-91/92: prepare the full faction database once; the first matching player is the lowest player-table ID.
    std::map<std::string, std::optional<sim::galactic::PlayerId>> factions;
    for (const auto& name : players.factions) factions.try_emplace(lower(name), std::nullopt);
    bool local_registered = false, shared_registered = false;
    for (const auto& player : players.players) {
        local_registered |= players.local_player && player.id == *players.local_player;
        shared_registered |= players.shared_local_player && player.id == *players.shared_local_player;
        const auto found = factions.find(lower(player.faction));
        if (found != factions.end() && (!found->second || player.id < *found->second)) found->second = player.id;
    }
    // Project deviation from WST-91: a per-peer local player cannot enter the multiplayer lockstep simulation.
    // The shared co-op faction is the one deterministic exception: every peer derives the same value.
    const auto local = multiplayer ? (shared_registered ? players.shared_local_player : std::nullopt)
                                   : (local_registered ? players.local_player : std::nullopt);
    if (auto added = scheduler.value().register_binding("EAWR_Story_Player.Lookup", [factions = std::move(factions), local](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Lookup = core::Result<auth::ValueList>;
        const auto invalid = [&](std::string message) {
            context.report(auth::codes::invalid_request, std::move(message));
            context.request_exit();
            return Lookup::success({});
        };
        // WST-90: string-only, exact arity; diagnostics stop this script even without a log sink.
        if (arguments.size() != 1 || !std::holds_alternative<std::string>(arguments.front().data))
            return invalid("Find_Player requires exactly one string argument");
        const auto name = lower(std::get<std::string>(arguments.front().data));
        auto player = local;
        if (name != "local") {
            const auto found = factions.find(name);
            if (found == factions.end()) return invalid("Find_Player: unknown faction '" + std::get<std::string>(arguments.front().data) + "'");
            player = found->second;
        }
        // WST-93: the sandbox interns each player's handle independently in each script's wrapper registry.
        if (player) return Lookup::success({auth::Value{auth::Handle{script::foc::handle_player, *player}}});
        return Lookup::success({});
    }); !added) return Result::failure(added.error());
    std::map<std::uint64_t, std::string> difficulties;
    for (const auto& player : players.players) {
        const auto level = lower(player.difficulty);
        difficulties.emplace(player.id, level == "easy" ? "Easy" : level == "normal" ? "Normal" : level == "hard" ? "Hard" : player.difficulty);
    }
    if (auto added = scheduler.value().register_method(script::foc::handle_player, "Get_Difficulty",
        [difficulties = std::move(difficulties)](auth::BindingContext&, const auth::ValueList& arguments) {
            using DifficultyResult = core::Result<auth::ValueList>;
            const auto* self = arguments.empty() ? nullptr : std::get_if<auth::Handle>(&arguments.front().data);
            if (!self || self->kind != script::foc::handle_player || !difficulties.contains(self->id))
                return DifficultyResult::failure(problem("Get_Difficulty requires a registered player"));
            return DifficultyResult::success({auth::Value::text(difficulties.at(self->id))});
        }); !added) return Result::failure(added.error());
    if (auto added = auth::register_galactic_first_object(scheduler.value(), players.types, players.planets,
        std::move(reader)); !added) return Result::failure(added.error());
    std::set<std::uint64_t> advisor_players;
    for (const auto& player : players.players) advisor_players.insert(player.id);
    if (auto added = scheduler.value().register_method(script::foc::handle_player, "Enable_Advisor_Hints",
        [advisor_players = std::move(advisor_players)](auth::BindingContext& context, const auth::ValueList& arguments) {
            using HintsResult = core::Result<auth::ValueList>;
            const auto* self = arguments.empty() ? nullptr : std::get_if<auth::Handle>(&arguments.front().data);
            if (arguments.size() != 3 || !self || self->kind != script::foc::handle_player
                || !advisor_players.contains(self->id) || !std::get_if<std::string>(&arguments[1].data)
                || !std::get_if<bool>(&arguments[2].data))
                return HintsResult::failure(problem("Enable_Advisor_Hints requires a registered player, mode string and boolean"));
            // Retail changes a per-player advisor presentation preference and
            // returns nothing. The advisor presentation consumer is not hosted.
            context.report(auth::codes::unsupported_api,
                "Enable_Advisor_Hints presentation preference is unavailable; story script continues");
            return HintsResult::success({});
        }); !added) return Result::failure(added.error());
    if (auto added = scheduler.value().register_prelude(player_module); !added) return Result::failure(added.error());
    std::map<std::string, sim::galactic::PlanetId> planets;
    for (const auto& [name, id] : players.planets) {
        const auto key = lower(name);
        const auto found = planets.find(key);
        if (found == planets.end() || id < found->second) planets.insert_or_assign(key, id);
    }
    if (auto added = scheduler.value().register_binding("FindPlanet", [planets = std::move(planets)](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Lookup = core::Result<auth::ValueList>;
        if (arguments.empty() || !std::holds_alternative<std::string>(arguments.front().data)) {
            context.report(auth::codes::invalid_request, "FindPlanet requires a string first argument");
            context.request_exit();
            return Lookup::success({});
        }
        const auto found = planets.find(lower(std::get<std::string>(arguments.front().data)));
        if (found == planets.end()) return Lookup::success({});
        return Lookup::success({auth::Value{auth::Handle{script::foc::handle_game_object, found->second}}});
    }); !added) return Result::failure(added.error());
    // RGC-176..178: type lookup, creation and the AI hand-off. Creation and the free-store write run in the ordered
    // native shadow of the story phase (the same journaled creation as the AI's forced production), so identities,
    // streams and the replay journal are the session's own on every peer and worker count.
    std::map<std::string, sim::galactic::TypeId> type_names;
    for (const auto& [name, id] : players.types) {
        const auto key = lower(name);
        const auto found = type_names.find(key);
        if (found == type_names.end() || id < found->second) type_names.insert_or_assign(key, id);
    }
    if (auto added = scheduler.value().register_binding("Find_Object_Type", [type_names = std::move(type_names)](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Lookup = core::Result<auth::ValueList>;
        if (arguments.empty() || !std::holds_alternative<std::string>(arguments.front().data)) {
            context.report(auth::codes::invalid_request, "Find_Object_Type requires a string first argument");
            context.request_exit();
            return Lookup::success({});
        }
        const auto found = type_names.find(lower(std::get<std::string>(arguments.front().data)));
        if (found == type_names.end()) return Lookup::success({});
        return Lookup::success({auth::Value{auth::Handle{script::foc::handle_type, found->second}}});
    }); !added) return Result::failure(added.error());
    std::set<std::uint64_t> spawn_players, spawn_planets;
    for (const auto& player : players.players) spawn_players.insert(player.id);
    for (const auto& planet : players.planets) spawn_planets.insert(planet.second);
    if (auto added = scheduler.value().register_binding("Spawn_Unit", [spawn_players = std::move(spawn_players), spawn_planets = std::move(spawn_planets)](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Spawn = core::Result<auth::ValueList>;
        const auto refuse = [&](std::string message) {
            context.report(auth::codes::invalid_request, std::move(message));
            context.request_exit();
            return Spawn::success({});
        };
        // RGC-176: three arguments (extra ones are ignored), each checked in order; nothing is created on a refusal.
        if (arguments.size() < 3) return refuse("Spawn_Unit requires a type, a planet and a player");
        const auto* type = std::get_if<auth::Handle>(&arguments[0].data);
        if (!type || type->kind != script::foc::handle_type || !type->id) return refuse("Spawn_Unit: parameter 1 is not an object type");
        const auto* planet = std::get_if<auth::Handle>(&arguments[1].data);
        if (!planet || planet->kind != script::foc::handle_game_object || !spawn_planets.contains(planet->id))
            return refuse("Spawn_Unit: parameter 2 is not a planet");
        const auto* player = std::get_if<auth::Handle>(&arguments[2].data);
        if (!player || player->kind != script::foc::handle_player || !spawn_players.contains(player->id))
            return refuse("Spawn_Unit: parameter 3 is not a player");
        auto made = context.apply_immediate("galactic.spawn_unit", {arguments[2], arguments[1], arguments[0]});
        if (!made) return made;
        if (made.value().empty()) {
            // RGC-178: a null creation is a script warning and no table.
            context.report(auth::codes::script_warning, "Spawn_Unit: the type created no object");
            return Spawn::success({});
        }
        return made;
    }); !added) return Result::failure(added.error());
    if (auto added = scheduler.value().register_method(script::foc::handle_game_object, "Prevent_AI_Usage",
        [](auth::BindingContext& context, const auth::ValueList& arguments) {
            using Prevent = core::Result<auth::ValueList>;
            const auto* actor = arguments.size() == 2 ? std::get_if<auth::Handle>(&arguments[0].data) : nullptr;
            const auto* flag = arguments.size() == 2 ? std::get_if<bool>(&arguments[1].data) : nullptr;
            if (!actor || actor->kind != script::foc::handle_game_object || !flag)
                return Prevent::failure(problem("Prevent_AI_Usage requires an object and a Boolean"));
            auto applied = context.apply_immediate("galactic.story_prevent_ai_usage", arguments);
            if (!applied) return applied;
            const auto* done = !applied.value().empty() && applied.value().size() <= 2 ? std::get_if<bool>(&applied.value()[0].data) : nullptr;
            if (!done) return Prevent::failure(problem("Prevent_AI_Usage native result is invalid"));
            // L-57: no AI pool for the owner (a human seat) is a warning, and the script goes on. An object the AI
            // free store does not track is a different warning.
            if (!*done) {
                const auto* reason = applied.value().size() == 2 ? std::get_if<std::string>(&applied.value()[1].data) : nullptr;
                context.report(auth::codes::script_warning, reason && *reason == "actor"
                    ? "Prevent_AI_Usage: the object is not known to the AI free store"
                    : "Prevent_AI_Usage: the owner has no initialized AI pool");
            }
            return Prevent::success({});
        }); !added) return Result::failure(added.error());
    // WST-56: the sub-plot scripts edit their plots (Lock_List/Unlock_List set the reward parameter, then send the event).
    {
        std::vector<core::Diagnostic> errors;
        script::foc::detail::register_story_plot_bindings(scheduler.value(),
            script::foc::StoryPlotAccess([link] { return link->impl().plots.link.get(); }), errors);
        if (!errors.empty()) return Result::failure(errors.front());
    }
    // WST-37/55: Story_Event(name[, object]) stages a STORY_AI_NOTIFICATION broadcast. The debug build's
    // The debug build dispatches the notification to the sub-plots at the call (WST-55), so the engine commits it at the call's place in
    // the script's command order, after the Set_Reward_Parameter calls before it (their values are captured per trigger).
    {
        std::map<std::uint64_t, std::string> names;
        for (const auto& [name, id] : players.planets) names.try_emplace(id, upper_text(name));
        if (auto added = scheduler.value().register_binding("Story_Event", [names = std::move(names)](
                auth::BindingContext& context, const auth::ValueList& arguments) {
            using Sent = core::Result<auth::ValueList>;
            if (arguments.empty() || arguments.size() > 2 || !std::holds_alternative<std::string>(arguments[0].data))
                return Sent::failure(problem("Story_Event expects a name and an optional object"));
            std::string planet;
            bool has_object = false;
            if (arguments.size() == 2 && !std::holds_alternative<std::monostate>(arguments[1].data)) {
                const auto* object = std::get_if<auth::Handle>(&arguments[1].data);
                if (!object || object->kind != script::foc::handle_game_object)
                    return Sent::failure(problem("Story_Event: the second argument must be a game object"));
                has_object = true;
                if (const auto found = names.find(object->id); found != names.end()) planet = found->second;
            }
            context.issue_command(std::string(verb_story_event), {auth::Value::text(std::get<std::string>(arguments[0].data)),
                auth::Value::text(has_object ? "1" : "0"), auth::Value::text(planet)});
            return Sent::success({});
        }); !added) return Result::failure(added.error());
    }
    // Check_Story_Flag(player, name[, planet[, reset]]): whether a TRIGGER_AI reward set (player, planet, name); `reset`
    // clears it. The read sees the flags as of the end of the last story step; the clear is committed in command order
    // after the service, so a flag consumed by one script is gone for every later service.
    if (auto added = scheduler.value().register_binding("Check_Story_Flag", [link](
            auth::BindingContext& context, const auth::ValueList& arguments) {
        using Checked = core::Result<auth::ValueList>;
        if (arguments.size() < 2 || arguments.size() > 4 || !std::holds_alternative<std::string>(arguments[1].data))
            return Checked::failure(problem("Check_Story_Flag expects a player, a flag name, an optional planet and an optional reset"));
        const auto* player = std::get_if<auth::Handle>(&arguments[0].data);
        if (!player && !std::holds_alternative<std::monostate>(arguments[0].data))
            return Checked::failure(problem("Check_Story_Flag: the first argument must be a player"));
        std::uint64_t planet = 0;
        if (arguments.size() > 2 && !std::holds_alternative<std::monostate>(arguments[2].data)) {
            const auto* object = std::get_if<auth::Handle>(&arguments[2].data);
            if (!object || object->kind != script::foc::handle_game_object)
                return Checked::failure(problem("Check_Story_Flag: the planet must be a game object or nil"));
            planet = object->id;
        }
        bool reset = false;
        if (arguments.size() > 3 && !std::holds_alternative<std::monostate>(arguments[3].data)) {
            const auto* flag = std::get_if<bool>(&arguments[3].data);
            if (!flag) return Checked::failure(problem("Check_Story_Flag: reset must be a Boolean or nil"));
            reset = *flag;
        }
        // No such player (a faction that is not in this game): nothing can have set a flag for it.
        if (!player || player->kind != script::foc::handle_player) return Checked::success({auth::Value{false}});
        const auto& name = std::get<std::string>(arguments[1].data);
        const auto* sink = link->impl().sink.get();
        const bool set = sink && sink->flag(static_cast<story::Player>(player->id), planet, name);
        if (set && reset)
            context.issue_command(std::string(verb_story_flag_clear), {auth::Value::text(std::to_string(player->id)),
                auth::Value::text(std::to_string(planet)), auth::Value::text(name)});
        return Checked::success({auth::Value{set}});
    }); !added) return Result::failure(added.error());
    // Debug sinks (PGDebug, NEW-L22): the engine writes them to its logs; here they have no effect and write no file.
    for (const std::string_view sink : {"_OuputDebug", "_ScriptMessage", "_CustomScriptMessage", "_MessagePopup", "DumpCallStack"})
        if (auto added = scheduler.value().register_binding(sink, [](auth::BindingContext&, const auth::ValueList&) {
                return core::Result<auth::ValueList>::success(auth::ValueList{}); }); !added) return Result::failure(added.error());
    std::vector<std::string_view> implemented{"GlobalValue.Get", "GlobalValue.Set", "Find_First_Object", "Find_Object_Type", "Spawn_Unit", "FindPlanet", "Find_Player", "DiscreteDistribution.Create", "GameRandom", "_OuputDebug", "_ScriptMessage",
        "_CustomScriptMessage", "_MessagePopup", "DumpCallStack", "Get_Story_Plot", "Story_Event", "Check_Story_Flag"};
    // `Object`, `Target`, `Budget`, `FreeStore` and `PlayerObject` are variables of the scripts (Common_Base_Definitions
    // clears them), and the inventory's "Object.x" names are methods of the wrappers they hold; bound as global tables they
    // would be protected and fault every Base_Definitions. Calls on them fail as an index of nil until the wrappers exist.
    for (const std::string_view name : auth::missing_galactic_api_names()) {
        const std::string_view receiver = name.substr(0, name.find('.'));
        if (receiver != name && (receiver == "Object" || receiver == "Target" || receiver == "Budget" || receiver == "FreeStore" || receiver == "PlayerObject")) implemented.push_back(name);
    }
    if (auto missing = auth::register_missing_galactic_api(scheduler.value(), implemented); !missing) return Result::failure(missing.error());
    return scheduler;
}

core::Result<void> start_galactic_story_scripts(auth::ScriptScheduler& scripts, const GalacticStoryScripts& content,
    std::vector<std::string>& notes) {
    for (const auto& entry : content.entries()) {
        if (entry.module.empty()) continue;
        if (auto created = scripts.create_instance(entry.instance, entry.module); !created) {
            // A chunk that fails to load leaves no instance (create_instance): the campaign goes on without it.
            notes.push_back(entry.subplot + ": Lua_Script " + entry.script + " did not start: " + created.error().code + " " + created.error().message);
            continue;
        }
        const std::uint64_t tick = scripts.completed_tick() + 1;
        // The initialization events the engine gives a fresh script before its first pump (L-20, L-27, WST-53).
        for (std::uint64_t step = 0; step < 2; ++step) {
            auth::ScriptEvent event;
            event.key = {tick, auth::first_simulation_producer, entry.instance, step};
            event.target = entry.instance;
            event.kind = step == 0 ? auth::ScriptEvent::Kind::call : auth::ScriptEvent::Kind::start_thread;
            event.name = step == 0 ? "Base_Definitions" : "main";
            if (auto posted = scripts.submit_event(std::move(event)); !posted) return posted;
        }
    }
    return core::Result<void>::success();
}

core::Result<void> forward_galactic_story_calls(auth::ScriptScheduler& scripts, const GalacticStoryScripts& content,
    std::span<const story::SubPlotDef> definitions, std::span<const story::ScriptCall> calls, std::uint64_t& sequence) {
    if (calls.empty()) return core::Result<void>::success();
    const auto running = scripts.instances();
    for (const auto& call : calls) {
        if (call.subplot >= definitions.size()) return core::Result<void>::failure(problem("story call names a sub-plot outside the plot table"));
        const auto* entry = content.find(definitions[call.subplot].name);
        if (entry == nullptr || !std::binary_search(running.begin(), running.end(), entry->instance)) continue;
        auth::ScriptEvent event;
        event.key = {scripts.completed_tick() + 1, auth::galactic_story_call_producer, entry->instance, ++sequence};
        event.target = entry->instance;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Story_Event_Trigger";
        event.arguments.push_back(auth::Value::text(call.event));
        if (auto posted = scripts.submit_event(std::move(event)); !posted) return posted;
    }
    return core::Result<void>::success();
}

} // namespace eawr::campaign
