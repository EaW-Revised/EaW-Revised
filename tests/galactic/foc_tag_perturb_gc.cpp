#include "foc_tag_perturb_support.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/data/galactic/galactic_data.hpp"

namespace foc_tag_perturb_test_support {
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
using eawr::core::Result;
using eawr::sim::math::Fixed;

// Explicit test representation, shared with the installed campaign setup contract:
// one fleet/member per starting-force request. No ground placement, AI, production,
// income, political-control provider, story or ability activation is fabricated.
cp::CampaignSetupServices sandbox_services() {
    cp::CampaignSetupServices s;
    s.location = [](const auto&) { return Result<void>::success(); };
    s.mark_initial_placement = [](const auto&) { return Result<void>::success(); };
    s.place = [](const cp::StartingForceRequest& r, const auto next, const auto&) {
        return Result<cp::StartingForcePlacement>::success(
            {{{next, r.owner, r.planet, {{next + 1, r.type}}}}, {}, next + 2, true, {}, {}, {}, {}, {}, 0, {}, {}, false, {}, 0});
    };
    s.unify_human_guard_fleets = [](const auto, const auto, const auto& setup) {
        return Result<std::vector<gc::Fleet>>::success(setup.fleets);
    };
    s.political_control = [](const auto& p, auto, const auto&) { return Result<gc::PlayerId>::success(p.owner); };
    s.credit_cap = [](const auto&, const auto&) { return Result<Fixed>::success(Fixed{}); };
    s.home = [](const auto&) { return Result<void>::success(); };
    s.assign_tech = [](const auto& r, const auto&) { return Result<std::int64_t>::success(r.requested_current); };
    return s;
}

cp::CampaignSetupOptions bindings(const dg::GalacticData& loaded, const units::GalacticTypeTable& types,
    const std::uint64_t seed) {
    cp::CampaignSetupOptions o;
    o.seed = seed;
    for (const auto& faction : loaded.factions) {
        const auto id = static_cast<gc::PlayerId>(o.players.size() + 1);
        o.players.push_back({{id, id, 0}, faction.name, true});
        if (!faction.is_neutral.value_or(false)) o.seats.push_back({gc::SeatId{id}, id, gc::SeatKind::ai, id - 1, true, 0});
    }
    for (const auto& type : types.types) o.types.push_back({o.types.size() + 1, type.name});
    return o;
}

Result<gc::GalacticSetup> setup(const dg::GalacticData& loaded, const units::GalacticTypeTable& types,
    const cp::CampaignSetupOptions& options, const std::string& name) {
    auto planned = cp::plan_campaign_setup(loaded, types, name, options);
    if (!planned) return Result<gc::GalacticSetup>::failure(planned.error());
    auto root = cp::CampaignRoot::create(std::move(planned).value(), sandbox_services());
    if (!root) return Result<gc::GalacticSetup>::failure(root.error());
    auto value = root.value().game()->galaxy().setup();
    // Content receipts change on every overlay, including unused tags. They are
    // metadata, not behavioral evidence. Clear BOTH identities for all runs.
    value.content_identity = {};
    value.campaign_identity = {};
    return Result<gc::GalacticSetup>::success(std::move(value));
}

std::string facts(const gc::GalacticSnapshot& s) {
    std::ostringstream out;
    out << "{\"frame\":" << s.clock().galactic_frame << ",\"cycle\":" << s.clock().fiscal_cycle
        << ",\"planets\":[";
    bool first = true;
    for (const auto& p : s.planets()) {
        out << (first ? "" : ",") << '[' << p.id << ',' << p.owner << ',' << p.position.x.raw()
            << ',' << p.position.y.raw() << ',' << p.position.z.raw() << ']'; first = false;
    }
    out << "],\"routes\":["; first = true;
    for (const auto& r : s.routes()) {
        out << (first ? "" : ",") << '[' << r.id << ',' << r.from << ',' << r.to << ',' << r.hs_speed_factor.raw() << ']'; first = false;
    }
    out << "],\"fleets\":["; first = true;
    for (const auto& f : s.fleets()) {
        out << (first ? "" : ",") << '[' << f.id << ',' << f.owner << ',' << f.orbit << ",[";
        bool member = true;
        for (const auto& m : f.members) { out << (member ? "" : ",") << '[' << m.id << ',' << m.type << ']'; member = false; }
        out << "]]"; first = false;
    }
    out << "],\"economy\":["; first = true;
    if (s.player_economy()) for (const auto& p : s.player_economy()->players) {
        out << (first ? "" : ",") << '[' << p.player << ',' << p.credits.raw() << ',' << p.current_tech << ',' << p.maximum_tech << ']'; first = false;
    }
    out << "],\"snapshot_sha256\":" << json_string(s.sha256()) << '}';
    return out.str();
}

struct Trial {
    std::vector<std::string> hashes, snapshots;
    std::string final_facts;
    std::string error;
};
Trial run(const gc::GalacticSetup& value, const Options& o, Clock::time_point deadline, std::size_t workers) {
    Trial t;
    auto game = cp::GameSession::create(value);
    if (!game) { t.error = game.error().message; return t; }
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t frame = 0; frame <= o.ticks; ++frame) {
        if (Clock::now() >= deadline) { t.error = "total runtime budget reached"; return t; }
        if (frame != 0) {
            auto tick = game.value().step(executor);
            if (!tick) { t.error = tick.error().message; return t; }
        }
        t.hashes.push_back(game.value().state_sha256());
        const auto snapshot = game.value().galaxy().snapshot();
        t.snapshots.push_back(snapshot->sha256());
        if (frame == o.ticks) t.final_facts = facts(*snapshot);
    }
    return t;
}
struct StockBaseline { std::string name; Trial trial; std::optional<gc::GalacticSetup> initial; };

void check(Installation& installation, const Options& options, const dg::GalacticData& base,
    const cp::CampaignSetupOptions& options_bindings, const std::vector<StockBaseline>& baselines,
    Clock::time_point deadline, RowResult& result) {
    auto& row = result.row;
    if (row.tag.find('@') != std::string::npos) {
        result.verdict = "not reachable";
        result.reason = "attribute identity is outside the existing value-overlay API"; return;
    }
    std::vector<data::ValueOverride> overrides;
    std::vector<data::DocumentOverride> documents;
    std::vector<std::string> errors;
    const auto add = [&](const std::string& id, const Found& found, auto&& install) {
        if (found.values.empty()) { result.skipped.push_back(id); return; }
        auto changed = change_values(row.change, found.values);
        if (const auto* why = std::get_if<std::string>(&changed)) { errors.push_back(id + ": " + *why); return; }
        auto values = std::get<Changed>(std::move(changed)).values;
        if (values == found.values) return;
        result.objects.push_back({id, found.values, values, false, false});
        install(std::move(values));
    };
    if (row.type == "constants") {
        auto document = data::load_document(*installation.filesystem, constants_path);
        if (!document) { result.verdict = "not reachable"; result.reason = document.error().message; return; }
        add(std::string(constants_path), find_values(document.value().root, row.tag), [&](auto values) {
            documents.push_back({std::string(constants_path), row.tag, std::move(values)});
        });
    } else if (row.type == "route") {
        for (const auto& route : base.trade_routes) {
            auto document = data::load_document(*installation.filesystem, route.file);
            if (!document) { errors.push_back(document.error().message); continue; }
            std::size_t matches_count = 0;
            for (const auto& node : document.value().root.children) {
                bool matches = false;
                for (const auto& attr : node.attributes) matches |= iequals(attr.name, "Name") && iequals(attr.value, route.name);
                if (!iequals(node.name, "TradeRoute") || !matches) continue;
                ++matches_count;
                add(route.name, find_values(node, row.tag), [&](auto values) {
                    documents.push_back({route.file, "TradeRoute/" + row.tag, std::move(values), route.name});
                });
            }
            if (matches_count != 1) errors.push_back(route.name + ": Name selector matched "
                + std::to_string(matches_count) + " nodes; expected exactly one");
        }
    } else {
        for (const auto& definition : installation.catalog->catalog.definitions()) {
            if (!definition.active || !definition.namespace_winner || !iequals(definition.type_name, row.element)) continue;
            auto object = installation.catalog->catalog.resolve(definition.id, definition.category);
            if (!object) continue;
            add(definition.id, find_values(object.value(), row.tag), [&](auto values) {
                overrides.push_back({definition.id, row.tag, std::move(values)});
            });
        }
    }
    if (overrides.empty() && documents.empty()) {
        result.verdict = "not reachable";
        result.reason = errors.empty() ? "no authored value for this class/tag in stock data" : errors.front(); return;
    }
    if (!errors.empty()) {
        // Do not prove a whole class using only the objects auto could change.
        result.verdict = "not reachable"; result.reason = errors.front(); return;
    }
    const data::DocumentOverrides scope(documents);
    auto catalog = data::with_overrides(installation.catalog->catalog, overrides);
    if (!catalog) { result.verdict = "not reachable"; result.reason = catalog.error().message; return; }
    auto loaded = dg::load_galactic_data(*installation.filesystem, catalog.value());
    if (!loaded) { result.verdict = "not reachable"; result.reason = loaded.error().message; return; }
    for (const auto& issue : loaded.value().issues) {
        if (issue.kind != dg::IssueKind::bad_value && issue.kind != dg::IssueKind::missing_required
            && issue.kind != dg::IssueKind::dangling_reference) continue;
        const bool inherited = std::any_of(base.issues.begin(), base.issues.end(), [&](const auto& earlier) {
            return earlier.kind == issue.kind && earlier.owner == issue.owner && earlier.tag == issue.tag
                && earlier.value == issue.value && earlier.reason == issue.reason;
        });
        if (!inherited) {
            result.verdict = "not reachable";
            result.reason = "invalid perturbed data: " + issue.owner + "/" + issue.tag + ": " + issue.reason;
            return;
        }
    }
    const auto types = units::galactic_type_table(catalog.value());
    result.tables_changed = dg::content_identity(base) != dg::content_identity(loaded.value());
    std::ostringstream campaigns;
    campaigns << '[';
    bool first = true, completed = false, incomplete = false;
    for (const auto& baseline : baselines) {
        campaigns << (first ? "" : ",") << "{\"campaign\":" << json_string(baseline.name); first = false;
        if (!baseline.trial.error.empty()) {
            campaigns << ",\"verdict\":\"not reachable\",\"reason\":" << json_string("baseline: " + baseline.trial.error) << '}';
            incomplete = true; continue;
        }
        auto built = setup(loaded.value(), types, options_bindings, baseline.name);
        Trial trial;
        bool reused = false;
        if (!built) trial.error = "perturbed setup: " + built.error().message;
        else if (baseline.initial && built.value() == *baseline.initial) {
            // This session has no external providers or commands: identical
            // normalized immutable setup is exactly the same deterministic run.
            trial = baseline.trial;
            reused = true;
        } else trial = run(built.value(), options, deadline, 1);
        if (!trial.error.empty()) {
            campaigns << ",\"verdict\":\"not reachable\",\"reason\":" << json_string(trial.error) << '}';
            incomplete = true; continue;
        }
        completed = true;
        const bool hash = trial.hashes != baseline.trial.hashes;
        const bool snapshot = trial.snapshots != baseline.trial.snapshots;
        result.hash_changed |= hash; result.facts_changed |= snapshot;
        if (hash || snapshot) {
            for (std::size_t i = 0; i < trial.hashes.size(); ++i) {
                if (trial.hashes[i] != baseline.trial.hashes[i] || trial.snapshots[i] != baseline.trial.snapshots[i]) {
                    if (!result.first_tick || i < *result.first_tick) result.first_tick = i;
                    break;
                }
            }
        }
        campaigns << ",\"verdict\":" << json_string(hash || snapshot ? "changes" : "no change")
                  << ",\"baseline_reused\":" << (reused ? "true" : "false")
                  << ",\"hash_changed\":" << (hash ? "true" : "false") << ",\"facts_changed\":" << (snapshot ? "true" : "false")
                  << ",\"baseline_facts\":" << baseline.trial.final_facts << ",\"perturbed_facts\":" << trial.final_facts << '}';
    }
    campaigns << ']'; result.campaigns_json = campaigns.str();
    result.ticks_run = completed ? options.ticks : 0;
    result.verdict = result.hash_changed || result.facts_changed ? "changes" : completed && !incomplete ? "no change" : "not reachable";
    result.reason = result.verdict == "changes" ? "identity-normalized state or published snapshot changes"
        : result.verdict == "not reachable" ? "campaign setup/step refused or total runtime budget reached; see campaigns"
        : "unchanged in fixed stock sandbox window; AI, story, production, income and ability activation services are absent";
}
} // namespace

int run_galactic_perturb(Installation& installation, const Options& options, const std::vector<PlanRow>& rows, Clock::time_point began) {
    const auto deadline = began + std::chrono::seconds(options.max_seconds);
    auto loaded = dg::load_galactic_data(*installation.filesystem, installation.catalog->catalog);
    if (!loaded) { std::cerr << loaded.error().message << '\n'; return 2; }
    const auto types = units::galactic_type_table(installation.catalog->catalog);
    const auto opts = bindings(loaded.value(), types, options.seed);
    std::vector<StockBaseline> baselines;
    bool deterministic = true;
    for (const auto& campaign : loaded.value().campaigns) {
        if (campaign.planets.empty() || (options.galactic != "*" && !iequals(options.galactic, campaign.name))) continue;
        StockBaseline baseline; baseline.name = campaign.name;
        auto value = setup(loaded.value(), types, opts, campaign.name);
        if (!value) baseline.trial.error = value.error().message;
        else {
            baseline.initial = value.value();
            baseline.trial = run(value.value(), options, deadline, 1);
            if (baseline.trial.error.empty()) {
                auto again = run(value.value(), options, deadline, options.check_workers);
                if (again.error.empty()) deterministic &= baseline.trial.hashes == again.hashes && baseline.trial.snapshots == again.snapshots;
                else baseline.trial.error = again.error;
            }
        }
        baselines.push_back(std::move(baseline));
    }
    if (baselines.empty()) { std::cerr << "no matching stock campaign\n"; return 2; }
    std::ofstream out(options.out, std::ios::binary);
    if (!out) return 2;
    out << "{\"baseline\":{\"scene\":\"galactic\",\"ticks\":" << options.ticks << ",\"seed\":" << options.seed
        << ",\"check_workers\":" << options.check_workers << ",\"deterministic\":" << (deterministic ? "true" : "false")
        << ",\"campaign_count\":" << baselines.size() << ",\"max_seconds\":" << options.max_seconds << ",\"campaigns\":[";
    bool first = true;
    for (const auto& b : baselines) {
        out << (first ? "" : ",") << "{\"campaign\":" << json_string(b.name) << ",\"error\":" << json_string(b.trial.error) << '}'; first = false;
    }
    out << "]}}\n";
    if (!deterministic) return 1;
    // One process mounts/parses the stock catalog once; each overlay reloads only
    // its tables, then all campaigns. Sequential execution bounds CPU/memory.
    for (const auto& row : rows) {
        RowResult result; result.row = row;
        const auto start = Clock::now();
        if (start >= deadline) { result.verdict = "not reachable"; result.reason = "total runtime budget reached before this row"; }
        else check(installation, options, loaded.value(), opts, baselines, deadline, result);
        result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        out << row_json(result) << '\n'; out.flush();
        std::cout << row.id << ": " << result.verdict << " (" << result.reason << ")" << std::endl;
    }
    std::cout << rows.size() << " rows in " << std::chrono::duration<double>(Clock::now() - began).count() << " s\n";
    return 0;
}
} // namespace foc_tag_perturb_test_support
