#include "eawr/campaign/galactic_story_scripts.hpp"

#include "galactic_story_random.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/authoritative/galactic_bridge.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

#include <algorithm>
#include <array>
#include <cstring>
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

} // namespace

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
    bool multiplayer, const GalacticStoryPlayers& players) {
    using Result = core::Result<auth::ScriptScheduler>;
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
    // RGC-190..199: the weighted list and the two random calls. Everything else the stock and mod story starts need
    // that this host does not implement faults by name below.
    if (auto added = story_random::register_story_random(scheduler.value(), multiplayer); !added) return Result::failure(added.error());
    // WST-91/92: prepare the full faction database once; the first matching player is the lowest player-table ID.
    std::map<std::string, std::optional<sim::galactic::PlayerId>> factions;
    for (const auto& name : players.factions) factions.try_emplace(lower(name), std::nullopt);
    bool local_registered = false;
    for (const auto& player : players.players) {
        local_registered |= players.local_player && player.id == *players.local_player;
        const auto found = factions.find(lower(player.faction));
        if (found != factions.end() && (!found->second || player.id < *found->second)) found->second = player.id;
    }
    // Project deviation from WST-91: a per-peer local player cannot enter the multiplayer lockstep simulation.
    const auto local = !multiplayer && local_registered ? players.local_player : std::nullopt;
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
    if (auto added = scheduler.value().register_prelude(player_module); !added) return Result::failure(added.error());
    // Debug sinks (PGDebug, NEW-L22): the engine writes them to its logs; here they have no effect and write no file.
    for (const std::string_view sink : {"_OuputDebug", "_ScriptMessage", "_CustomScriptMessage", "_MessagePopup", "DumpCallStack"})
        if (auto added = scheduler.value().register_binding(sink, [](auth::BindingContext&, const auth::ValueList&) {
                return core::Result<auth::ValueList>::success(auth::ValueList{}); }); !added) return Result::failure(added.error());
    std::vector<std::string_view> implemented{"Find_Player", "DiscreteDistribution.Create", "GameRandom", "_OuputDebug", "_ScriptMessage",
        "_CustomScriptMessage", "_MessagePopup", "DumpCallStack"};
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
