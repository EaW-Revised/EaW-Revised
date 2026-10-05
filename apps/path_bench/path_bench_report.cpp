#include "path_bench_internal.hpp"

namespace path_bench {

[[nodiscard]] double percentile(std::vector<double> values, const double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    // Nearest rank.
    auto rank = static_cast<std::size_t>(fraction * static_cast<double>(values.size()) + 0.999999);
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    return values[rank - 1];
}

void report(const Run& run, const std::string& selection, const std::size_t workers) {
    std::vector<double> after;
    std::vector<double> before;
    for (const auto& record : run.after) after.push_back(record.ms);
    for (const auto& record : run.before) before.push_back(record.ms);
    std::uint64_t searches = 0;
    std::uint64_t slots = 0;
    tactical::PathSearchStats sum;
    std::uint64_t max_expansions = 0;
    std::uint64_t longest = 0;
    std::uint64_t slot_ticks = 0;
    std::uint64_t max_searches_in_tick = 0;
    std::uint64_t abandoned = 0;
    std::uint64_t sliced = 0;
    std::uint64_t slices = 0;
    for (const auto& record : run.after) {
        std::uint64_t in_tick = 0;
        for (const auto& call : record.calls) {
            if (call.slot) {
                ++slots;
                slot_ticks += call.total_ticks;
                continue;
            }
            abandoned += call.part == tactical::PathSearchPart::abandoned ? 1 : 0;
            sliced += call.part == tactical::PathSearchPart::last_slice ? 1 : 0;
            slices += call.part == tactical::PathSearchPart::slice || call.part == tactical::PathSearchPart::last_slice ? 1 : 0;
            ++searches;
            ++in_tick;
            sum.tries += call.tries;
            sum.expansions += call.expansions;
            sum.children += call.children;
            sum.queries += call.queries;
            sum.windows += call.windows;
            sum.leaves += call.leaves;
            sum.narrow += call.narrow;
            sum.total_ticks += call.total_ticks;
            sum.query_ticks += call.query_ticks;
            sum.set_ticks += call.set_ticks;
            max_expansions = std::max(max_expansions, call.expansions);
            longest = std::max(longest, call.total_ticks);
        }
        max_searches_in_tick = std::max(max_searches_in_tick, in_tick);
    }
    const auto ns = [&](const std::uint64_t ticks) { return static_cast<double>(ticks) * run.ns_per_clock_tick; };
    const auto per = [](const double value, const std::uint64_t count) { return count == 0 ? 0.0 : value / static_cast<double>(count); };
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << selection << ", " << workers << " worker(s): " << run.after.size() << " ticks from the order: p50 "
              << percentile(after, 0.5) << " ms, p99 " << percentile(after, 0.99) << " ms, max " << percentile(after, 1.0)
              << " ms; order tick " << (run.after.empty() ? 0.0 : run.after.front().ms) << " ms (plan-searches phase "
              << (run.after.empty() ? 0.0 : run.after.front().plan_ms) << " ms)\n";
    std::cout << "    before the order: p50 " << percentile(before, 0.5) << " ms, p99 " << percentile(before, 0.99) << " ms\n";
    std::cout << "    path searches " << searches << " (at most " << max_searches_in_tick << " in a tick), tries "
              << sum.tries << ", expansions mean " << per(static_cast<double>(sum.expansions), searches) << " max "
              << max_expansions << "; longest search " << ns(longest) / 1e6 << " ms; slot searches " << slots << " ("
              << ns(slot_ticks) / 1e6 << " ms)\n";
    std::cout << "    PC-08: " << abandoned << " searches given up at the budget, " << sliced << " sliced searches landed ("
              << slices << " slices); at most " << run.most_sliced << " in flight after a tick, holding "
              << static_cast<double>(run.most_sliced_bytes) / 1048576.0 << " MiB\n";
    const double total = ns(sum.total_ticks);
    const double query = ns(sum.query_ticks);
    const double set = ns(sum.set_ticks);
    std::cout << "    per expansion " << per(total, sum.expansions) << " ns = collision queries " << per(query, sum.expansions)
              << " + open/closed set " << per(set, sum.expansions) << " + rest " << per(total - query - set, sum.expansions)
              << "; per expansion " << per(static_cast<double>(sum.queries), sum.expansions) << " queries, "
              << per(static_cast<double>(sum.windows), sum.expansions) << " windows, "
              << per(static_cast<double>(sum.leaves), sum.expansions) << " leaves, "
              << per(static_cast<double>(sum.narrow), sum.expansions) << " exact tests\n";
    // Include the historical no-search hitch even when another tick is slower.
    const auto detail = [](const TickRecord& record) {
        std::cout << "    tick " << record.tick << ": " << record.ms << " ms; world " << record.world_ms
                  << ", AI engine " << record.engine_ms << ", Lua service " << record.service_ms
                  << ", other " << record.ms - record.world_ms - record.engine_ms - record.service_ms << " ms\n";
        std::cout << "      phases:";
        for (const auto& [name, cost] : record.phases) std::cout << ' ' << name << '=' << cost;
        std::cout << '\n';
    };
    const auto hitch = std::find_if(run.after.begin(), run.after.end(), [](const TickRecord& record) { return record.tick == 855; });
    if (hitch != run.after.end()) detail(*hitch);
    const auto worst = std::max_element(run.after.begin(), run.after.end(),
        [](const TickRecord& a, const TickRecord& b) { return a.ms < b.ms; });
    if (worst != run.after.end() && worst != hitch) detail(*worst);
}

void write_csv(const std::string& path, const Run& run) {
    std::ofstream out(path, std::ios::binary);
    std::set<std::string> phases;
    for (const auto* records : {&run.before, &run.after}) {
        for (const auto& record : *records) {
            for (const auto& [name, cost] : record.phases) {
                static_cast<void>(cost);
                phases.insert(name);
            }
        }
    }
    out << "tick,ms,plan_ms,searches,slots,expansions,queries,leaves,narrow,search_ms,query_ms,set_ms,world_ms,engine_ms,service_ms,other_ms";
    for (const auto& phase : phases) out << ',' << phase << "_ms";
    out << '\n';
    for (const auto* records : {&run.before, &run.after}) {
        for (const auto& record : *records) {
            std::uint64_t searches = 0;
            std::uint64_t slots = 0;
            tactical::PathSearchStats sum;
            for (const auto& call : record.calls) {
                ++(call.slot ? slots : searches);
                sum.expansions += call.expansions;
                sum.queries += call.queries;
                sum.leaves += call.leaves;
                sum.narrow += call.narrow;
                sum.total_ticks += call.total_ticks;
                sum.query_ticks += call.query_ticks;
                sum.set_ticks += call.set_ticks;
            }
            const auto ms = [&](const std::uint64_t ticks) { return static_cast<double>(ticks) * run.ns_per_clock_tick / 1e6; };
            out << record.tick << ',' << record.ms << ',' << record.plan_ms << ',' << searches << ',' << slots << ','
                << sum.expansions << ',' << sum.queries << ',' << sum.leaves << ',' << sum.narrow << ','
                << ms(sum.total_ticks) << ',' << ms(sum.query_ticks) << ',' << ms(sum.set_ticks) << ','
                << record.world_ms << ',' << record.engine_ms << ',' << record.service_ms << ','
                << record.ms - record.world_ms - record.engine_ms - record.service_ms;
            for (const auto& phase : phases) {
                const auto found = record.phases.find(phase);
                out << ',' << (found == record.phases.end() ? 0.0 : found->second);
            }
            out << '\n';
        }
    }
}

// Every path search from the order on: its tick, unit and work counts (in the order the searches
// finished, which with more than one worker is not the planning order).
void write_searches_csv(const std::string& path, const Run& run) {
    std::ofstream out(path, std::ios::binary);
    out << "tick,entity,tries,expansions,children,queries,leaves,narrow,search_ms\n";
    for (const auto& record : run.after) {
        for (const auto& call : record.calls) {
            if (call.slot) continue;
            out << record.tick << ',' << call.entity << ',' << call.tries << ',' << call.expansions << ',' << call.children
                << ',' << call.queries << ',' << call.leaves << ',' << call.narrow << ','
                << static_cast<double>(call.total_ticks) * run.ns_per_clock_tick / 1e6 << '\n';
        }
    }
}

// The pinned rows of a 1-worker run: every search from the order on, in planning order.
[[nodiscard]] std::vector<std::string> work_rows(const Run& run) {
    std::vector<std::string> rows;
    for (const auto& record : run.after) {
        for (const auto& call : record.calls) {
            if (call.slot) continue;
            const auto layer = run.layers.find(call.entity);
            constexpr std::string_view parts = "wasl"; // whole, abandoned, slice, last slice
            rows.push_back(std::to_string(record.tick) + ',' + std::to_string(call.entity) + ','
                + (layer != run.layers.end() ? std::to_string(layer->second) : std::string("-")) + ','
                + parts[static_cast<std::size_t>(call.part)] + ',' + std::to_string(call.tries) + ',' + std::to_string(call.expansions) + ',' + std::to_string(call.queries) + ','
                + std::to_string(call.leaves) + ',' + std::to_string(call.narrow));
        }
    }
    return rows;
}

// PC-07, PC-08 in a 1-worker run: a search starts only while its layer has spent less than
// the budget in its tick and stops within one parent (10 children) past it; a slice counts at
// most the slice size and one parent. Returns the violations.
[[nodiscard]] std::vector<std::string> budget_violations(const Run& run, const tactical::AvoidanceRules& rules) {
    constexpr std::uint64_t parent = 10;
    std::vector<std::string> violations;
    for (const auto& record : run.after) {
        std::map<std::size_t, std::uint64_t> spent;
        for (const auto& call : record.calls) {
            if (call.slot) continue;
            const auto where = "tick " + std::to_string(record.tick) + " unit " + std::to_string(call.entity);
            if (call.part == tactical::PathSearchPart::slice || call.part == tactical::PathSearchPart::last_slice) {
                if (call.expansions > rules.search_slice + parent) {
                    violations.push_back(where + ": a slice of " + std::to_string(call.expansions) + " expansions");
                }
                continue;
            }
            const auto layer = run.layers.find(call.entity);
            if (layer == run.layers.end()) continue;
            auto& used = spent[layer->second];
            if (used >= rules.search_budget) {
                violations.push_back(where + " searched after " + std::to_string(used) + " expansions");
            }
            used += call.expansions;
            if (used > rules.search_budget + parent) {
                violations.push_back(where + " took layer " + std::to_string(layer->second) + " to " + std::to_string(used)
                    + " expansions");
            }
        }
    }
    return violations;
}

} // namespace path_bench
