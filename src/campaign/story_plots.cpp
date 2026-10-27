#include "eawr/campaign/story_plots.hpp"

#include "eawr/core/text.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <utility>

namespace eawr::campaign {
namespace {
namespace story = sim::galactic::story;

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
bool same(std::string_view a, std::string_view b) { return lower(a) == lower(b); }
std::string trimmed(std::string_view text) { return std::string(core::trim_view(text)); }

// WST-05: a token list splits on space, comma, tab and line breaks; TBD is ignored like an empty value.
void append_tokens(std::string_view text, std::vector<std::string>& out) {
    if (data::retail::is_placeholder(text)) return;
    std::string token;
    const auto flush = [&] { if (!token.empty()) out.push_back(std::move(token)); token.clear(); };
    for (const char c : text) {
        if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') flush();
        else token.push_back(c);
    }
    flush();
}

// `Prefix` + number suffix (Event_Param3 -> 3), case-insensitive; 0 when the tag is not that family.
std::size_t numbered(std::string_view tag, std::string_view prefix) {
    if (tag.size() <= prefix.size() || !same(tag.substr(0, prefix.size()), prefix)) return 0;
    std::size_t value{};
    for (const char c : tag.substr(prefix.size())) {
        if (c < '0' || c > '9') return 0;
        value = value * 10 + static_cast<std::size_t>(c - '0');
        if (value > 99) return 0;
    }
    return value;
}

std::int64_t whole(std::string_view text) {
    const auto token = trimmed(text);
    std::int64_t value{};
    bool negative{};
    std::size_t i = 0;
    if (i < token.size() && (token[i] == '-' || token[i] == '+')) negative = token[i++] == '-';
    for (; i < token.size() && token[i] >= '0' && token[i] <= '9'; ++i) value = value * 10 + (token[i] - '0');
    return negative ? -value : value;
}

// WST-04/05/06. Returns false when the event is dropped (unknown or missing event type).
bool parse_event(const data::XmlNode& node, story::EventDef& event, std::vector<std::string>& notes, const std::string& file) {
    std::string name;
    for (const auto& attribute : node.attributes) if (same(attribute.name, "Name")) {
        name = trimmed(attribute.value);
        data::tag_trace::used_attribute(node, attribute.name);
    }
    if (name.size() > 255) name.resize(255);
    event.name = story::upper(name);
    event.authored_name = name;
    std::string incoming;
    for (const auto& child : node.children) {
        const auto& tag = child.name;
        const auto& text = child.raw_text;
        data::tag_trace::used(child);
        if (const auto n = numbered(tag, "Event_Param"); n >= 1 && n <= story::token_slots_event) { append_tokens(text, event.params[n - 1]); continue; }
        if (const auto n = numbered(tag, "Reward_Param"); n >= 1 && n <= story::token_slots_reward) { append_tokens(text, event.reward_params[n - 1]); continue; }
        if (same(tag, "Reward_Param_List")) { append_tokens(text, event.reward_list); continue; }
        if (same(tag, "Prereq")) { // one group: the names inside are an AND (WST-06)
            std::vector<std::string> names;
            append_tokens(text, names);
            for (auto& each : names) each = story::upper(each);
            if (!names.empty()) event.prereqs.push_back(std::move(names));
            continue;
        }
        if (data::retail::is_placeholder(text)) continue; // TBD is ignored as if empty
        if (same(tag, "Event_Type")) event.type = trimmed(text);
        else if (same(tag, "Reward_Type")) event.reward_type = trimmed(text);
        else if (same(tag, "Branch")) event.branch = story::upper(trimmed(text));
        else if (same(tag, "Perpetual")) event.perpetual = data::retail::boolean(text).value_or(false);
        else if (same(tag, "Multiplayer")) event.multiplayer = data::retail::boolean(text).value_or(false);
        else if (same(tag, "Inactive_Delay")) { if (const auto n = data::retail::leading_number(trimmed(text))) event.inactive_delay = n->value; }
        else if (same(tag, "Timeout")) { if (const auto n = data::retail::leading_number(trimmed(text))) event.timeout = n->value; }
        else if (same(tag, "Story_Dialog")) event.dialog = trimmed(text);
        else if (same(tag, "Story_Chapter")) event.chapter = whole(text);
        else if (same(tag, "Story_Var")) event.variable = trimmed(text);
        else if (same(tag, "Story_Tag")) event.tag = trimmed(text);
        else if (same(tag, "Story_Incoming")) incoming = trimmed(text);
        else if (same(tag, "Story_Dialog_Popup")) event.popup = data::retail::boolean(text).value_or(true);
        else if (same(tag, "Story_Dialog_SFX")) event.sound = data::retail::boolean(text).value_or(true);
        // Reward_Position, Event_Filter and the speech-done timeout belong to later parts (WST-05).
    }
    if (!incoming.empty()) event.tag = incoming; // WST-70: a nonempty Story_Incoming overrides Story_Tag
    // WST-18: Timeout is parsed and stored, never serviced.
    if (event.name.empty() || !story::known_event_type(event.type)) {
        notes.push_back(file + ": event " + (event.name.empty() ? std::string("<unnamed>") : event.name)
            + " dropped, unknown event type '" + event.type + "' (WST-04)");
        return false;
    }
    return true;
}

core::Result<void> load_subplot(const vfs::Vfs& files, story::Player owner, std::string_view file, bool active,
    std::string script, StoryPlots& out) {
    const std::string path = "Data/XML/" + std::string(file);
    auto document = data::load_document(files, path);
    if (!document) { // WST-02: a listed sub-plot that cannot be opened asserts and is skipped
        out.notes.push_back(std::string(file) + ": sub-plot file not loaded (" + document.error().message + ")");
        return core::Result<void>::success();
    }
    story::SubPlotDef sub;
    sub.name = std::string(file); sub.owner = owner; sub.active = active; sub.script = std::move(script);
    std::map<std::string, std::size_t> by_name;
    for (const auto& node : document.value().root.children) {
        if (!same(node.name, "Event")) continue;
        story::EventDef event;
        if (!parse_event(node, event, out.notes, sub.name)) continue;
        // A duplicate name asserts and the later definition replaces the earlier one (in its slot).
        if (const auto at = by_name.find(event.name); at != by_name.end()) {
            out.notes.push_back(sub.name + ": duplicate event " + event.name + ", the later definition replaces the earlier (WST-04)");
            sub.events[at->second] = std::move(event);
            continue;
        }
        by_name.emplace(event.name, sub.events.size());
        sub.events.push_back(std::move(event));
    }
    // WST-89: a later load of the same authored sub-plot name replaces its owner and definition.
    const auto existing = std::find_if(out.subplots.begin(), out.subplots.end(), [&](const auto& row) { return row.name == sub.name; });
    if (existing == out.subplots.end()) out.subplots.push_back(std::move(sub));
    else *existing = std::move(sub);
    return core::Result<void>::success();
}
} // namespace

core::Result<void> load_story_plot(const vfs::Vfs& files, const story::Player owner, const std::string_view plot_file, StoryPlots& out) {
    const auto name = trimmed(plot_file);
    if (name.empty()) return core::Result<void>::success(); // WST-01: an empty name loads nothing and succeeds
    auto document = data::load_document(files, "Data/XML/" + name);
    if (!document) { // a missing plot file logs an error and loads nothing
        out.notes.push_back(name + ": plot file not loaded (" + document.error().message + ")");
        return core::Result<void>::success();
    }
    out.plots.push_back({owner, name});
    std::string script; // WST-02: held for the NEXT sub-plot entry, cleared after it even when that entry is empty
    for (const auto& node : document.value().root.children) {
        data::tag_trace::used(node);
        const bool active = same(node.name, "Active_Plot");
        if (same(node.name, "Lua_Script")) { script = trimmed(node.raw_text); continue; }
        if (!active && !same(node.name, "Suspended_Plot")) {
            out.notes.push_back(name + ": unexpected key '" + node.name + "' (WST-02)");
            continue;
        }
        auto held = std::exchange(script, {});
        const auto file = trimmed(node.raw_text);
        if (file.empty()) continue;
        if (auto loaded = load_subplot(files, owner, file, active, std::move(held), out); !loaded) return loaded;
    }
    return core::Result<void>::success();
}

std::vector<std::string> story_unit_names(const StoryPlots& plots) {
    std::vector<std::string> names;
    for (const auto& subplot : plots.subplots) for (const auto& event : subplot.events) {
        if ((event.reward_type != "SPAWN_HERO" && event.reward_type != "UNIQUE_UNIT") || event.reward_params[0].empty()) continue;
        const auto& name = event.reward_params[0].front();
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

core::Result<StoryPlots> load_story_plots(const vfs::Vfs& files, const data::galactic::GalacticData& data, const CampaignSetupPlan& plan) {
    StoryPlots out;
    const auto* campaign = data.campaign(plan.campaign_name());
    if (!campaign) return core::Result<StoryPlots>::success(std::move(out));
    const auto load = [&](const CampaignPlayerBinding& binding, const std::string& plot) {
        const auto first_note = out.notes.size();
        auto result = load_story_plot(files, binding.player.player_id, plot, out);
        // WST-88: retain the nonfatal loader result with the complete authored route.
        for (auto i = first_note; i < out.notes.size(); ++i)
            out.notes[i] = campaign->name + ": faction " + binding.faction + ", Data/XML/" + plot + ": " + out.notes[i];
        return result;
    };
    // WGS-07: Rebel, Empire, then Underworld plot, each for its (active) player.
    struct Route { std::string_view side_constant; const std::string* plot; };
    const std::array routes{Route{"Good_Side_Name", &campaign->rebel_story_name}, Route{"Evil_Side_Name", &campaign->empire_story_name},
        Route{"Corrupt_Side_Name", &campaign->underworld_story_name}};
    for (const auto& route : routes) {
        const auto* side = data.constant(route.side_constant);
        if (!side || trimmed(side->text).empty()) {
            out.notes.push_back(std::string(route.side_constant) + " missing: no plot routed (WGS-07)");
            continue;
        }
        const auto binding = std::find_if(plan.player_bindings().begin(), plan.player_bindings().end(), [&](const auto& row) {
            return row.active && same(row.faction, trimmed(side->text));
        });
        if (binding == plan.player_bindings().end()) continue; // only existing active players load their plot
        if (auto loaded = load(*binding, *route.plot); !loaded)
            return core::Result<StoryPlots>::failure(loaded.error());
    }
    // WST-87: generic associations add plots after the named routes, including repeated factions.
    for (const auto& route : campaign->story_names) {
        const auto binding = std::find_if(plan.player_bindings().begin(), plan.player_bindings().end(), [&](const auto& row) {
            return row.active && same(row.faction, route.faction);
        });
        if (binding == plan.player_bindings().end()) {
            out.notes.push_back(campaign->name + ": faction " + route.faction + ", Data/XML/" + route.name
                + ": no active player; remaining Story_Name associations not loaded (WST-88)");
            break;
        }
        if (auto loaded = load(*binding, route.name); !loaded)
            return core::Result<StoryPlots>::failure(loaded.error());
    }
    return core::Result<StoryPlots>::success(std::move(out));
}

} // namespace eawr::campaign
