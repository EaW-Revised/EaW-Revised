#pragma once

// DiscreteDistribution, the weighted list the story, object and plan scripts of both modes call
// (docs/research/remake-gc-retail-rules.md RGC-190..194). One definition for every host: the galactic story scheduler
// and the battle scheduler both pin distribution_module in their manifest and call register_discrete_distribution.

#include "ai_equations.hpp"
#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::script::foc::distribution {

namespace auth = script::authoritative;
using Number = script::numeric::LuaNumber;

// The module the story scheduler pins and runs in every instance before its script (ScriptScheduler::register_prelude).
inline constexpr std::string_view distribution_module = "EAWR/Story/DiscreteDistribution.lua";

// DiscreteDistribution (RGC-190..194). The list is an object of the script's own state, not host storage: a table that
// holds the inserted values exactly as passed (identity included) next to their running 32-bit totals, so its save, hash
// and rollback are the Lua graph's. The float arithmetic and the draw are the natives below.
inline constexpr std::string_view distribution_source = R"LUA(
DiscreteDistribution = {}

function DiscreteDistribution.Create()
    local values = {}
    local totals = {}
    local count = 0
    local list = {}
    list.Insert = function(...)
        if arg.n ~= 2 then
            error("DiscreteDistribution.Insert expects exactly two arguments (value, weight)")
        end
        if type(arg[2]) ~= "number" then
            error("DiscreteDistribution.Insert expects a number as its second argument")
        end
        local weight = EAWR_Distribution.Weight(arg[2])
        if weight > 0 then
            count = count + 1
            values[count] = arg[1]
            if count == 1 then
                totals[count] = weight
            else
                totals[count] = EAWR_Distribution.Add(totals[count - 1], weight)
            end
        end
    end
    list.Sample = function()
        if count == 0 then
            return
        end
        return values[EAWR_Distribution.Pick(totals)]
    end
    return list
end
)LUA";

// The engine globals this definition provides, for the Lua API scan (tools/lua_api_scan): a Lua-defined global has no
// registration call to find.
inline constexpr std::string_view lua_defined_globals[] = {"DiscreteDistribution.Create"};

namespace detail {

inline core::Diagnostic fault(std::string message) {
    core::Diagnostic out;
    out.code = std::string(auth::codes::invalid_request);
    out.message = std::move(message);
    return out;
}

inline const Number* number_at(const auth::ValueList& arguments, std::size_t index) {
    return index < arguments.size() ? std::get_if<Number>(&arguments[index].data) : nullptr;
}

inline core::Result<auth::ValueList> one_number(Number value) {
    return core::Result<auth::ValueList>::success(auth::ValueList{auth::Value::number(value)});
}

} // namespace detail

// Registers the natives the prelude calls and the prelude itself on a scheduler whose manifest already holds
// distribution_module (added with add_to_manifest).
[[nodiscard]] inline core::Result<void> register_discrete_distribution(auth::ScriptScheduler& scheduler) {
    using namespace detail;
    // RGC-191: weights narrow to 32-bit floats; a weight that is zero, negative or NaN is not stored (it narrows to 0 here).
    if (auto added = scheduler.register_binding("EAWR_Distribution.Weight", [](auth::BindingContext&, const auth::ValueList& arguments) {
            const Number* weight = number_at(arguments, 0);
            if (arguments.size() != 1 || weight == nullptr)
                return core::Result<auth::ValueList>::failure(fault("EAWR_Distribution.Weight expects one number"));
            const Number narrowed = ai::to_single(*weight);
            return one_number(narrowed > Number(0) ? narrowed : Number(0));
        }); !added) return added;
    // RGC-191: every entry keeps a running total, a 32-bit float sum.
    if (auto added = scheduler.register_binding("EAWR_Distribution.Add", [](auth::BindingContext&, const auth::ValueList& arguments) {
            const Number* left = number_at(arguments, 0);
            const Number* right = number_at(arguments, 1);
            if (arguments.size() != 2 || left == nullptr || right == nullptr)
                return core::Result<auth::ValueList>::failure(fault("EAWR_Distribution.Add expects two numbers"));
            return one_number(ai::to_single(*left + *right));
        }); !added) return added;
    // RGC-192/194: one 15-bit draw of the game stream, f = draw / 32767 and u = f x total in 32-bit floats; the first
    // entry whose running total reaches u (u = 0 picks the first). The ascending running totals come in, the 1-based index out.
    if (auto added = scheduler.register_binding("EAWR_Distribution.Pick", [](auth::BindingContext& context, const auth::ValueList& arguments) {
            const auto* list = arguments.size() == 1 ? std::get_if<std::vector<auth::Value>>(&arguments[0].data) : nullptr;
            if (list == nullptr || list->empty())
                return core::Result<auth::ValueList>::failure(fault("EAWR_Distribution.Pick expects a non-empty list of totals"));
            std::vector<Number> totals;
            totals.reserve(list->size());
            for (const auto& item : *list) {
                const auto* total = std::get_if<Number>(&item.data);
                if (total == nullptr) return core::Result<auth::ValueList>::failure(fault("EAWR_Distribution.Pick expects numeric totals"));
                totals.push_back(*total);
            }
            const std::uint32_t draw = static_cast<std::uint32_t>(context.random().next_word() >> 49U);
            const Number fraction = ai::to_single(Number(draw) / Number(32767));
            const Number threshold = ai::to_single(fraction * totals.back());
            std::size_t index = 0;
            while (index + 1 < totals.size() && totals[index] < threshold) ++index;
            return one_number(Number(static_cast<std::uint64_t>(index + 1)));
        }); !added) return added;
    return scheduler.register_prelude(distribution_module);
}

} // namespace eawr::script::foc::distribution
