#include "cli_internal.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include <cctype>
#include <set>

namespace sim_headless::cli {

int run_tactical(const Options& options, const std::vector<std::uint8_t>& bytes) {
    namespace tactical = eawr::sim::tactical;
    const auto replay = tactical::parse_replay(bytes, options.replay_path);
    if (!replay) {
        std::cerr << eawr::core::format_diagnostic(replay.error()) << '\n';
        return 3;
    }
    if (replay.value().setup.match_identity && options.game_root.empty()) {
        std::cerr << "EAWR-MATCH-0002: --game-root is required to verify this replay's match identity\n";
        return 3;
    }
    if (options.game_root.empty()) {
        // Only the built-in synthetic fixture closures can run with default tables.
        std::ostringstream encoded_identity;
        for (const auto byte : replay.value().setup.content_identity)
            encoded_identity << "0123456789abcdef"[byte >> 4U] << "0123456789abcdef"[byte & 15U];
        const auto identity = encoded_identity.str();
        const std::set<std::string> built_in{
            "3742568d91976a9a0e3ad533bbad509bcfce3aa5f6f7c10bed1b393f0bc6e63b",
            "c709d8b355dc2770cd7e69af5507966a8185df88ef46ede7a44781962ad1c7e6",
            "7355e046d3d095645949a6ecd10a2767e8bd0c750bde02ea005ee1dc76baa91e",
            "d3928a3b8606ccdb6bc351847164108ae11fc784769116cded247794b1999cd0",
            "7171717171717171717171717171717171717171717171717171717171717171"};
        if (!built_in.contains(identity)) {
            std::cerr << "--game-root is required for a replay outside the built-in synthetic content closures\n";
            return 3;
        }
    }
    // With --game-root the session runs with the installation's content tables, which must
    // be the content the setup names.
    eawr::skirmish::SessionContent content;
    tactical::VictoryRules victory;
    tactical::EconomyRules economy;
    if (!options.game_root.empty()) {
        auto loaded = load_tables(options);
        if (!loaded) {
            std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
            return 3;
        }
        if (replay.value().setup.skirmish) {
            const auto& setup = replay.value().setup;
            const auto& metadata = *setup.skirmish;
            const auto fail = [](const eawr::core::Diagnostic& error) {
                std::cerr << eawr::core::format_diagnostic(error) << '\n';
                return 3;
            };
            const auto& filesystem = *loaded.value()->filesystem;
            const auto& catalog = loaded.value()->catalog->catalog;
            auto& tables = *loaded.value()->tables;
            eawr::scene::VfsAssetCache cache(filesystem);
            eawr::units::LoadInput input;
            input.catalog = &catalog;
            input.filesystem = &filesystem;
            input.model = cache.access().model;
            input.space_map = metadata.map;
            // CLI M2 recordings retain the legacy pinned table closure. Viewer
            // recordings use selected-map tables; resolve that closure only if
            // the already-loaded tables do not match the recorded identity.
            if (eawr::units::content_identity(tables) != setup.content_identity) {
                auto selected_tables = eawr::units::load_unit_tables(input);
                if (!selected_tables) return fail(selected_tables.error());
                tables = std::move(selected_tables).value();
            }
            eawr::skirmish::FixtureOptions selected;
            selected.map = metadata.map;
            auto fixture = [&] {
                if (setup.first_free_entity_id) {
                    // A selected-force child may use a campaign map without lobby capacity or
                    // team spawn markers. Its recorded identity is checked against the raw inputs.
                    eawr::skirmish::Fixture recorded;
                    recorded.map = metadata.map;
                    recorded.map_sha256 = metadata.map_sha256;
                    return eawr::core::Result<eawr::skirmish::Fixture>::success(std::move(recorded));
                }
                return eawr::skirmish::fixture_from_options(selected, filesystem, catalog);
            }();
            if (!fixture) return fail(fixture.error());
            auto inputs = eawr::skirmish::read_start_inputs(fixture.value(), filesystem, catalog, tables);
            if (!inputs) return fail(inputs.error());
            fixture = eawr::skirmish::replay_fixture(fixture.value(), inputs.value(), setup, false);
            if (!fixture) return fail(fixture.error());
            inputs = eawr::skirmish::read_start_inputs(fixture.value(), filesystem, catalog, tables);
            if (!inputs) return fail(inputs.error());
            // Mirror the live selected-faction closure; the recording's identity still guards every load.
            std::set<std::string> missing;
            const auto same_name = [](const std::string& left, const std::string& right) {
                return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                    [](const unsigned char a, const unsigned char b) { return std::tolower(a) == std::tolower(b); });
            };
            const auto need = [&](const std::string& type) {
                if ((setup.first_free_entity_id || !eawr::skirmish::roster_disabled_types().contains(eawr::battle::type_id(type)))
                    && !tables.find(type)) missing.insert(type);
            };
            for (const auto& slot : fixture.value().slots) {
                for (const auto& type : slot.fleet) need(type);
                if (setup.first_free_entity_id) continue;
                for (const auto& forces : inputs.value().faction_forces)
                    if (same_name(forces.faction, slot.faction))
                        for (const auto& type : forces.space_skirmish_default_forces) need(type);
                const std::string marker = "Team_" + std::string(slot.team < 10U ? "0" : "")
                    + std::to_string(slot.team) + "_Space_Station";
                for (const auto& placement : inputs.value().placements) {
                    if (!placement.marker || !same_name(placement.type, marker)) continue;
                    for (const auto& candidate : placement.marker_for) {
                        std::string affiliations = candidate.affiliation;
                        std::replace(affiliations.begin(), affiliations.end(), ',', ' ');
                        std::istringstream names(affiliations);
                        std::string faction;
                        while (names >> faction) if (same_name(faction, slot.faction)) need(candidate.type);
                    }
                }
            }
            if (!missing.empty() && eawr::units::content_identity(tables) != setup.content_identity) {
                for (const auto type : eawr::units::pinned_m2_types()) input.types.emplace_back(type);
                input.types.insert(input.types.end(), missing.begin(), missing.end());
                for (const auto type : eawr::units::pinned_m2_obstacles()) input.obstacles.emplace_back(type);
                auto extended_tables = eawr::units::load_unit_tables(input);
                if (!extended_tables) return fail(extended_tables.error());
                tables = std::move(extended_tables).value();
                inputs.value().tables = &tables;
            }
            if (auto valid = eawr::skirmish::validate_replay_identity(setup, inputs.value()); !valid) return fail(valid.error());
            if (eawr::units::content_identity(tables) != setup.content_identity) {
                std::cerr << "recorded skirmish content identity differs from the mounted tables\n";
                return 3;
            }
            if (setup.first_free_entity_id) {
                auto bound = eawr::campaign::conflict_entry_content(setup, inputs.value());
                if (!bound) return fail(bound.error());
                content.sensors = std::move(bound.value().sensors);
                content.durability = std::move(bound.value().durability);
                content.motion = std::move(bound.value().motion);
                content.combat = std::move(bound.value().combat);
                content.abilities = std::move(bound.value().abilities);
                content.fog = std::move(bound.value().fog);
                victory = std::move(bound.value().victory);
                economy = std::move(bound.value().economy);
            } else {
                auto start = eawr::skirmish::build_start(fixture.value(), inputs.value());
                if (!start) return fail(start.error());
                auto bound = eawr::skirmish::session_content(tables, eawr::skirmish::human_slots(fixture.value()));
                if (!bound) return fail(bound.error());
                content = std::move(bound).value();
                if (auto levels = eawr::skirmish::bind_difficulty(content, start.value(), inputs.value()); !levels)
                    return fail(levels.error());
                auto fog = eawr::skirmish::fog_rules(inputs.value());
                if (!fog) return fail(fog.error());
                content.fog = fog.value();
                content.map_box = eawr::skirmish::map_box(inputs.value());
                auto rules = eawr::skirmish::economy_rules(start.value(), inputs.value(), tables);
                if (!rules) return fail(rules.error());
                economy = std::move(rules).value();
                victory = eawr::skirmish::victory_rules(start.value(), tables);
            }
        } else {
            // Scenario recordings can extend the default table closure with stock types.
            // Resolve their checksums in setup order, as the native recorder does. The
            // identity check below still rejects a different installation or closure.
            if (!replay.value().setup.match_identity
                && eawr::units::content_identity(*loaded.value()->tables) != replay.value().setup.content_identity) {
                auto& tables = *loaded.value()->tables;
                const auto& catalog = loaded.value()->catalog->catalog;
                std::vector<std::string> extra;
                for (const auto& unit : replay.value().setup.units) {
                    const bool known = std::any_of(tables.units.begin(), tables.units.end(),
                        [&](const auto& type) { return eawr::battle::type_id(type.id) == unit.type_id; });
                    if (known) continue;
                    std::vector<std::string> matches;
                    for (const auto& definition : catalog.definitions()) {
                        if (definition.namespace_winner && definition.category == eawr::data::Category::game_object
                            && eawr::battle::type_id(definition.id) == unit.type_id) matches.push_back(definition.id);
                    }
                    if (matches.size() != 1) {
                        std::cerr << "recorded scenario type does not resolve uniquely in installed content\n";
                        return 3;
                    }
                    if (std::find(extra.begin(), extra.end(), matches.front()) == extra.end()) extra.push_back(matches.front());
                }
                if (!extra.empty()) {
                    eawr::scene::VfsAssetCache cache(*loaded.value()->filesystem);
                    eawr::units::LoadInput input;
                    input.catalog = &catalog;
                    input.filesystem = &*loaded.value()->filesystem;
                    input.model = cache.access().model;
                    for (const auto type : eawr::units::pinned_m2_types()) input.types.emplace_back(type);
                    input.types.insert(input.types.end(), extra.begin(), extra.end());
                    auto extended = eawr::units::load_unit_tables(input);
                    if (!extended) {
                        std::cerr << eawr::core::format_diagnostic(extended.error()) << '\n';
                        return 3;
                    }
                    tables = std::move(extended).value();
                }
            }
            if (!replay.value().setup.match_identity
                && eawr::units::content_identity(*loaded.value()->tables) != replay.value().setup.content_identity) {
                eawr::core::Diagnostic mismatch;
                mismatch.code = std::string(tactical::diagnostic_codes::invalid_setup);
                mismatch.message = "the unit tables of --game-root are not the content the replay's setup names";
                mismatch.logical_path = options.replay_path;
                std::cerr << eawr::core::format_diagnostic(mismatch) << '\n';
                return 3;
            }
            // #77, #76: the replay does not record which players are human; they are the pinned
            // fixture's human slots (the victory rules' and the ability table's).
            const auto humans = eawr::skirmish::human_slots(eawr::skirmish::m2_fixture());
            auto tables = eawr::skirmish::session_content(*loaded.value()->tables, humans);
            if (!tables) {
                std::cerr << eawr::core::format_diagnostic(tables.error()) << '\n';
                return 3;
            }
            content = std::move(tables).value();
            // #495: an M2 replay runs on the M2 map's fog grid, as the live session does.
            auto inputs = eawr::skirmish::read_start_inputs(eawr::skirmish::m2_fixture(), *loaded.value()->filesystem,
                loaded.value()->catalog->catalog, *loaded.value()->tables);
            if (!inputs) {
                std::cerr << eawr::core::format_diagnostic(inputs.error()) << '\n';
                return 3;
            }
            auto fog = eawr::skirmish::fog_rules(inputs.value());
            if (!fog) {
                std::cerr << eawr::core::format_diagnostic(fog.error()) << '\n';
                return 3;
            }
            content.fog = fog.value();
            content.map_box = eawr::skirmish::map_box(inputs.value());
            victory = eawr::skirmish::victory_rules(replay.value().setup, *loaded.value()->tables, humans,
                inputs.value().space_victory_condition);
            // #530: a replay of the M2 start runs with its economy, as the live session does. The start
            // is rebuilt from the same inputs; a replay of another setup has none.
            auto fixture = eawr::skirmish::m2_fixture();
            fixture.match = eawr::skirmish::replay_match_options(replay.value().setup, inputs.value().match_defaults);
            if (replay.value().setup.match_policy) {
                auto recorded = eawr::skirmish::replay_fixture(fixture, inputs.value(), replay.value().setup);
                if (!recorded) { std::cerr << eawr::core::format_diagnostic(recorded.error()) << '\n'; return 3; }
                fixture = std::move(recorded).value();
                inputs = eawr::skirmish::read_start_inputs(fixture, *loaded.value()->filesystem,
                    loaded.value()->catalog->catalog, *loaded.value()->tables);
                if (!inputs) { std::cerr << eawr::core::format_diagnostic(inputs.error()) << '\n'; return 3; }
            }
            // MTID is authoritative even without SKSU. That legacy replay mode
            // resolves the M2 inputs above, then verifies every recorded domain.
            if (auto valid = eawr::skirmish::validate_replay_identity(replay.value().setup, inputs.value()); !valid) {
                std::cerr << eawr::core::format_diagnostic(valid.error()) << '\n';
                return 3;
            }
            auto start = eawr::skirmish::build_start(fixture, inputs.value());
            if (start && start.value().setup.players == replay.value().setup.players
                && (replay.value().setup.match_policy || start.value().setup.units == replay.value().setup.units)) {
                auto rules = eawr::skirmish::economy_rules(start.value(), inputs.value(), *loaded.value()->tables);
                if (!rules) {
                    std::cerr << eawr::core::format_diagnostic(rules.error()) << '\n';
                    return 3;
                }
                economy = std::move(rules).value();
            }
        }
    }
    auto session_result = tactical::TacticalSession::from_replay(
        replay.value(), content.sensors, content.durability, content.motion, content.fog, content.combat, victory,
        content.abilities, economy, content.map_box);
    if (!session_result) {
        std::cerr << eawr::core::format_diagnostic(session_result.error()) << '\n';
        return 3;
    }
    auto session = std::move(session_result).value();
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    std::ostringstream hashes;
    std::ostringstream events;
    std::ostringstream snapshots;
    std::string census;
    if (!options.census_path.empty()) {
        auto text = eawr::skirmish::census_json(replay.value().setup, nullptr);
        if (!text) {
            std::cerr << eawr::core::format_diagnostic(text.error()) << '\n';
            return 3;
        }
        census = std::move(text).value();
    }
    hashes << "tick,sha256\n";
    events << "tick,player,sequence,unit,event,order,reason\n";
    snapshots << "tick,sha256\n";
    snapshots << session.completed_tick() << ',' << session.snapshot()->sha256() << '\n';
    while (session.completed_tick() < replay.value().final_tick_count) {
        const auto tick = session.step(executor);
        if (!tick) {
            std::cerr << eawr::core::format_diagnostic(tick.error()) << '\n';
            return 3;
        }
        hashes << tick.value().completed_tick << ',' << tick.value().state_sha256 << '\n';
        snapshots << tick.value().completed_tick << ',' << tick.value().snapshot->sha256() << '\n';
        for (const auto& event : tick.value().snapshot->events()) {
            events << event.tick << ',' << event.player << ',' << event.sequence << ','
                   << event.unit << ',' << tactical::to_string(event.kind) << ','
                   << tactical::to_string(event.order) << ',' << tactical::to_string(event.reason)
                   << '\n';
        }
    }

    if (const auto& outcome = session.outcome()) {
        std::cout << "outcome " << tactical::to_string(outcome->condition) << " winner " << outcome->winner << " team "
                  << outcome->winner_team << " decided_tick " << outcome->decided_tick << " unit "
                  << outcome->deciding_unit << " end_tick " << outcome->end_tick << '\n';
    }
    std::vector<eawr::platform::PublishedFile> outputs;
    outputs.push_back({"hash output", options.hash_path, hashes.str()});
    if (!options.events_path.empty()) {
        outputs.push_back({"event output", options.events_path, events.str()});
    }
    if (!options.snapshot_path.empty()) {
        outputs.push_back({"snapshot output", options.snapshot_path, snapshots.str()});
    }
    if (!options.census_path.empty()) {
        outputs.push_back({"census output", options.census_path, census});
    }
    return publish(outputs);
}

// Human-readable tick-zero listing: players, then units, then the SK-23 launches.

int run_replay(const std::optional<Options>& options) {
    std::ifstream input(options->replay_path, std::ios::binary | std::ios::ate);
    if (!input) {
        return report_io_error(options->replay_path, "could not open replay input", 3);
    }
    const auto end = input.tellg();
    const auto input_size = static_cast<std::streamoff>(end);
    if (input_size < 0 || static_cast<std::uint64_t>(input_size) > eawr::sim::replay_max_bytes) {
        return report_io_error(options->replay_path, "replay input exceeds the 256 MiB limit", 3);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input_size));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input && !bytes.empty()) {
        return report_io_error(options->replay_path, "could not read complete replay input", 3);
    }

    if (const auto tactical_version = eawr::sim::tactical::peek_replay_format_version(bytes);
        tactical_version == eawr::sim::tactical::replay_format_version
        || tactical_version == eawr::sim::tactical::replay_format_version_squadrons
        || tactical_version == eawr::sim::tactical::replay_format_version_extensions
        || tactical_version == eawr::sim::tactical::replay_format_version_squadron_extensions) {
        if (!options->trace_path.empty()) {
            return report_argument_error("--trace-out requires a replay-v1 input");
        }
        return run_tactical(*options, bytes);
    }
    if (!options->game_root.empty()) {
        return report_argument_error("--game-root requires a replay-v2 input");
    }
    if (!options->events_path.empty() || !options->snapshot_path.empty() || !options->census_path.empty()) {
        return report_argument_error(
            "--events-out, --snapshot-out and --census-out require a replay-v2 input");
    }

    const auto replay = eawr::sim::parse_replay(bytes, options->replay_path);
    if (!replay) {
        std::cerr << eawr::core::format_diagnostic(replay.error()) << '\n';
        return 3;
    }
    auto world_result = eawr::sim::World::create(replay.value());
    if (!world_result) {
        std::cerr << eawr::core::format_diagnostic(world_result.error()) << '\n';
        return 3;
    }
    auto world = std::move(world_result).value();
    const eawr::platform::ThreadWorkerAdapter executor(options->workers);
    const bool tracing = !options->trace_path.empty();
    const auto objects = tracing ? trace_objects(replay.value()) : std::vector<TraceObject>{};
    std::ostringstream csv;
    std::ostringstream trace;
    csv << "tick,sha256\n";
    if (tracing) {
        trace << "tick,object,field,value\n";
        append_trace_rows(trace, world.completed_tick(), objects, world.entities());
    }
    while (world.completed_tick() < world.final_tick_count()) {
        const auto tick = world.step(executor);
        if (!tick) {
            std::cerr << eawr::core::format_diagnostic(tick.error()) << '\n';
            return 3;
        }
        csv << tick.value().completed_tick << ',' << tick.value().state_sha256 << '\n';
        if (tracing) {
            append_trace_rows(trace, tick.value().completed_tick, objects, world.entities());
        }
    }

    std::string header;
    if (tracing) {
        // The remake's build identity is the SHA-256 of the running sim_headless image.
        const auto build = eawr::platform::current_executable_sha256();
        if (!build) {
            return report_io_error(options->trace_path, "could not hash the running executable", 4);
        }
        header = trace_header(replay.value(), eawr::sim::sha256_hex(bytes), *build);
    }
    std::vector<eawr::platform::PublishedFile> outputs;
    outputs.push_back({"hash output", options->hash_path, csv.str()});
    if (tracing) {
        outputs.push_back({"trace output", options->trace_path, trace.str()});
        outputs.push_back({"trace header", trace_header_path(options->trace_path).string(), header});
    }
    return publish(outputs);
}

} // namespace sim_headless::cli
