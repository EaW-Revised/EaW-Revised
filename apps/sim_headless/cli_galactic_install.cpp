#include "cli_galactic_install.hpp"
#include "cli_galactic_install_report.hpp"

#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/story_plots.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <syncstream>
#include "eawr/script/foc/galactic_ai.hpp"

#include <array>
#include "eawr/diagnostics/gc_memory.hpp"
#include <fstream>
#include <thread>
#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sim_headless::cli {

namespace {

namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;

using Report = InstalledCampaignReport;

[[nodiscard]] std::string quoted(const std::string& text) { return eawr::core::json_quote(text); }

// Diagnostics a stage went on without: counts by code, plus every message as a fact so nothing is silent.
void diagnostics_fact(Report& report, const std::string& stage, const std::span<const eawr::core::Diagnostic> rows) {
    std::map<std::string, std::size_t> by_code;
    std::ostringstream messages;
    messages << '[';
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ++by_code[rows[i].code];
        messages << (i ? ",\n      " : "\n      ") << quoted(rows[i].code + ": " + rows[i].message);
    }
    messages << "\n    ]";
    report.fact(stage + "_diagnostics", messages.str());
    std::ostringstream detail;
    detail << rows.size() << " diagnostics";
    for (const auto& [code, count] : by_code) detail << ' ' << code << '=' << count;
    std::cout << "       " << stage << ": " << detail.str() << '\n';
}

// The human player's side of a started campaign: one fleet move, one purchase, the fiscal clock. Each result is a
// stage line; nothing here changes the session beyond ordinary player commands.
void gameplay(Report& report, gc::GalacticSession& session, const eawr::platform::ThreadWorkerAdapter& executor,
    const std::uint64_t play_ticks) {
    const auto first = session.snapshot();
    gc::SeatId seat_id{};
    gc::PlayerId player{};
    for (const auto& row : first->seats()) {
        if (row.kind == gc::SeatKind::human && row.active) {
            seat_id = row.seat_id;
            player = row.player;
            break;
        }
    }
    if (!player) {
        report.note("play", false, "no active human seat");
        return;
    }
    std::uint64_t sequence = 0;
    const auto submit = [&](gc::CommandPayload payload) {
        return session.submit(gc::GameCommand{{session.completed_tick() + 1, seat_id, ++sequence}, std::move(payload)});
    };
    const auto credits_of = [&](const gc::GalacticSnapshot& snapshot) -> std::int64_t {
        if (!snapshot.player_economy()) return -1;
        for (const auto& row : snapshot.player_economy()->players)
            if (row.player == player) return row.credits.trunc_to_integer();
        return -1;
    };
    // A failed step ends the gameplay stage with its diagnostic; it is never read as "still travelling" or "not produced".
    std::string step_error;
    const auto advance = [&]() {
        const auto stepped = session.step(executor);
        if (!stepped) step_error = eawr::core::format_diagnostic(stepped.error());
        return bool(stepped);
    };
    const auto start_cycle = first->clock().fiscal_cycle;
    const auto start_credits = credits_of(*first);

    // Fleet move: the first human fleet in orbit with a route out of its planet.
    bool moved = false;
    std::string refusal;
    for (const auto& fleet : first->fleets()) {
        if (moved || fleet.owner != player || fleet.motion.transit || fleet.members.empty()) continue;
        for (const auto& route : first->routes()) {
            if (route.from != fleet.orbit && route.to != fleet.orbit) continue;
            const auto destination = route.from == fleet.orbit ? route.to : route.from;
            gc::MoveFleet order;
            order.fleet = fleet.id;
            order.path = {fleet.orbit, destination};
            if (const auto sent = submit(order); !sent) {
                if (refusal.empty()) refusal = eawr::core::format_diagnostic(sent.error());
                continue;
            }
            std::uint64_t ticks = 0;
            bool arrived = false;
            while (ticks < play_ticks && !arrived) {
                if (!advance()) break;
                ++ticks;
                for (const auto& now : session.snapshot()->fleets())
                    if (now.id == fleet.id) arrived = now.orbit == destination && !now.motion.transit;
            }
            report.note("play-move", arrived, "fleet " + std::to_string(fleet.id) + " from planet " + std::to_string(fleet.orbit)
                + " to planet " + std::to_string(destination) + (arrived ? " arrived after " : " still travelling after ")
                + std::to_string(ticks) + " ticks" + (step_error.empty() ? std::string{} : "; step failed: " + step_error));
            if (!step_error.empty()) return;
            moved = true;
            break;
        }
    }
    if (!moved) report.note("play-move", false, "no human fleet could be ordered along a route; first refusal: "
        + (refusal.empty() ? std::string("none (no human fleet in orbit with a route)") : refusal));

    // Production: the cheapest allowed space unit on any planet the human may build on.
    const auto now = session.snapshot();
    const gc::ObservedProductionOffer* best = nullptr;
    for (const auto& offer : now->production_offers()) {
        if (offer.player != player || offer.admission != gc::ProductionAdmission::allowed || !offer.price
            || offer.kind != gc::ProductionKind::unit || offer.domain != gc::ProductionDomain::space
            || offer.price->trunc_to_integer() < 100) continue; // a priced unit, not a one-credit information card
        if (!best || offer.price->raw() < best->price->raw()) best = &offer;
    }
    if (!best) {
        report.note("play-build", false, "no allowed space unit offer for the human player");
    } else {
        const auto planet = best->planet;
        const auto type = best->type;
        const auto domain = best->domain;
        const auto price = best->price->trunc_to_integer();
        const auto fleets_before = static_cast<std::size_t>(std::count_if(now->fleets().begin(), now->fleets().end(),
            [&](const auto& fleet) { return fleet.owner == player && fleet.orbit == planet; }));
        gc::ProductionCommand order;
        order.planet = planet;
        order.type = type;
        order.action = gc::ProductionAction::buy;
        order.domain = domain;
        const auto accepted = submit(order);
        if (!accepted) {
            report.note("play-build", false, "purchase refused: " + eawr::core::format_diagnostic(accepted.error()));
        } else {
            std::uint64_t ticks = 0;
            bool built = false;
            while (ticks < play_ticks && !built) {
                if (!advance()) break;
                ++ticks;
                const auto current = session.snapshot();
                const auto count = std::count_if(current->fleets().begin(), current->fleets().end(),
                    [&](const auto& fleet) { return fleet.owner == player && fleet.orbit == planet; });
                built = static_cast<std::size_t>(count) > fleets_before;
            }
            report.note("play-build", built, "type " + std::to_string(type) + " at planet " + std::to_string(planet) + " price "
                + std::to_string(price) + (built ? " produced after " : " not produced after ") + std::to_string(ticks) + " ticks"
                + (step_error.empty() ? std::string{} : "; step failed: " + step_error));
            if (!step_error.empty()) return;
        }
    }

    // Days: let the fiscal clock run three more cycles past the orders (bounded by the same step budget per cycle).
    const auto target_cycle = session.snapshot()->clock().fiscal_cycle + 3;
    for (std::uint64_t ticks = 0; ticks < play_ticks * 3 && session.snapshot()->clock().fiscal_cycle < target_cycle; ++ticks)
        if (!advance()) break;
    const auto end = session.snapshot();
    report.note("play-days", step_error.empty() && end->clock().fiscal_cycle >= start_cycle,
        "fiscal cycle " + std::to_string(start_cycle) + " -> " + std::to_string(end->clock().fiscal_cycle) + ", tick "
            + std::to_string(end->completed_tick()) + ", human credits " + std::to_string(start_credits) + " -> "
            + std::to_string(credits_of(*end)) + (step_error.empty() ? std::string{} : "; step failed: " + step_error));
}

} // namespace

int run_installed_campaign(const InstalledCampaignRequest& request) {
    static std::string trace_filter;
    trace_filter = request.perception_trace;
    struct TraceScope {
        ~TraceScope() { eawr::script::foc::galactic_ai::observe_perception_failures(nullptr); }
    } trace_scope;
    if (!trace_filter.empty()) eawr::script::foc::galactic_ai::observe_perception_failures(
        [](std::string_view equation, gc::PlayerId player, gc::PlanetId target, std::span<const std::string> tokens) {
            if (equation != trace_filter) return;
            std::osyncstream output(std::clog);
            output << "gc-perception-unavailable equation=" << equation << " player=" << player << " target=" << target << " lookup=";
            for (std::size_t i = 0; i < tokens.size(); ++i) output << (i ? "." : "") << tokens[i];
            output << '\n';
        });

    Report report;
    const auto finish = [&](const int code) {
        if (!request.report_path.empty() && !report.write(request.report_path)) {
            std::cerr << "could not write " << request.report_path << '\n';
            return 4;
        }
        return code;
    };
    const auto fail = [&](const std::string& stage, const eawr::core::Diagnostic& diagnostic) {
        report.note(stage, false, eawr::core::format_diagnostic(diagnostic));
        return finish(3);
    };

    const std::filesystem::path root(request.game_root);
    auto layers = request.mod_root.empty() ? std::vector<eawr::vfs::LayerRoot>{} : eawr::vfs::mod_chain_roots(request.mod_root);
    layers.emplace_back("expansion", root / "corruption" / "Data");
    layers.emplace_back("base", root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(layers);
    if (!chain) return fail("mount", chain.error());
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& manifest : chain.value()) mounts.push_back(std::move(manifest.mount));
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    if (!filesystem) return fail("mount", filesystem.error());
    report.note("mount", true, std::to_string(mounts.size()) + " layers");

    const auto profile = request.mod_root.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake;
    const auto catalog = eawr::data::load_catalog(filesystem.value(), profile);
    if (!catalog) return fail("catalog", catalog.error());
    {
        std::map<std::string, std::size_t> by_code;
        for (const auto& diagnostic : catalog.value().diagnostics) ++by_code[diagnostic.code];
        std::ostringstream detail;
        detail << catalog.value().diagnostics.size() << " diagnostics";
        for (const auto& [code, count] : by_code) detail << ' ' << code << '=' << count;
        report.note("catalog", true, detail.str());
    }

    const auto data = eawr::data::galactic::load_galactic_data(filesystem.value(), catalog.value().catalog);
    if (!data) return fail("galactic-data", data.error());
    {
        std::map<std::string, std::size_t> families;
        for (const auto& issue : data.value().issues) {
            ++families[std::string(eawr::data::galactic::to_string(issue.kind)) + ' ' + issue.tag];
        }
        std::ostringstream facts;
        facts << '{';
        bool first = true;
        for (const auto& [family, count] : families) {
            facts << (first ? "" : ", ") << quoted(family) << ": " << count;
            first = false;
        }
        facts << '}';
        report.fact("issue_families", facts.str());
        std::ostringstream rows;
        rows << '[';
        for (std::size_t i = 0; i < data.value().issues.size(); ++i) {
            const auto& issue = data.value().issues[i];
            rows << (i ? ",\n      " : "\n      ") << "{\"kind\": " << quoted(std::string(eawr::data::galactic::to_string(issue.kind)))
                 << ", \"file\": " << quoted(issue.file) << ", \"line\": " << issue.line << ", \"owner\": " << quoted(issue.owner)
                 << ", \"tag\": " << quoted(issue.tag) << ", \"value\": " << quoted(issue.value)
                 << ", \"reason\": " << quoted(issue.reason) << '}';
        }
        rows << "\n    ]";
        report.fact("issues", rows.str());
        for (const auto& [family, count] : families) std::cout << "       issue " << family << " x" << count << '\n';
        std::ostringstream detail;
        detail << data.value().campaigns.size() << " campaigns, " << data.value().planets.size() << " planets, "
               << data.value().trade_routes.size() << " routes, " << data.value().factions.size() << " factions, "
               << data.value().issues.size() << " issues";
        report.note("galactic-data", true, detail.str());
    }

    const auto* source = data.value().campaign(request.campaign);
    if (source == nullptr) {
        report.note("campaign", false, "no campaign named \"" + request.campaign + "\" in the loaded data");
        return finish(3);
    }
    {
        std::ostringstream detail;
        detail << source->locations.size() << " locations, " << source->planets.size() << " planets, "
               << source->trade_routes.size() << " routes, " << source->starting_forces.size() << " starting forces";
        report.note("campaign", true, detail.str());
    }

    const auto referenced = cp::campaign_referenced_types(data.value(), request.campaign);
    const auto types = eawr::units::galactic_type_table(catalog.value().catalog, false, referenced);
    {
        std::map<std::string, std::size_t> families;
        std::map<std::string, std::string> samples;
        for (const auto& issue : types.issues) {
            const auto family = std::string(eawr::data::galactic::to_string(issue.kind)) + ' ' + issue.tag;
            if (++families[family] == 1) samples.emplace(family, issue.owner + ": " + issue.reason);
        }
        std::ostringstream facts;
        facts << '{';
        bool first = true;
        for (const auto& [family, count] : families) {
            facts << (first ? "" : ", ") << quoted(family) << ": " << count;
            first = false;
        }
        facts << '}';
        report.fact("type_issue_families", facts.str());
        for (const auto& [family, count] : families)
            std::cout << "       type issue " << family << " x" << count << " (first: " << samples[family] << ")\n";
    }
    report.note("types", true, std::to_string(types.types.size()) + " galactic types");

    cp::CampaignSetupOptions options;
    options.seed = request.seed;
    for (const auto& faction : data.value().factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool local = faction.name == source->starting_active_player;
        if (local) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) {
            options.seats.push_back({gc::SeatId{id}, id, local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true,
                local ? id : 0});
        }
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});

    const std::string difficulty = "Normal_Default";
    if (request.live_ticks) {
        // The viewer's own start path: prepared campaign with the native AI, run by the live session in driven mode.
        const auto owner = std::make_shared<const eawr::vfs::Vfs>(std::move(filesystem).value());
        auto prepared = eawr::platform::PreparedLiveCampaign::create(
            owner, catalog.value().catalog, data.value(), types, request.campaign, options, difficulty, request.live_ai,
            std::array<std::uint64_t, 6>{300, 1, 3, 3, 300, 1}, request.goals_every != 0 || eawr::diagnostics::GcMemory::enabled());
        if (!prepared) return fail("live-prepare", prepared.error());
        std::ostringstream ai;
        for (const auto& installed : prepared.value()->native_ai_players()) ai << ' ' << installed.player << '=' << installed.player_type;
        report.note("live-prepare", true, std::string("native AI ") + (prepared.value()->native_ai_installed() ? "installed for" : "not installed;") + ai.str());
        {
            const auto& notes = prepared.value()->native_ai_diagnostics();
            std::ostringstream lines;
            lines << '[';
            for (std::size_t i = 0; i < notes.size(); ++i) lines << (i ? ",\n      " : "\n      ") << quoted(notes[i]);
            lines << "\n    ]";
            report.fact("live_prepare_diagnostics", lines.str());
            report.note("live-prepare-diagnostics", true, std::to_string(notes.size()) + " AI load diagnostics");
        }
        {
            // RGC-172/173 (walk E, Q09): every Story_Name pair resolves when the roster holds every loaded faction.
            auto plots = cp::load_story_plots(*owner, data.value(), prepared.value()->plan());
            if (!plots) return fail("story-plots", plots.error());
            std::size_t dropped = 0;
            std::ostringstream lines;
            lines << '[';
            for (const auto& note : plots.value().notes) {
                if (note.find("WST-88") != std::string::npos) ++dropped;
                lines << (&note == plots.value().notes.data() ? "\n      " : ",\n      ") << quoted(note);
            }
            lines << "\n    ]";
            report.fact("story_plot_notes", lines.str());
            std::size_t scripted = 0;
            for (const auto& sub : plots.value().subplots) if (!sub.script.empty()) ++scripted;
            report.note("story-plots", dropped == 0, std::to_string(source->story_names.size()) + " Story_Name pairs listed, "
                + std::to_string(plots.value().plots.size()) + " plots loaded, " + std::to_string(plots.value().subplots.size())
                + " sub-plots (" + std::to_string(scripted) + " with a Lua_Script), " + std::to_string(dropped) + " pair traversals ended early (WST-88)");
        }
        const auto before = prepared.value()->snapshot();
        const auto goal_reader = prepared.value()->native_ai_goal_reader();
        eawr::platform::LiveGameSession::Options live_options;
        live_options.workers = request.workers;
        live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
        // WST-53: what the sub-plot scripts ran into, once per distinct fault (the tick is dropped from the text).
        std::mutex script_mutex;
        std::map<std::string, std::size_t> script_faults;
        live_options.tick_diagnostics = [&](const std::span<const eawr::core::Diagnostic> rows) {
            const std::lock_guard lock(script_mutex);
            for (const auto& row : rows) {
                if (row.message.rfind("story script ", 0) != 0) continue;
                auto text = row.code + ": " + row.message;
                const auto at = text.find(" at tick ");
                const auto colon = at == std::string::npos ? std::string::npos : text.find(": ", at);
                if (colon != std::string::npos) text.erase(at, colon - at);
                ++script_faults[text];
            }
        };
        auto live = eawr::platform::LiveGameSession::start(std::move(prepared).value(), live_options);
        if (!live) return fail("live-start", live.error());
        const auto memory_checkpoint = [&](std::uint64_t tick) {
            if (!eawr::diagnostics::GcMemory::enabled()) return;
            constexpr std::array<std::uint64_t, 6> ticks{0, 600, 1800, 3600, 7200, 14400};
            if (std::find(ticks.begin(), ticks.end(), tick) == ticks.end()) return;
            const std::filesystem::path directory(eawr::diagnostics::GcMemory::directory());
            std::filesystem::create_directories(directory);
            const auto stem = std::to_string(tick);
            const auto pending = directory / (stem + ".pending");
            {
                eawr::diagnostics::GcMemory memory;
                memory.row("shared.cli.catalog_definitions", catalog.value().catalog.definitions());
                memory.row("shared.cli.galactic_types", types.types);
                std::ofstream output(pending);
                output << "{\"tick\":" << tick << ",\"cli\":";
                memory.write(output);
                output << ",\"native\":";
                if (goal_reader) {
                    const auto rows = goal_reader();
                    output << (rows.empty() ? "null" : rows.front());
                } else output << "null";
                output << '}';
                output.flush();
                if (!output) throw std::runtime_error("memory checkpoint write failed");
            }
            std::filesystem::rename(pending, directory / (stem + ".json"));
            // The external sampler observes the exact stopped tick, then releases
            // it. The bounded handshake is diagnostic only, never simulation state.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            while (!std::filesystem::exists(directory / (stem + ".release"))) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("memory sampler handshake timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        memory_checkpoint(0);
        // Advance in short chunks and note every fleet seen travelling, so the report shows movement the final frame no longer has.
        std::set<gc::FleetId> ever_travelling;
        std::map<gc::PlayerId, std::size_t> travelling_by_owner;
        bool reached = true, timed_out = false;
        const auto timeout = std::chrono::minutes(30);
        constexpr std::uint64_t sample_ticks = 60;
        const auto next_sample = [&](std::uint64_t current) {
            auto next = std::min(current + sample_ticks, request.live_ticks);
            if (request.goals_every) next = std::min(next, (current / request.goals_every + 1) * request.goals_every);
            return next;
        };
        for (std::uint64_t target = next_sample(0); reached; target = next_sample(target)) {
            live.value()->advance_to(target);
            const auto waiting_since = std::chrono::steady_clock::now();
            reached = live.value()->wait_for(target, timeout);
            timed_out = std::chrono::steady_clock::now() - waiting_since >= timeout;
            if (!reached) break;
            memory_checkpoint(target);
            if (const auto sampled = live.value()->frame(); sampled.latest)
                for (const auto& fleet : sampled.latest->fleets())
                    if (fleet.motion.transit && ever_travelling.insert(fleet.id).second) ++travelling_by_owner[fleet.owner];
            if (!eawr::diagnostics::GcMemory::enabled() && goal_reader && request.goals_every && (target % request.goals_every == 0 || target >= request.live_ticks))
                for (const auto& line : goal_reader()) report.note("live-goals", true, "tick " + std::to_string(target) + " " + line);
            if (target >= request.live_ticks) break;
        }
        // Join before reading faults so every callback is finished on failure and timeout too.
        live.value()->stop();
        const auto frame = live.value()->frame();
        const auto failure = live.value()->failure();
        {
            const std::lock_guard lock(script_mutex);
            report.story_scripts(script_faults);
        }
        if (failure || !reached || !frame.latest) {
            report.live_failure(failure, reached, timed_out, request.live_ticks, live.value()->completed_tick());
            return finish(3);
        }
        std::size_t moved_fleets = 0, new_fleets = 0, flipped_planets = 0;
        std::map<gc::FleetId, const gc::Fleet*> old_fleets;
        for (const auto& fleet : before->fleets()) old_fleets.emplace(fleet.id, &fleet);
        for (const auto& fleet : frame.latest->fleets()) {
            const auto old = old_fleets.find(fleet.id);
            if (old == old_fleets.end()) ++new_fleets;
            else if (old->second->orbit != fleet.orbit || fleet.motion.transit) ++moved_fleets;
        }
        std::map<gc::PlanetId, gc::PlayerId> old_owner;
        for (const auto& planet : before->planets()) old_owner.emplace(planet.id, planet.owner);
        for (const auto& planet : frame.latest->planets())
            if (old_owner.contains(planet.id) && old_owner.at(planet.id) != planet.owner) ++flipped_planets;
        std::ostringstream detail;
        detail << "tick " << frame.latest->completed_tick() << ", fiscal cycle " << frame.latest->clock().fiscal_cycle << ", "
               << moved_fleets << " fleets moved or travelling, " << new_fleets << " new fleets, " << flipped_planets
               << " planets changed owner, pending conflict " << (frame.pending_conflict ? "yes" : "no") << "; "
               << ever_travelling.size() << " distinct fleets were seen travelling (by owner:";
        for (const auto& [travelling_owner, count] : travelling_by_owner) detail << ' ' << travelling_owner << '=' << count;
        detail << ')';
        report.note("live-run", true, detail.str());
        return finish(0);
    }
    auto planned =cp::plan_live_campaign_setup(filesystem.value(), data.value(), types, request.campaign, options, difficulty);
    if (!planned) return fail("plan", planned.error());
    report.note("plan", true, "campaign setup plan built");
    diagnostics_fact(report, "plan", planned.value().diagnostics());
    const auto services = cp::prepare_live_campaign_services(
        filesystem.value(), catalog.value().catalog, data.value(), types, planned.value(), difficulty);
    if (!services) return fail("services", services.error());
    report.note("services", true, "stock providers prepared");
    diagnostics_fact(report, "services", services.value().load_diagnostics);
    const auto campaign_root = cp::CampaignRoot::create(planned.value(), services.value());
    if (!campaign_root) return fail("placement", campaign_root.error());
    const auto& setup = campaign_root.value().game()->galaxy().setup();
    {
        std::size_t members = 0;
        for (const auto& fleet : setup.fleets) members += fleet.members.size();
        std::ostringstream detail;
        detail << setup.players.size() << " players, " << setup.planets.size() << " planets, " << setup.routes.size()
               << " routes, " << setup.fleets.size() << " fleets, " << members << " fleet members";
        report.note("placement", true, detail.str());
    }

    auto session = gc::GalacticSession::create(setup);
    if (!session) return fail("session", session.error());
    report.note("session", true, "tick-zero hash " + session.value().state_sha256());
    const eawr::platform::ThreadWorkerAdapter executor(request.workers);
    for (std::uint64_t tick = 0; tick < request.ticks; ++tick) {
        const auto step = session.value().step(executor);
        if (!step) return fail("step " + std::to_string(tick + 1), step.error());
    }
    report.note("ticks", true, std::to_string(request.ticks) + " ticks, final hash " + session.value().state_sha256());
    if (request.play_ticks) gameplay(report, session.value(), executor, request.play_ticks);
    return finish(0);
}

} // namespace sim_headless::cli
