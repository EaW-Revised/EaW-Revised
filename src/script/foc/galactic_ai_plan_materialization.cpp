#include "galactic_ai_plans.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace eawr::script::foc::ai::galactic_goals {
namespace {
using authoritative::Value;
template<class T> Value optional_value(const std::optional<T>& value) {
    if (!value) return {};
    if constexpr (std::is_same_v<T, bool>) return Value{*value};
    else return Value::number(*value);
}
core::Diagnostic invalid(std::string message) {
    return {"galactic-ai.plan-materialization", core::Severity::error, std::move(message), {}, {}, {}, {}};
}
}
std::vector<std::uint64_t> galactic_contrast_category_order(std::span<const std::uint64_t> keys) {
    // WGA-448: only the retained ordering is modelled. Values and lookup stay
    // in the ordinary typed map; neither platform hash iteration nor a stock
    // category roster can choose the first equal contrast score.
    std::size_t buckets = 8;
    std::map<std::uint64_t, std::size_t> group_of_bucket;
    std::vector<std::vector<std::uint64_t>> groups;
    std::set<std::uint64_t> seen;
    const auto add_to_group = [&](std::uint64_t key) {
        const auto seed = (key ^ UINT64_C(0xdeadbeef)) & UINT64_C(0x7fffffff);
        const auto hash = (seed * UINT64_C(16807)) % UINT64_C(2147483647);
        const auto bucket = hash & static_cast<std::uint64_t>(buckets - 1);
        const auto [group, inserted] = group_of_bucket.try_emplace(bucket, groups.size());
        if (inserted) groups.emplace_back();
        auto& values = groups[group->second];
        values.insert(std::lower_bound(values.begin(), values.end(), key), key);
    };
    const auto flattened = [&] {
        std::vector<std::uint64_t> result;
        for (const auto& group : groups) result.insert(result.end(), group.begin(), group.end());
        return result;
    };
    for (const auto key : keys) {
        if (!seen.insert(key).second) continue;
        const auto pending = to_single(real(static_cast<std::int64_t>(seen.size())));
        const auto capacity = to_single(real(static_cast<std::int64_t>(buckets)));
        if (real(1) < to_single(pending / capacity)) {
            const auto previous = flattened();
            if (buckets < 512) buckets *= 8;
            else {
                const auto required = static_cast<std::size_t>(truncate(pending));
                if (required > (std::numeric_limits<std::size_t>::max() >> 1) + 1)
                    throw std::length_error("galactic contrast category map exceeds representable bucket count");
                buckets = std::bit_ceil(required);
            }
            group_of_bucket.clear(); groups.clear();
            for (const auto retained : previous) add_to_group(retained);
        }
        add_to_group(key);
    }
    return flattened();
}
core::Result<MaterializedPlanCatalog> materialize_plan_catalog(const detail::Host& host, const PlanCatalog& catalog) {
    using Result = core::Result<MaterializedPlanCatalog>;
    MaterializedPlanCatalog out;
    for (const auto& source : catalog.plans) {
        std::string goals;
        for (const auto& goal : source.goals) { if (!goals.empty()) goals += '|'; goals += goal; }
        std::vector<Value> teams;
        GalacticPlanConstraints constraints;
        constraints.minimum_cost = source.minimum_cost; constraints.additional_cost = source.additional_cost;
        constraints.magic_stealing = source.magic_stealing; constraints.pool_count = source.pool_count;
        for (const auto& force : source.teams) {
            std::vector<Value> declarations{Value::text(force.name)};
            GalacticForceConstraints options;
            for (const auto& declaration : force.declarations) {
                const auto text = trimmed(declaration); const auto name = upper_case(trimmed(text.substr(0, text.find('='))));
                if (name == "DENYHEROATTACH") options.deny_heroes = true;
                else if (name == "DENYSPECIALWEAPONATTACH") options.deny_special_weapons = true;
                else if (name == "REQUIREUNIQUESTAGE") options.unique_stage = true;
                else if (name == "REQUIREMATCHINGSTAGE") options.matching_stage = true;
                else if (!name.empty() && name.front() == '-') {
                    // RGC-61: the remainder is one type lookup, not a split list. A defined type is excluded; an undefined name
                    // is ignored (the debug build logs nothing; this load lists it).
                    const auto remainder = trimmed(trimmed(text.substr(0, text.find('='))).substr(1)); // authored spelling
                    const auto type = host.types_by_name.find(upper_case(remainder));
                    if (type != host.types_by_name.end()) options.excluded_types.push_back(type->second->type_id);
                    else out.diagnostics.push_back(source.name + ": " + force.name + " excludes unknown type " + remainder
                        + "; the exclusion is ignored (RGC-61)");
                }
                declarations.push_back(Value::text(declaration));
            }
            teams.push_back(Value{std::move(declarations)}); constraints.forces.push_back(std::move(options));
        }
        std::vector<Value> required;
        for (const auto& category : source.required_categories) required.push_back(Value::text(category));
        authoritative::ValueList values{Value::text(goals), Value{std::move(teams)}, optional_value(source.ignore_target),
            optional_value(source.magic), optional_value(source.allow_freestore), optional_value(source.allow_engaged),
            optional_value(source.failure_adjust), optional_value(source.minimum_contrast), optional_value(source.maximum_contrast),
            Value{std::move(required)}, optional_value(source.allow_special_weapons)};
        std::vector<std::string> notes;
        auto plan = build_plan(host, source.name, source.module, values, notes, true);
        if (!plan) return Result::failure(plan.error());
        // RGC-62: an undefined inclusion name is logged and skipped, and a row left with no possible type is removed at
        // parse time; any other note still refuses the plan.
        constexpr std::string_view unknown_type = "unknown type ", unknown_required = "unknown required category ";
        for (const auto& note : notes) {
            if (note.starts_with(unknown_type)) out.diagnostics.push_back(source.name + ": " + note + "; the name is skipped (RGC-62)");
            else if (note.starts_with(unknown_required)) out.diagnostics.push_back(source.name + ": " + note + "; the entry is skipped (RGC-162)");
            else return Result::failure(invalid(source.name + ": " + note));
        }
        plan.value().galactic_contrast_qualified = true;
        for (const auto& [enemy, friendly] : source.contrast) {
            const auto category = host.setup.content.categories.find(enemy);
            if (category == host.setup.content.categories.end()) continue;
            // The original inserts the first list for a repeated enemy category.
            auto [row, inserted] = plan.value().galactic_contrast.try_emplace(category->second);
            if (!inserted) continue;
            plan.value().galactic_contrast_order.push_back(category->second);
            for (const auto& [name, weight] : friendly) {
                detail::ContrastEntry entry; entry.weight = weight;
                if (const auto bits = host.setup.content.categories.find(name); bits != host.setup.content.categories.end()) entry.category_bits = bits->second;
                else if (const auto type = host.types_by_name.find(name); type != host.types_by_name.end()) entry.type = type->second->type_id;
                else continue;
                row->second.push_back(entry);
            }
        }
        plan.value().galactic_contrast_order = galactic_contrast_category_order(plan.value().galactic_contrast_order);
        for (std::size_t index = 0; index < plan.value().taskforces.size(); ++index) {
            const auto& options = constraints.forces[index];
            for (auto& team : plan.value().taskforces[index].teams)
                std::erase_if(team.types, [&](auto type) { return std::find(options.excluded_types.begin(), options.excluded_types.end(), type) != options.excluded_types.end(); });
        }
        out.constraints.emplace(source.name, std::move(constraints)); out.plans.push_back(std::move(plan).value());
    }
    return Result::success(std::move(out));
}
} // namespace eawr::script::foc::ai::galactic_goals
