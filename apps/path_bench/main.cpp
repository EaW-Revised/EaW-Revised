// Path search cost in the M2 battle (#520, docs/simulation.md#path-search-cost): loads the FoC M2
// start with the FoC AI (needs the game data), gives the human player's move orders across the
// map, and prints the tick times after the order with the work and time of the path searches
// (tactical::PathSearchProbe). Exits 1 when two worker counts disagree on a tick's state hash.
//
//   path_bench [--game-root <dir>] [--selection owner|owner-group|all|large[,...]] [--order-tick <n>] [--ticks 300]
//              [--workers 1,4] [--destination x,y] [--timing on|off] [--csv <prefix>] [--list 1]
//              [--pin <file> [--update-pin 1]] [--replay-out <file>] [--ships <prefix>] [--search-budget <n>]
//
// "owner" (the default, the owner's benchmark) stages 20 capital ships of the four M2 types that
// move at tick 0 and orders them at tick 600: 12 as one group move and 8 as single moves in the
// same tick, across the map through the centre's stations and pads. "owner-group" stages the same
// 20 ships and gives them one player order at tick 600, a group move of all 20 to the owner
// group's target (#613: the eye-check clip's order; the 8 single moves of "owner" are no player
// order, and their ships stand at rest in the group's way when its front ships plan). "all" orders every ship and
// squadron the human player has at tick 3000 (the station's launches included); "large" does the
// same with 9 staged ships more. --timing off leaves the probe's clock out of the tick times.
// --csv writes <prefix>-<selection>-w<workers>.csv (per tick) and ...-searches.csv (per search).
// Tick CSVs include world, AI engine, Lua service, commit and partition costs. --execution live
// uses production dispatch and hashing; legacy (the default) retains the historical workload.
// --profile on samples the stepping threads on Windows x64 (EAWR_DEBUG_SYMBOLS names functions).
// Profile runs perturb tick times and must be kept separate from timed comparisons.
// --replay-out writes the first run's replay (setup, staged ships and every command, the AI's
// included) for `--eawr-live-session replay` in the viewer or `sim_headless --replay`.
// --search-budget <n> replaces the per-tick search budget of the avoidance rules (PC-07; a large
// value plans every search whole in its frame, as FoC does).
// --ships writes <prefix>-<selection>-w<workers>.csv: the staged ships after the order of an owner
// selection (every tick for 16 ticks, every 10 up to 300, then every 100): position, yaw and plan
// (start tick, target, nodes, end frame).
// --pin (the regression test path_bench_owner_work): the first worker count must be 1, whose
// searches come in planning order; every search's (and slice's) work from the order on must
// match <file> (--update-pin 1 rewrites it), and every tick must stay in its budgets
// (space-movement PC-07, PC-08): no search starts in a layer that has spent its budget, none
// passes it by more than one parent, and no slice passes the slice size by more than one parent
// (a sliced search that has not ended at its landing would). Without the game data it prints
#include "path_bench_internal.hpp"

using namespace path_bench;

int main(const int argc, const char* const argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--melee") return eawr::bench::melee_main(argc, argv);
        if (std::string_view(argv[index]) == "--profile-attach") return eawr::bench::attach_main(argc, argv);
    }
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::cerr << "usage: path_bench [--game-root <dir>] [--order-tick <n>] [--ticks <n>] [--workers <n>[,<n>...]]\n"
                     "                  [--selection owner|owner-group|all|large[,...]] [--csv <prefix>]\n"
                     "                  [--execution live|legacy] [--profile on [--profile-interval <us>]]\n";
        return 2;
    }
    if (!options->game_root) {
        if (options->pin) {
            std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the path benchmark\n";
            return 0;
        }
        std::cerr << "path_bench: set EAWR_EAW_GAME_ROOT or pass --game-root\n";
        return 2;
    }
    if (options->pin && (options->workers.front() != 1 || options->selections.size() != 1)) {
        std::cerr << "path_bench: --pin needs one selection and 1 as the first worker count\n";
        return 2;
    }
    auto content = load(*options->game_root);
    if (!content) return 1;
    if (options->search_budget && content->content.motion.avoidance) {
        content->content.motion.avoidance->search_budget = *options->search_budget;
    }
    if (options->list) {
        for (const auto& unit : content->start.units) {
            const auto* footprint = content->content.motion.footprint(unit.state.type_id);
            std::cout << "start " << unit.state.entity_id << ' ' << unit.type << " role " << skirmish::to_string(unit.role)
                      << " owner " << unit.state.owner << " at (" << unit.state.position.x.raw() / Fixed::scale << ", "
                      << unit.state.position.y.raw() / Fixed::scale << ")"
                      << (footprint != nullptr ? " layer " + std::to_string(static_cast<int>(footprint->layer)) : std::string())
                      << '\n';
        }
        for (const auto name : {"Corellian_Corvette", "Corellian_Gunboat", "Nebulon_B_Frigate", "Alliance_Assault_Frigate",
                 "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Broadside_Class_Cruiser", "Interdictor_Cruiser",
                 "Star_Destroyer", "Victory_Destroyer", "Calamari_Cruiser", "Marauder_Missile_Cruiser", "Home_One"}) {
            const auto type = skirmish::type_id(name);
            const auto* footprint = content->content.motion.footprint(type);
            std::cout << "type " << name << (content->content.motion.find(type) != nullptr ? " moves" : " no motion")
                      << (footprint != nullptr ? " layer " + std::to_string(static_cast<int>(footprint->layer)) : std::string())
                      << '\n';
        }
    }
    Probe probe(options->timing);
    tactical::set_path_search_probe(&probe);
    bool agree = true;
    for (const auto& selection : options->selections) {
        std::cout << "selection " << selection << ", execution " << (options->live_execution ? "live" : "legacy") << ":\n";
        std::optional<std::vector<std::string>> reference;
        for (const auto workers : options->workers) {
            std::optional<eawr::bench::Sampler> sampler;
            if (options->profile) sampler.emplace(options->profile_interval_us);
            auto result = run(*content, selection, workers, *options, probe, sampler ? &*sampler : nullptr);
            if (!result) {
                tactical::set_path_search_probe(nullptr);
                return 1;
            }
            if (sampler) std::cout << "  (profiled: the sampling perturbs these tick times)\n";
            report(*result, selection, workers);
            if (sampler) sampler->report(std::cout, 60);
            std::string joined;
            for (const auto& hash : result->hashes) joined += hash;
            std::cout << "    hashes digest " << eawr::sim::sha256_hex(std::span(
                reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size())) << '\n';
            if (options->csv) {
                const auto prefix = *options->csv + "-" + selection + "-w" + std::to_string(workers);
                write_csv(prefix + ".csv", *result);
                write_searches_csv(prefix + "-searches.csv", *result);
            }
            if (options->ships && !result->ships_csv.empty()) {
                std::ofstream ships(*options->ships + "-" + selection + "-w" + std::to_string(workers) + ".csv", std::ios::binary);
                ships << result->ships_csv;
            }
            if (options->pin && !reference) {
                const auto rows = work_rows(*result);
                for (const auto& violation : budget_violations(*result, *content->content.motion.avoidance)) {
                    std::cout << "    FAIL: " << violation << '\n';
                    agree = false;
                }
                if (options->update_pin) {
                    std::ofstream output(*options->pin, std::ios::binary);
                    output << "tick,entity,layer,part,tries,expansions,queries,leaves,narrow\n";
                    for (const auto& row : rows) output << row << '\n';
                    std::cout << "    wrote " << *options->pin << '\n';
                } else {
                    std::ifstream input(*options->pin, std::ios::binary);
                    std::string line;
                    std::getline(input, line);
                    std::vector<std::string> pinned;
                    while (std::getline(input, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        pinned.push_back(line);
                    }
                    if (pinned != rows) {
                        std::cout << "    FAIL: the searches' work differs from " << *options->pin << " (" << rows.size()
                                  << " searches, " << pinned.size() << " pinned)\n";
                        for (std::size_t index = 0; index < std::max(rows.size(), pinned.size()); ++index) {
                            const auto now = index < rows.size() ? rows[index] : std::string("-");
                            const auto was = index < pinned.size() ? pinned[index] : std::string("-");
                            if (now != was) std::cout << "      pinned " << was << ", now " << now << '\n';
                        }
                        agree = false;
                    }
                }
            }
            if (!reference) {
                reference = result->hashes;
            } else if (*reference != result->hashes) {
                std::cout << "    FAIL: the hashes differ from the first worker count\n";
                agree = false;
            }
        }
    }
    tactical::set_path_search_probe(nullptr);
    return agree ? 0 : 1;
}
