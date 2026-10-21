#include "stock_snapshot.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
void take(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
}
std::string game_root() {
#if defined(_MSC_VER)
    char* text{}; std::size_t size{};
    if (_dupenv_s(&text, &size, "EAWR_EAW_GAME_ROOT") || !text) return {};
    std::string result(text); std::free(text); return result;
#else
    const auto* text = std::getenv("EAWR_EAW_GAME_ROOT");
    return text ? text : "";
#endif
}
// Every admitted economy-page input, before rounding, including known-empty and unknown.
std::string page_facts(const gc::GalacticSnapshot& snapshot) {
    std::ostringstream out;
    const auto* forecast = snapshot.economy_forecast();
    if (!forecast) return "unknown";
    const auto optional = [&](const auto& value) {
        if (value) out << value->raw(); else out << '?';
        out << ',';
    };
    out << forecast->player << ',' << forecast->base.raw() << ',' << forecast->bonus.raw()
        << ',' << forecast->gross_total.raw() << ',';
    optional(forecast->adjusted_payment); optional(forecast->trade_routes);
    optional(forecast->alignment); optional(forecast->political); optional(forecast->corruption);
    for (const auto& planet : forecast->planets) {
        out << ';' << planet.planet << ',' << planet.basic.raw() << ',';
        optional(planet.actual); optional(planet.trade);
        if (!planet.modifiers) out << '?';
        else for (const auto& modifier : *planet.modifiers)
            out << '[' << modifier.source_type << ',' << modifier.income.raw() << ']';
    }
    for (const auto& planet : snapshot.planets()) out << ";owner," << planet.id << ',' << planet.owner;
    return out.str();
}
} // namespace

int stock_snapshot_benchmark(const std::span<const std::size_t> workers,
    const std::uint64_t samples, const std::uint64_t warmup) {
    const auto root_path = game_root();
    if (root_path.empty()) { std::cout << "SKIPPED: stock snapshot requires EAWR_EAW_GAME_ROOT\n"; return 77; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id,
            std::filesystem::path(root_path) / folder / "Data")).mount);
    const auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(filesystem, catalog.catalog));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    constexpr auto name = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(name);
    if (!source) throw std::runtime_error("stock snapshot campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool local = faction.name == source->starting_active_player;
        if (local) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, local ? id : 0});
    }
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& original : originals) options.types.push_back({options.types.size() + 1, original});
    const auto planned = take(cp::plan_live_campaign_setup(filesystem, data, types, name, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(filesystem, catalog.catalog,
        data, types, planned, "Normal_Default"));
    const auto root = take(cp::CampaignRoot::create(planned, services));
    const auto& setup = root.game()->galaxy().setup();
    std::vector<std::string> reference;
    std::cout << "stock_snapshot,campaign,workers,samples,warmup,median_us,page_facts_sha256,state_hash\n";
    for (const auto count : workers) {
        auto session = take(gc::GalacticSession::create(setup));
        const eawr::platform::ThreadWorkerAdapter pool(count);
        std::vector<std::string> evidence;
        evidence.push_back(page_facts(*session.snapshot()) + ':' + session.state_sha256());
        for (std::uint64_t tick = 0; tick < warmup; ++tick) {
            const auto step = take(session.step(pool));
            evidence.push_back(page_facts(*step.snapshot) + ':' + step.state_hash.get());
        }
        const auto facts = page_facts(*session.snapshot());
        const auto hash = session.state_sha256();
        std::vector<double> times; times.reserve(static_cast<std::size_t>(samples));
        // Empty control barriers publish snapshots without advancing or servicing the galaxy.
        // Thus only publication is timed; worker counts apply to the stock warmup above.
        for (std::uint64_t sample = 0; sample < samples; ++sample) {
            const auto start = std::chrono::steady_clock::now();
            take(session.apply_session_controls({}));
            times.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
            if (page_facts(*session.snapshot()) != facts)
                throw std::runtime_error("stock page facts changed at a publication-only barrier");
        }
        if (session.state_sha256() != hash) throw std::runtime_error("publication mutated stock state");
        if (reference.empty()) reference = evidence;
        else if (evidence != reference) throw std::runtime_error("stock page/state worker disagreement");
        std::sort(times.begin(), times.end());
        const auto middle = times.size() / 2;
        const auto median = times.size() % 2 ? times[middle] : (times[middle - 1] + times[middle]) / 2;
        std::cout << std::fixed << std::setprecision(3) << "stock_snapshot," << name << ',' << count
            << ',' << samples << ',' << warmup << ',' << median << ','
            << eawr::core::sha256_hex(std::span(
                reinterpret_cast<const std::uint8_t*>(facts.data()), facts.size())) << ',' << hash << '\n';
        std::cout << "stock_page_facts," << count << ',' << facts << '\n';
    }
    return 0;
}
