#include "eawr/sim/galactic/story_adapters.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace st = eawr::sim::galactic::story;
int failures{};
void expect(bool pass, const char* message) { if (!pass) { ++failures; std::cerr << "FAIL: " << message << '\n'; } }
st::FixtureState fixture() {
    st::FixtureState s;
    s.players = {{1, "Rebel", false, true, 2, 2, 100, {}}, {2, "Empire", true, false, 1, 1, 0, {}}};
    s.types = {{"Hero", "Rebel", 1, true}, {"Ship", "Rebel", 3, true}, {"Enemy", "Empire", 1, true}};
    st::Planet a; a.name = "Home"; a.owner = 1;
    st::Planet b; b.name = "Safe"; b.owner = 1; b.x = 10;
    st::Planet c; c.name = "Foreign"; c.owner = 2; c.x = 11;
    s.planets = {a, b, c};
    return s;
}
void feed_contracts() {
    st::FakeFeed feed;
    std::vector<st::Recipient> recipients{{"one", 1, true}, {"two", 2, true}, {"suspended", 1, false}};
    st::DispatchGate gate; gate.active_mode = false;
    expect(feed.submit({1, {}, st::EconomyNotice{st::Economy::tech_level, 2, {}, {}, st::BaseKind::either, 1}}, gate, recipients).empty(), "WST-13 inactive parent queues tech");
    std::string speech = "voice";
    expect(feed.submit({2, 1, st::TextNotice{st::Text::speech_done, speech, {}}}, gate, recipients).empty(), "WST-13 queued speech");
    speech.clear();
    st::FakeFeed restored(feed.state()); // Fixture checkpoint boundary; live checkpoint wiring is separate.
    expect(restored.submit({2, 1, st::TextNotice{st::Text::generic, "duplicate", {}}}, gate, recipients).empty(), "duplicate submission refused after restore");
    gate.active_mode = true; gate.single_step = true;
    expect(restored.service(gate, recipients).empty() && restored.state().pending.size() == 2, "WST-13 single step keeps queue");
    gate.single_step = false;
    const auto deliveries = restored.service(gate, recipients);
    expect(deliveries.size() == 3, "WST-14 unnamed broadcast, owner filter and subplot activity");
    expect(deliveries[0].notification.sequence == 1 && deliveries[1].notification.sequence == 1 && deliveries[2].notification.sequence == 2, "WST-13 same-frame arrival order");
    expect(std::get<st::TextNotice>(deliveries[2].notification.payload).text == "voice", "WST-13 speech copied");
    expect(restored.service(gate, recipients).empty(), "continue cannot duplicate callback");
    gate.foreground_state = false;
    expect(restored.submit({3, {}, st::MovieDoneNotice{}}, gate, recipients).empty() && restored.state().pending.empty(), "WST-13 absent application foreground state drops");
    gate.foreground_state = true; gate.foreground = false;
    expect(restored.submit({4, {}, st::MovieDoneNotice{}}, gate, recipients).empty(), "WST-13 background queues");
    gate.replay = true;
    expect(restored.service(gate, recipients).size() == 2, "WST-13 replay opens delivery gate");
    gate.attract_demo = true;
    expect(restored.submit({5, {}, st::MovieDoneNotice{}}, gate, recipients).empty() && restored.state().pending.empty(), "WST-13 attract demo drops");
    std::vector<std::string> planets{"Home"}, texts{"Ready"};
    expect(st::matches_ai({st::Text::ai_notification, "rEaDy", "hOmE"}, planets, texts), "WST-37 case insensitive matching planet");
    expect(!st::matches_ai({st::Text::ai_notification, "Ready", {}}, planets, texts), "WST-37 absence does not match planet event");
    expect(!st::matches_ai({st::Text::ai_notification, "Ready", "Home"}, {}, texts), "WST-37 planet does not match planetless event");
    expect(st::matches_ai({st::Text::ai_notification, "Ready", {}}, {}, texts), "WST-37 planetless match");
    const auto losses = st::destruction("Variant", "Original", "Home", 1);
    expect(losses.size() == 2 && losses[0].type == "Variant" && losses[1].type == "Original", "WST-34 original type second");
    expect(st::destruction("Type", "tYpE", "Home", 1).size() == 1, "WST-34 same resolved type once");
    expect(st::event_name(st::ConquestNotice{"Home", 1, 2}) == "STORY_CONQUER", "WST-32 ownership payload");
    expect(st::event_name(st::ConquestCountNotice{"Home", 12}) == "STORY_CONQUER_COUNT", "WGM-192 separate count payload");
    st::FakeFeed tactical_feed; gate = {}; gate.active_mode = false;
    expect(tactical_feed.submit({1, {}, st::TacticalNotice{st::Tactical::destroy, "Hero", "Home", {}, 1}}, gate, recipients).empty()
        && tactical_feed.state().pending.empty(), "WST-13 tactical destruction dropped while parent inactive");
    expect(!st::notification_seam(st::TacticalNotice{}).empty(), "tactical consumers retain named seam");
    expect(st::event_name(st::EconomyNotice{st::Economy::credits, 100, {}, {}, st::BaseKind::either, 1}) == "STORY_ACCUMULATE", "WST-33 credit balance payload");
    bool rejected{};
    try { static_cast<void>(st::FakeFeed({1, {{2, {}, st::MovieDoneNotice{}}}})); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "corrupt feed snapshot diagnosed");
}
void admission_contracts() {
    st::AdmissionEvent event; event.owner = 1; event.planets = {"Home"}; event.required_types = {"Hero"};
    event.orbit_types = {"Guard"}; event.filter = st::OwnerFilter::friendly; event.link_tactical = true;
    std::vector<st::AdmissionEvent> events{event};
    st::TravelNotice travel; travel.planet = "home"; travel.fleet.owner = 1; travel.fleet.types = {"Hero"}; travel.fleet.orbit_types = {"Guard"};
    std::vector<st::OwnerRelation> relations{{1, true}, {2, false}};
    const auto query = [&] { return st::admission(events, travel, relations, st::BaseKind::space); };
    expect(query().restricted_entry && query().special_space_map && !query().special_land_map, "WST-52 matching active enter restriction and space map");
    travel.fleet.stealth = true; expect(!query().restricted_entry, "WST-31 stealth gate");
    events[0].allow_stealth = true; events[0].raid = true;
    expect(!query().restricted_entry, "WST-31 raid capable gate");
    travel.fleet.raid_capable = true; events[0].exclusive = true; travel.fleet.types.push_back("Extra");
    expect(!query().restricted_entry, "WST-31 exclusive fleet contents");
    travel.fleet.types.pop_back(); expect(query().restricted_entry, "WST-31 all fleet predicates pass");
    travel.fleet.orbit_types.clear(); expect(!query().restricted_entry, "WST-31 orbit type condition");
    events[0].orbit_types.clear(); events[0].subplot_active = false; expect(!query().restricted_entry, "WST-52 suspended subplot excluded");
    events[0].subplot_active = true; events[0].triggered = true; expect(!query().restricted_entry, "WST-14 triggered excluded");
    events[0].triggered = false; events[0].disabled = true; expect(!query().restricted_entry, "WST-14 disabled excluded");
    events[0] = event; events[0].kind = st::Travel::move_hero; events[0].heroes = {"Hero"}; travel.fleet.heroes = {"Hero"};
    expect(query().special_space_map && !query().restricted_entry, "WST-52 hero in transport space map");
    events[0].kind = st::Travel::deploy_hero;
    expect(st::admission(events, travel, relations, st::BaseKind::ground).special_land_map, "WST-52 deployed hero land map");
    events[0].load_tactical_map = true; events[0].map_kind = st::BaseKind::space;
    expect(!st::admission(events, travel, relations, st::BaseKind::ground).special_land_map, "WST-35 map kind gate");
    events[0].map_kind = st::BaseKind::ground;
    expect(st::admission(events, travel, relations, st::BaseKind::ground).special_land_map, "WST-52 load tactical map contents");
    events[0] = event; events[0].owner = 2;
    expect(!query().restricted_entry, "each subplot owner uses its own relation");
}
void rewards() {
    auto s = fixture();
    auto result = st::apply(s, 1, st::Spawn{"Hero", "Home", 2});
    expect(result.status == st::RewardStatus::applied && s.units.size() == 2 && s.units[0].planet == "Home", "WST-40 authored count and named planet");
    expect(result.effects.size() == 8 && result.effects[1].kind == st::EffectKind::human_flash && result.effects[1].duration_milliseconds == 1500, "WST-40 ordered human effects and radar duration");
    s.planets[0].orbit_owners = {2};
    expect(st::apply(s, 1, st::Spawn{"Hero", "Home"}).status == st::RewardStatus::applied && s.units.back().planet == "Safe", "WST-40 occupied orbit safe fallback");
    s = fixture();
    expect(st::apply(s, 1, st::Spawn{"Hero", "Absent"}).status == st::RewardStatus::applied && s.units.back().planet == "Home", "WST-40 absent named planet owned fallback");
    s = fixture(); s.units.push_back({50, "Hero", "Safe", 1, true, false});
    expect(st::apply(s, 1, st::Spawn{"Hero", "Absent"}).status == st::RewardStatus::applied && s.units.back().planet == "Safe", "WST-40 first owned object anchors fallback");
    s = fixture();
    expect(st::apply(s, 1, st::Spawn{"Hero", "Foreign"}).status == st::RewardStatus::applied && s.units.back().planet == "Safe", "WST-40 nearest owned planet");
    s = fixture(); result = st::apply(s, 2, st::Spawn{"Enemy", "Foreign"});
    expect(s.units.back().free_store && result.effects.size() == 2 && result.effects[1].kind == st::EffectKind::free_store, "WST-40 AI free store handoff");
    s = fixture(); s.planets[0].orbit_owners = {2}; s.planets[1].orbit_owners = {2};
    expect(st::apply(s, 1, st::Spawn{"Hero", "Home"}).status == st::RewardStatus::invalid && s.units.empty() && s.next_object == 1, "failed placement leaves state intact");
    s = fixture(); s.units.push_back({2, "Hero", "Home", 1, true, false});
    expect(st::apply(s, 1, st::Spawn{"Hero", "Home", 2}).status == st::RewardStatus::invalid && s.units.size() == 1 && s.next_object == 1, "partial spawn collision rolls back");
    s = fixture();
    expect(st::apply(s, 1, st::ProductionReward{st::ProductionSwitch::lock, {"Hero"}, false}).status == st::RewardStatus::applied, "WST-40 lock type");
    static_cast<void>(st::apply(s, 1, st::ProductionReward{st::ProductionSwitch::disable, {"Hero"}, false}));
    static_cast<void>(st::apply(s, 1, st::ProductionReward{st::ProductionSwitch::buildable, {"Hero", "Ship", "Enemy"}, false}));
    expect(!s.players[0].production[0].locked && s.players[0].production[0].disabled && s.players[0].production.size() == 1, "WST-40 unlock retains disabled, faction and tech gates");
    static_cast<void>(st::apply(s, 1, st::ProductionReward{st::ProductionSwitch::enable, {"Hero"}, false}));
    expect(!s.players[0].production[0].disabled, "WST-40 enable clears disabled only");
    result = st::apply(s, 1, st::CreditsReward{5});
    expect(s.players[0].credits == 105 && std::get<st::EconomyNotice>(*result.effects[0].feedback).value == 105, "WST-41 credits feedback whole balance");
    expect(st::apply(s, 1, st::CreditsReward{std::numeric_limits<std::int64_t>::max()}).status == st::RewardStatus::invalid && s.players[0].credits == 105, "credit overflow transactional");
    result = st::apply(s, 1, st::TechReward{"Empire", 4});
    expect(s.players[1].tech == 4 && s.players[1].max_tech == 4 && std::get<st::EconomyNotice>(*result.effects[0].feedback).object_owner == 2, "WST-41 technology targets authored faction");
    for (auto restriction : {st::Restriction::entry, st::Restriction::autoresolve, st::Restriction::corruption, st::Restriction::sabotage, st::Restriction::black_market, st::Restriction::abilities, st::Restriction::reveal}) {
        expect(st::apply(s, 1, st::RestrictionReward{restriction, "Home", true}).status == st::RewardStatus::applied, "WST-42 restriction switch");
    }
    expect(s.planets[0].restricted && s.planets[0].restrict_autoresolve && s.planets[0].restrict_corruption && s.planets[0].restrict_sabotage && s.planets[0].restrict_black_market && s.planets[0].restrict_abilities && s.planets[0].revealed, "WST-42 independent restriction fields");
    expect(st::apply(s, 1, st::DeferredReward{"ENABLE_AUTORESOLVE", {}}).status == st::RewardStatus::inert && s.planets[0].restrict_autoresolve, "WST-42 inert autoresolve keyword");
    expect(st::apply(s, 1, st::DeferredReward{"SET_PLANET_VISIBILITY_LEVEL", {"raw"}}).status == st::RewardStatus::unverified, "WST-U09 unverified layout explicit");
    static_cast<void>(st::apply(s, 1, st::VictoryWaitReward{false}));
    static_cast<void>(st::apply(s, 1, st::VictoryWaitReward{false}));
    static_cast<void>(st::apply(s, 1, st::VictoryWaitReward{true}));
    expect(s.victory.waits == 1, "WST-45 victory wait decrement");
    expect(st::apply(s, 1, st::VictoryWaitReward{false, false}).status == st::RewardStatus::inert
        && s.victory.waits == 1, "WGS-174 absent parameter leaves nested waits unchanged");
    result = st::apply(s, 1, st::VictoryReward{"Empire", false});
    expect(s.victory.waits == 0 && s.victory.winner == 2U && s.victory.delay_seconds == 7 && s.victory.test_requested && result.effects[0].duration_milliseconds == 7000, "WST-45 forced victory clears waits and tests now");
    static_cast<void>(st::apply(s, 1, st::VictoryReward{"Rebel", true}));
    expect(s.victory.delay_seconds == 1 && s.victory.suppress_text, "WST-45 suppressed text one second");
    static_cast<void>(st::apply(s, 1, st::VictoryWaitReward{true}));
    expect(s.victory.waits == 0, "WST-45 wait cannot underflow");
}

std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* value = nullptr; std::size_t size{};
    if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") != 0 || !value) return {};
    std::string root(value); std::free(value); return root.empty() ? std::nullopt : std::optional<std::string>(root);
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}
std::string field(const eawr::data::XmlNode& event, std::string_view key) {
    const auto it = std::find_if(event.children.begin(), event.children.end(), [&](const auto& n) { return n.name == key; });
    if (it == event.children.end()) return {};
    const auto first = it->raw_text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return it->raw_text.substr(first, it->raw_text.find_last_not_of(" \t\r\n") - first + 1);
}
std::vector<std::string> tokens(std::string text) {
    std::istringstream input(std::move(text)); std::vector<std::string> out;
    for (std::string token; input >> token;) out.push_back(std::move(token));
    return out;
}
void installed() {
    const auto root = game_root();
    if (!root) { std::cout << "SKIPPED: stock story adapter needs EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mount = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        if (!mount) throw std::runtime_error(mount.error().message);
        mounts.push_back(std::move(mount).value().mount);
    }
    const auto fs = eawr::vfs::Vfs::mount(mounts).value();
    const auto doc = eawr::data::load_document(fs, "Data/XML/Story_Sandbox_60_Rebel.xml").value();
    struct Row { std::string name, type; const eawr::data::XmlNode* xml{}; std::vector<std::vector<std::string>> prereqs; bool triggered{}; };
    std::vector<Row> rows;
    auto s = fixture(); s.types.clear();
    for (const auto& node : doc.root.children) {
        if (node.name != "Event") continue;
        Row row; row.xml = &node; row.type = field(node, "Event_Type");
        for (const auto& a : node.attributes) if (a.name == "Name") row.name = a.value;
        for (const auto& child : node.children) if (child.name == "Prereq") row.prereqs.push_back(tokens(child.raw_text));
        const auto reward = field(node, "Reward_Type");
        if (reward == "SPAWN_HERO" || reward == "UNIQUE_UNIT") s.types.push_back({field(node, "Reward_Param1"), "Rebel", 0});
        rows.push_back(std::move(row));
    }
    expect(!rows.empty(), "stock plot has event rows");
    // Test-only graph driver, using document order as fixture policy (NOT WST-U01 parity).
    // Only the start/tech chains of this stock GC plot are driven; production engine integration is separate.
    st::FakeFeed feed; const std::vector<st::Recipient> recipient{{"stock", 1, true}};
    std::uint64_t sequence{}; std::size_t callbacks{}, rewarded{}, unverified_layouts{};
    const auto drive = [&](const st::Payload& notification) {
        const auto deliveries = feed.submit({++sequence, 1, notification}, {}, recipient);
        expect(deliveries.size() == 1, "stock notification resolves adapter");
        bool progress = true;
        while (progress) {
            progress = false;
            for (auto& row : rows) {
                if (row.triggered) continue;
                const bool active = row.prereqs.empty() || std::any_of(row.prereqs.begin(), row.prereqs.end(), [&](const auto& group) {
                    return !group.empty() && std::all_of(group.begin(), group.end(), [&](const auto& name) {
                        return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.name == name && r.triggered; });
                    });
                });
                if (!active) continue;
                bool match = row.type == "STORY_TRIGGER";
                if (row.type == st::event_name(notification)) {
                    if (const auto* time = std::get_if<st::ElapsedNotice>(&notification)) match = time->time_seconds >= eawr::sim::math::Fixed::from_decimal(field(*row.xml, "Event_Param1")).value();
                    if (const auto* tech = std::get_if<st::EconomyNotice>(&notification)) match = tech->value >= std::stoll(field(*row.xml, "Event_Param1"));
                }
                if (!match) continue;
                ++callbacks; row.triggered = true; progress = true; // WST-20 callback before reward.
                const auto reward_name = field(*row.xml, "Reward_Type");
                std::optional<st::Reward> reward;
                if (reward_name == "SPAWN_HERO" || reward_name == "UNIQUE_UNIT") {
                    const auto count = field(*row.xml, "Reward_Param3");
                    reward = st::Spawn{field(*row.xml, "Reward_Param1"), field(*row.xml, "Reward_Param2"), count.empty() ? 1 : std::stoll(count)};
                } else if (reward_name == "BUILDABLE_UNIT") {
                    // WST-U09: this stock layout authors a type name at position 2,
                    // not WST-40's summarised numeric lock flag. Preserve it verbatim;
                    // do not infer a positional parser from the data alone.
                    const auto seam = st::apply(s, 1, st::DeferredReward{reward_name,
                        {field(*row.xml, "Reward_Param1"), field(*row.xml, "Reward_Param2")}});
                    expect(seam.status == st::RewardStatus::unverified, "stock buildable layout retains WST-U09 seam");
                    ++unverified_layouts;
                }
                if (reward) { expect(st::apply(s, 1, *reward).status == st::RewardStatus::applied, "stock authored reward resolves sink"); ++rewarded; }
            }
        }
    };
    drive(st::ElapsedNotice{eawr::sim::math::Fixed::from_integer(1000).value(), false});
    const auto first_units = s.units.size();
    expect(first_units > 0, "stock first elapsed chain spawns heroes on absent named planets");
    s.players[0].tech = 5;
    drive(st::EconomyNotice{st::Economy::tech_level, 5, {}, {}, st::BaseKind::either, 1});
    expect(s.units.size() > first_units && rewarded > first_units && callbacks >= rewarded, "stock tech notifications reach additional hero rewards");
    expect(unverified_layouts == 1, "stock unsupported buildable layout identified once");
    expect(std::all_of(s.units.begin(), s.units.end(), [](const auto& u) { return u.owner == 1 && u.planet == "Home"; }), "stock fixture fallback retains reward ownership");
    std::cout << "stock story adapter: notifications=" << sequence << " callbacks=" << callbacks << " rewards=" << rewarded << " units=" << s.units.size() << " unverified_layouts=" << unverified_layouts << '\n';
}
} // namespace
int main(int argc, const char* const[]) {
    try { if (argc > 1) installed(); else { feed_contracts(); admission_contracts(); rewards(); } }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return failures == 0 ? 0 : 1;
}
