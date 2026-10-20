#include <algorithm>
#include <cstdint>
#include <exception>
#include <ios>
#include <map>
#include <memory>
#include <ostream>
#include <vector>
#include <cstddef>
#include <string>
#include "../support/env.hpp"
#include "../support/result.hpp"
// #4058: an AI-on galactic save only grows by its recording. Runs the stock Sandbox_Equal_Footing_Empire campaign
// (idle human, native AI on the other seats), measures the checkpoint section sizes as the days go by, and proves
// that a direct restore (which never reads the journal) continues to the same state hashes.
#include "galactic_support.hpp"
#include "eawr/campaign/galactic_ai_services.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "ai_campaign_gate.hpp"
#include "../../src/campaign/galactic_ai_usability.hpp"
#include "eawr/campaign/live_conflict.hpp"
#include "eawr/campaign/space_entry.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/data/retail_values.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
using eawr::testing::must;

namespace {
namespace cp = eawr::campaign;
namespace ck = eawr::checkpoint;
namespace gc = eawr::sim::galactic;
using namespace galactic_test;

std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    return std::filesystem::path(std::move(*root));
}

struct Sample {
    std::uint64_t tick{};
    std::size_t total{};
    std::map<std::string, std::size_t> sections;
    std::map<std::string, std::size_t> ai_components;
    std::size_t effects{}, commands{}, journal_bytes{}, payments{}, odd_payments{};
};
// A production bill journals only its outcome: build u64, accepted u8, payer u32, charge i64.
constexpr std::size_t payment_payload_bytes = 8 + 1 + 4 + 8;

// Regression budgets for this stock idle-human fixture, not runtime save caps.
// Earlier compact saves grew about 205 bytes/tick with only 6 KiB of owner
// growth across 10,000 ticks. Allow service-state variation within each interval.
bool growth_is_bounded(std::int64_t owner, std::int64_t total, std::int64_t journal, std::uint64_t ticks) {
    return owner < 64 * 1024
        && total <= static_cast<std::int64_t>(ticks) * 512 + 128 * 1024
        && total <= journal + journal / 4 + 128 * 1024;
}

Sample measure(const cp::GameSession& game, const ck::Identities& identities, const std::filesystem::path& keep) {
    const auto bytes = must(ck::save_game_checkpoint(game, identities));
    const auto parsed = must(ck::parse_checkpoint(bytes));
    Sample row; row.tick = game.completed_tick(); row.total = bytes.size();
    for (const auto& section : parsed.sections) row.sections[std::string(ck::section_name(section.id))] = section.bytes.size();
    if (const auto* ai = parsed.find(ck::SectionId::galactic_ai)) {
        const auto saved = must(cp::decode_galactic_ai_save(ai->bytes));
        if (saved.state) for (const auto& [name, data] : saved.state->components) row.ai_components[name] = data.size();
    }
    const auto record = game.galaxy().record();
    row.effects = record.native_effects.size();
    for (const auto& effect : record.native_effects) {
        row.journal_bytes += 23 + effect.payload.size();
        if (effect.kind != gc::NativeEffectKind::production_payment) continue;
        ++row.payments; row.odd_payments += effect.payload.size() != payment_payload_bytes;
    }
    row.commands = record.commands.size();
    if (!keep.empty()) {
        std::ofstream out(keep / ("tick" + std::to_string(row.tick) + ".eawrsave"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return row;
}

struct Options {
    std::uint64_t ticks = 3000, interval = 1000, restore_at = 0, continue_for = 60, cycle_at = 0, cycle_ticks = 200;
    std::size_t workers = 4;
    std::filesystem::path keep;
};

int run(const Options& wanted) {
    galactic_test::expect(growth_is_bounded(1024, 2048, 0, 1000),
        "an interval without journal entries permits bounded service-state growth");
    galactic_test::expect(!growth_is_bounded(64 * 1024, 2048, 0, 1000),
        "absolute owner growth budget applies without journal entries");
    galactic_test::expect(!growth_is_bounded(0, 700 * 1024, 700 * 1024, 1000),
        "absolute total growth budget applies even when the recording explains all growth");
    const std::string_view campaign_name = "Sandbox_Equal_Footing_Empire";
    const auto root = game_root();
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto files = must(eawr::vfs::Vfs::mount(mounts));
    const auto loaded = must(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    const auto* source = data.campaign(campaign_name);
    galactic_campaign_gate::check(source != nullptr, "day campaign exists");
    cp::CampaignSetupOptions options; options.forced_seed = 1;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        const bool human = faction.name == source->starting_active_player;
        options.players.push_back({{id, id, 0}, faction.name, true});
        options.seats.push_back({gc::SeatId{id}, id, human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
        if (human) options.local_observer = id;
    }
    std::vector<std::string> originals; for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) options.types.push_back({options.types.size() + 1, name});
    const auto plan = must(cp::plan_live_campaign_setup(files, data, types, campaign_name, options, "Normal_Default"));
    const auto* fog = data.constant("AIUsesFogOfWarGalactic");
    galactic_campaign_gate::check(fog, "stock AI fog policy exists");
    const auto setup = must(cp::prepare_stock_galactic_ai_runtime(files, loaded.catalog, data, types, plan, {},
        std::make_shared<eawr::platform::ThreadWorkerAdapter>(wanted.workers),
        eawr::data::retail::boolean(fog->text).value(), gc::galactic_max_objects, gc::galactic_max_objects, "Normal_Default"));
    auto observed = std::make_shared<galactic_campaign_gate::Observations>();
    for (const auto& player : setup->lifecycle.players) observed->players.emplace(player.player, galactic_campaign_gate::PlayerProgress{});
    const auto services = must(cp::prepare_live_campaign_services(files, loaded.catalog, data, types, plan, "Normal_Default"));
    auto root_owner = must(cp::CampaignRoot::create(plan, services));
    const auto space = must(cp::prepare_stock_space_entry(files, loaded.catalog, data, types, plan));
    const auto usability = must(cp::prepare_stock_galactic_ai_usability(data, plan, setup->perception));
    cp::GalacticAiStockQueryFactory queries = [usability] {
        auto provider = std::make_shared<cp::GalacticAiUsabilityProvider>(usability);
        cp::GalacticAiStockQueries out;
        out.usability = [provider](auto player, const auto& snapshot, auto planet) { return provider->usability(player, snapshot, planet); };
        out.connectivity_publication = [provider](const auto& parent, auto& world, auto& engine, const auto& pool) { return provider->connectivity_publication(parent, world, engine, pool); };
        out.movement = [provider](auto player, const auto& snapshot, const auto& world, const auto& engine, const auto& pool) { return provider->movement(player, snapshot, world, engine, pool); };
        out.external.emplace("GALACTIC_USABILITY", std::move(provider));
        return eawr::core::Result<cp::GalacticAiStockQueries>::success(std::move(out));
    };
    auto factory = must(cp::stock_galactic_ai_providers(setup, services.stock_starting_forces, space, std::move(queries),
        [observed](auto view) { observed->runtimes.push_back(std::move(view)); },
        [observed](const auto& notice) { observed->battle(notice); }));
    eawr::platform::CampaignServiceConfiguration configuration;
    configuration.configure = [&](auto& game) { return cp::configure_live_campaign_conflicts(game, files, loaded.catalog, data, types, plan, space); };
    configuration.restore.configure_session = configuration.configure;
    configuration.restore.content = [space](const auto& conflict) { return space->content(conflict); };
    must(cp::configure_galactic_ai_runtime(configuration, setup, std::move(factory)));
    must(configuration.configure(*root_owner.game()));
    must(configuration.configure_native_ai(*root_owner.game()));
    auto& game = *root_owner.game();
    const auto identities = ck::game_identities(game.galaxy().setup());

    gc::SeatId human_seat{};
    for (const auto& seat : options.seats) if (seat.kind == gc::SeatKind::human) human_seat = seat.seat_id;
    std::vector<Sample> samples;
    for (std::uint64_t tick = 1; tick <= wanted.ticks; ++tick) {
        (void)must(game.step(*setup->executor));
        if (wanted.interval && tick % wanted.interval == 0) {
            const auto row = measure(game, identities, wanted.keep);
            std::cout << "SAVE_GROWTH tick=" << row.tick << " total=" << row.total << " effects=" << row.effects << " payments=" << row.payments
                << " journal=" << row.journal_bytes << " commands=" << row.commands;
            for (const auto& [name, size] : row.sections) std::cout << ' ' << name << '=' << size;
            for (const auto& [name, size] : row.ai_components) std::cout << " ai." << name << '=' << size;
            std::cout << std::endl;
            galactic_test::expect(row.odd_payments == 0, "every production bill journals only its compact outcome");
            samples.push_back(row);
        }
        if (wanted.cycle_at && tick == wanted.cycle_at) {
            // #4058: save sizes across save/load cycles. Each cycle asks for a save the way a player does (a SaveRequest
            // from the human seat), writes it, loads it, and plays on; the history in the next save starts there.
            // The baseline is the unbounded save at this tick (the history since the campaign began).
            const auto baseline = must(ck::save_game_checkpoint(game, identities));
            const auto baseline_session = must(ck::parse_checkpoint(baseline)).find(ck::SectionId::session)->bytes.size();
            std::cout << "SAVE_CYCLES baseline tick=" << game.completed_tick() << " bytes=" << baseline.size()
                << " session=" << baseline_session << std::endl;
            std::size_t last_session = 0, last_total = 0;
            cp::GameSession* active = &game;
            std::optional<ck::GameRestore> holder;
            std::uint64_t sequence = 0;
            for (int cycle = 1; cycle <= 3; ++cycle) {
                must(active->submit({{active->completed_tick(), human_seat, ++sequence}, gc::SaveRequest{cycle, false, false, false}}));
                (void)must(active->step(*setup->executor));
                const auto saved = must(ck::save_game_checkpoint(*active, identities));
                const auto parsed = must(ck::parse_checkpoint(saved));
                std::cout << "SAVE_CYCLES cycle=" << cycle << " tick=" << active->completed_tick() << " bytes=" << saved.size()
                    << " session=" << parsed.find(ck::SectionId::session)->bytes.size()
                    << " conflicts=" << parsed.find(ck::SectionId::conflicts)->bytes.size()
                    << " owner=" << parsed.find(ck::SectionId::galactic_owner)->bytes.size() << std::endl;
                last_session = parsed.find(ck::SectionId::session)->bytes.size(); last_total = saved.size();
                auto restored = must(ck::load_game_checkpoint(saved, identities, configuration.restore, *setup->executor));
                galactic_test::expect(must(ck::save_game_checkpoint(restored.session, identities)) == saved, "a load recaptures its save before it steps");
                holder.emplace(std::move(restored)); active = &holder->session;
                for (std::uint64_t i = 0; i < wanted.cycle_ticks; ++i) (void)must(active->step(*setup->executor));
            }
            // Each later save holds only the play since the last one, so it is smaller than the whole-campaign save.
            galactic_test::expect(last_session < baseline_session && last_total < baseline.size(),
                "a save after two save/load cycles is smaller than the unbounded save at its start");
            break;
        }
        if (tick != wanted.restore_at) continue;
        // Direct restore adopts the saved owner without replaying a tick or reading the journal as input.
        const auto saved = must(ck::save_game_checkpoint(game, identities));
        auto restored = must(ck::load_game_checkpoint(saved, identities, configuration.restore, *setup->executor));
        galactic_test::expect(restored.work.resimulated_ticks == 0, "restore imports the owner without simulating ticks");
        galactic_test::expect(must(ck::save_game_checkpoint(restored.session, identities)) == saved, "restore recaptures the identical checkpoint");
        for (std::uint64_t i = 0; i < wanted.continue_for; ++i) {
            const auto expected = must(game.step(*setup->executor)).state_hash.get();
            const auto got = must(restored.session.step(*setup->executor)).state_hash.get();
            if (expected != got) { galactic_test::expect(false, "restored session continues to the control's state hash at every tick"); break; }
        }
        // The restored session dropped the journal it was loaded with (#4058); what it journals after must match the control's.
        const auto control_effects = game.galaxy().native_effects();
        const auto dropped = restored.session.galaxy().native_dropped();
        const auto kept = restored.session.galaxy().native_effects();
        galactic_test::expect(dropped <= control_effects.size() && kept.size() == control_effects.size() - dropped
            && std::equal(kept.begin(), kept.end(), control_effects.begin() + static_cast<std::ptrdiff_t>(dropped)),
            "restored session journals the same effects as the control after continuing");
        tick += wanted.continue_for; // the control stepped ahead
        std::cout << "SAVE_GROWTH restore at=" << wanted.restore_at << " continued=" << wanted.continue_for << " workers=" << wanted.workers << std::endl;
    }
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto& before = samples[i - 1]; const auto& after = samples[i];
        const auto grown = [&](const Sample& x, const Sample& y, const char* section) {
            return static_cast<std::int64_t>(y.sections.at(section)) - static_cast<std::int64_t>(x.sections.at(section)); };
        const auto journal = static_cast<std::int64_t>(after.journal_bytes) - static_cast<std::int64_t>(before.journal_bytes);
        const auto total = static_cast<std::int64_t>(after.total) - static_cast<std::int64_t>(before.total);
        galactic_test::expect(growth_is_bounded(grown(before, after, "galactic-owner"), total, journal, after.tick - before.tick),
            "owner growth is below 64 KiB/interval; total growth is bounded per tick and by the recording");
    }
    return 0;
}
int main_run(int argc, char** argv) {
    std::cout << "CTEST_FULL_OUTPUT" << std::endl;
    Options wanted;
    if (argc > 1) wanted.ticks = std::stoull(argv[1]);
    if (argc > 2) wanted.interval = std::stoull(argv[2]);
    if (argc > 3) wanted.workers = std::stoull(argv[3]);
    if (argc > 4) wanted.restore_at = std::stoull(argv[4]);
    const bool observe = argc > 5 && std::string_view(argv[5]) == "observe";
    if (argc > 6 && std::string_view(argv[6]) != "-") wanted.keep = argv[6];
    if (argc > 7) wanted.cycle_at = std::stoull(argv[7]);
    if (argc > 8) wanted.cycle_ticks = std::stoull(argv[8]);
    run(wanted);
    // "observe": deliberately nonzero so ctest prints the sizes of an otherwise passing run (observation only).
    if (observe) return 1;
    return galactic_test::failures == 0 ? 0 : 1;
}
} // namespace
int case_galactic_ai_save_growth_tests(int argc, char** argv) { try { return main_run(argc, argv); } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
