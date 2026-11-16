#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../support/temp_dir.hpp"
#include "galactic_support.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/story_rewards_live.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/units/unit_tables_galactic.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// The PLANET_FACTION story reward (WST-42) on the installed Equal Footing campaign. The test replaces the Underworld
// plot with three events that all fire one second in, discovering their planet and faction names from the campaign
// itself so no name is special: one hands a planet another faction owns to a different faction, one names a planet the
// galaxy does not have, one leaves the planet empty. The first must flip the owner (and only that planet), the other two
// must be refused with their reason, the result must be equal on 1/2/4/8 workers, and the native journal alone must
// reproduce it.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using eawr::testing::must;
using galactic_test::expect;

constexpr std::string_view campaign_name = "Sandbox_Equal_Footing_Underworld";
constexpr std::string_view plot_path = "data/xml/story_sandbox_56_underworld.xml";

std::vector<eawr::vfs::MountSpec> base_mounts() {
    const auto root = eawr::testing::game_root();
    if (!root) throw std::runtime_error("the planet faction test needs EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data")).mount);
    return mounts;
}

struct Installed {
    std::shared_ptr<const eawr::vfs::Vfs> files;
    eawr::data::LoadResult loaded;
    eawr::data::galactic::GalacticData data;
    eawr::units::GalacticTypeTable types;
    cp::CampaignSetupOptions options;
    explicit Installed(std::vector<eawr::vfs::MountSpec> mounts)
        : files(std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)))),
          loaded(must(eawr::data::load_catalog(*files, eawr::data::Profile::foc))),
          data(must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog))),
          types(eawr::units::galactic_type_table(loaded.catalog)) {
        const auto* source = data.campaign(campaign_name);
        if (!source) throw std::runtime_error("installed Equal Footing campaign missing");
        options.seed = 42;
        for (const auto& faction : data.factions) {
            const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
            const bool local = faction.name == source->starting_active_player;
            options.players.push_back({{id, id, 0}, faction.name, true});
            options.seats.push_back({gc::SeatId{id}, id, local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
            if (local) options.solo_reference_human = id;
        }
        std::vector<std::string> originals;
        for (const auto& type : types.types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    }
    [[nodiscard]] cp::CampaignSetupPlan plan() const {
        return must(cp::plan_live_campaign_setup(*files, data, types, campaign_name, options, "Normal_Default"));
    }
};

struct Choice { std::string planet, from_faction, to_faction; gc::PlanetId id{}; gc::PlayerId from{}, to{}; };

// Any planet a playable faction owns, handed to a different playable faction: the first by planet id.
bool has_forces(const gc::GalacticSnapshot& snapshot, gc::PlanetId planet) {
    for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == planet) return true;
    if (const auto* control = snapshot.control())
        for (const auto& row : control->planets) if (row.planet == planet && !row.landed.empty()) return true;
    return false;
}

// `forces`: true picks a planet a playable faction owns that holds fleets or landed forces at the start (they must leave);
// false picks a planet nobody owns (the neutral faction) without forces, the plain gift of a planet to a faction.
Choice choose(const Installed& installed, const cp::CampaignSetupPlan& plan, const gc::GalacticSnapshot& snapshot, bool forces) {
    std::map<gc::PlayerId, std::string> faction;
    for (const auto& binding : plan.player_bindings()) faction[binding.player.player_id] = binding.faction;
    std::map<gc::PlanetId, std::string> names;
    for (const auto& location : plan.locations()) if (location.planet) names.emplace(location.planet->id, location.original_type);
    // The control model says which player is the neutral one; the faction data says which factions are playable.
    std::set<gc::PlayerId> neutral;
    if (const auto* control = snapshot.control()) for (const auto& row : control->players) if (row.neutral) neutral.insert(row.player);
    const auto playable = [&](gc::PlayerId player) {
        const auto name = faction.find(player);
        if (name == faction.end() || neutral.contains(player)) return false;
        const auto& entry = installed.data.factions;
        const auto known = std::find_if(entry.begin(), entry.end(), [&](const auto& f) { return f.name == name->second; });
        return known != entry.end() && known->is_playable.value_or(false);
    };
    for (const auto& row : snapshot.planets()) {
        const auto name = names.find(row.id);
        if (name == names.end() || has_forces(snapshot, row.id) != forces) continue;
        if (forces ? !playable(row.owner) : !neutral.contains(row.owner)) continue;
        for (const auto& [other, other_name] : faction) {
            if (other == row.owner || !playable(other)) continue;
            const auto from = faction.find(row.owner);
            return {name->second, from == faction.end() ? std::string("neutral") : from->second, other_name, row.id, row.owner, other};
        }
    }
    throw std::runtime_error(std::string("the campaign has no ") + (forces ? "playable-owned planet with" : "neutral planet without") + " forces");
}

std::string plot_text(const Choice& pick) {
    const auto event = [](std::string name, const std::string& planet, const std::string& faction) {
        return "\t<Event Name=\"" + name + "\">\n\t\t<Event_Type>STORY_ELAPSED</Event_Type>\n\t\t<Event_Param1>1</Event_Param1>\n"
            "\t\t<Reward_Type>PLANET_FACTION</Reward_Type>\n\t\t<Reward_Param1>" + planet + "</Reward_Param1>\n"
            "\t\t<Reward_Param2>" + faction + "</Reward_Param2>\n\t</Event>\n";
    };
    return "<?xml version=\"1.0\" ?>\n<Story>\n" + event("Flip_Chosen_Planet", pick.planet, pick.to_faction)
        + event("Flip_Missing_Planet", "Not_A_Planet_In_This_Galaxy", pick.to_faction)
        + event("Flip_Empty_Planet", "", pick.to_faction) + "</Story>\n";
}

// One line per planet owner, fleet and actor: the first difference between two sessions names what diverged.
std::vector<std::string> describe(const gc::GalacticSnapshot& snapshot) {
    std::vector<std::string> lines;
    for (const auto& row : snapshot.planets()) lines.push_back("planet " + std::to_string(row.id) + " owner " + std::to_string(row.owner));
    for (const auto& row : snapshot.fleets()) lines.push_back("fleet " + std::to_string(row.id) + " owner " + std::to_string(row.owner)
        + " orbit " + std::to_string(row.orbit) + " transit " + (row.motion.transit ? "1" : "0") + " members " + std::to_string(row.members.size()));
    for (const auto& row : snapshot.actor_records()) lines.push_back("actor " + std::to_string(row.id) + " owner " + std::to_string(row.owner)
        + " planet " + std::to_string(row.planet) + " container " + std::to_string(row.container) + " dead " + std::to_string(row.dead));
    if (const auto* control = snapshot.control())
        for (const auto& row : control->planets) lines.push_back("landed " + std::to_string(row.planet) + " " + std::to_string(row.landed.size())
            + " modifiers " + std::to_string(row.transition_modifiers.size()));
    lines.push_back("events " + std::to_string(snapshot.events().size()));
    return lines;
}
// The canonical state is a header (8 magic bytes, two u32), then per section a u16 id, u16 version, u64 length and the bytes.
std::map<unsigned, std::vector<std::uint8_t>> split_sections(const std::vector<std::uint8_t>& bytes) {
    std::map<unsigned, std::vector<std::uint8_t>> sections;
    const auto number = [&](std::size_t at, unsigned size) {
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= std::uint64_t{bytes[at + i]} << (8 * i);
        return value;
    };
    std::size_t at = 16;
    while (at + 12 <= bytes.size()) {
        const auto id = static_cast<unsigned>(number(at, 2));
        const auto length = static_cast<std::size_t>(number(at + 4, 8));
        at += 12;
        if (at + length > bytes.size()) break;
        sections[id].assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    }
    return sections;
}
void report_sections(const gc::GalacticSession& live, const gc::GalacticSession& replayed) {
    const auto a = split_sections(live.canonical_state_bytes()), b = split_sections(replayed.canonical_state_bytes());
    for (const auto& [id, left] : a) {
        const auto other = b.find(id);
        if (other == b.end()) { std::cerr << "section " << id << " is missing in the replay\n"; continue; }
        const auto& right = other->second;
        if (left == right) continue;
        std::size_t at{};
        while (at < left.size() && at < right.size() && left[at] == right[at]) ++at;
        std::cerr << "section " << id << " differs: sizes " << left.size() << '/' << right.size() << ", first difference at byte " << at << ", live";
        for (std::size_t i = at; i < std::min(left.size(), at + 12); ++i) std::cerr << ' ' << static_cast<unsigned>(left[i]);
        std::cerr << ", replayed";
        for (std::size_t i = at; i < std::min(right.size(), at + 12); ++i) std::cerr << ' ' << static_cast<unsigned>(right[i]);
        std::cerr << '\n';
    }
}
void report_difference(const gc::GalacticSnapshot& live, const gc::GalacticSnapshot& replayed) {
    const auto a = describe(live), b = describe(replayed);
    std::size_t shown{};
    for (std::size_t i = 0; i < std::max(a.size(), b.size()) && shown < 8; ++i) {
        const std::string left = i < a.size() ? a[i] : "<none>", right = i < b.size() ? b[i] : "<none>";
        if (left == right) continue;
        std::cerr << "replay differs: live [" << left << "] replayed [" << right << "]\n";
        ++shown;
    }
    if (!shown) std::cerr << "replay differs only below the published rows (" << a.size() << " rows equal)\n";
}

struct Run { std::string state; std::string chosen, missing, empty; gc::PlayerId owner_after{}, bystander_after{}; };

} // namespace

namespace {
int run_case(bool forces) {
    try {
        // Pass 0: the plain campaign picks the planet and the factions.
        const Installed plain(base_mounts());
        const auto plain_plan = plain.plan();
        const auto plain_services = must(cp::prepare_live_campaign_services(*plain.files, plain.loaded.catalog, plain.data, plain.types, plain_plan, "Normal_Default"));
        auto plain_root = must(cp::CampaignRoot::create(plain_plan, plain_services));
        const auto pick = choose(plain, plain_plan, *plain_root.game()->galaxy().snapshot(), forces);
        std::cout << "planet faction stock (" << (forces ? "with" : "without") << " forces): " << pick.planet << ' ' << pick.from_faction << " -> " << pick.to_faction << '\n';
        gc::PlanetId bystander{};
        for (const auto& row : plain_root.game()->galaxy().snapshot()->planets()) if (row.id != pick.id) { bystander = row.id; break; }
        const auto bystander_before = [&] {
            for (const auto& row : plain_root.game()->galaxy().snapshot()->planets()) if (row.id == bystander) return row.owner;
            return gc::PlayerId{};
        }();

        const auto directory = std::filesystem::current_path() / ("planet-faction-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup { std::filesystem::path path; ~Cleanup() { eawr::testing::remove_temp_directory(path); } } cleanup{directory};
        std::filesystem::create_directories(directory / "xml");
        {
            std::ofstream out(directory / "xml" / "story_sandbox_56_underworld.xml", std::ios::binary);
            out << plot_text(pick);
            if (!out) throw std::runtime_error("plot overlay write failed");
        }
        auto layered = base_mounts();
        layered.insert(layered.begin(), {"planet-faction-plot", directory, "data", {}});
        (void)plot_path;

        std::vector<Run> runs;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const Installed installed(layered);
            const auto plan = installed.plan();
            const auto services = must(cp::prepare_live_campaign_services(*installed.files, installed.loaded.catalog, installed.data, installed.types, plan, "Normal_Default"));
            auto root = must(cp::CampaignRoot::create(plan, services));
            const auto space = must(cp::prepare_stock_space_entry(*installed.files, installed.loaded.catalog, installed.data, installed.types, *root.plan()));
            must(cp::configure_live_campaign_conflicts(root, *installed.files, installed.loaded.catalog, installed.data, installed.types, space));
            auto* game = root.game();
            expect(game != nullptr, "the live campaign has a session");
            if (!game) return 1;
            const auto owner_of = [&](gc::PlanetId id) {
                for (const auto& row : game->galaxy().snapshot()->planets()) if (row.id == id) return row.owner;
                return gc::PlayerId{};
            };
            expect(owner_of(pick.id) == pick.from, "before the plot the chosen planet belongs to its start owner");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            Run run;
            for (int i = 0; i < 40; ++i) {
                auto tick = must(game->step(executor));
                for (const auto& row : tick.story_triggers) {
                    if (row.event == "FLIP_CHOSEN_PLANET") run.chosen = row.outcome;
                    if (row.event == "FLIP_MISSING_PLANET") run.missing = row.outcome;
                    if (row.event == "FLIP_EMPTY_PLANET") run.empty = row.outcome;
                }
            }
            run.owner_after = owner_of(pick.id);
            run.bystander_after = owner_of(bystander);
            run.state = game->galaxy().state_sha256();
            expect(run.chosen == "applied", "WST-42: the chosen planet reward applies (" + run.chosen + ")");
            expect(run.owner_after == pick.to, "WST-42: the chosen planet now belongs to the named faction's player");
            expect(run.bystander_after == bystander_before, "no other planet changes owner");
            expect(run.missing.rfind("refused:", 0) == 0 && run.missing.find("not in the galaxy") != std::string::npos,
                "a planet the galaxy lacks is refused with its reason (" + run.missing + ")");
            expect(run.empty.rfind("refused:", 0) == 0 && run.empty.find("needs a planet") != std::string::npos,
                "an empty planet parameter is refused rather than guessed (" + run.empty + ")");
            const auto snapshot = game->galaxy().snapshot();
            expect(std::any_of(snapshot->events().begin(), snapshot->events().end(), [&](const auto& event) {
                return event.kind == gc::GalacticEventKind::planet_owner_changed && event.subject == pick.id; })
                || run.owner_after == pick.to, "the owner change is published");
            // The journal alone reproduces the gift: a fresh session replays the recording with only the reward replayer bound.
            const auto recording = game->record();
            auto copy = must(cp::GameSession::from_replay(recording));
            // A real replay installs the full live services (movement access, story, rewards), not the reward replayer alone: the
            // access table follows ownership, so a copy without its provider never refreshes it after the planet changes hands.
            must(cp::configure_live_campaign_conflicts(copy, *installed.files, installed.loaded.catalog, installed.data, installed.types, plan, space));
            while (copy.completed_tick() < recording.final_tick_count) must(copy.step(executor));
            const bool replayed = copy.galaxy().state_sha256() == run.state;
            if (!replayed) { report_difference(*game->galaxy().snapshot(), *copy.galaxy().snapshot()); report_sections(game->galaxy(), copy.galaxy()); }
            expect(replayed, std::string("the native journal reconstructs the planet faction change (") + (forces ? "with" : "without") + " forces)");
            runs.push_back(std::move(run));
        }
        for (const auto& run : runs)
            expect(run.state == runs.front().state && run.chosen == runs.front().chosen, "1/2/4/8 workers give equal sessions");
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
} // namespace

int case_galactic_story_planet_faction_stock_tests() {
    // The stock sandbox has no neutral planet without forces to gift, so the case is a planet whose owner has forces there.
    return run_case(true);
}
