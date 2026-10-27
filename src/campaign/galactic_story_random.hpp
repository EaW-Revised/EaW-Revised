#pragma once

// The random-number side of the galactic story-script host (docs/research/remake-gc-retail-rules.md RGC-190..199): the
// weighted list `DiscreteDistribution`, `GameRandom` and `GameRandom.Free_Random`. One included translation unit
// (galactic_story_scripts.cpp); nothing here is part of a public header.

#include "../script/foc/ai_equations.hpp"
#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::campaign::story_random {

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

// RGC-195/198: one 15-bit step. `free` selects the free stream (RGC-197), otherwise the synchronised game stream.
inline std::uint32_t step(auth::BindingContext& context, bool free) {
    const std::uint64_t word = free ? context.random().next_free_word() : context.random().next_word();
    return static_cast<std::uint32_t>(word >> 49U);
}

// RGC-198: bounds swapped when reversed, equal bounds return the bound with no draw, a span above 32766 is cut to 32766,
// the draw is rejection sampling on the smallest covering bit mask (each rejected word one step).
inline std::int64_t integer_draw(auth::BindingContext& context, std::int64_t low, std::int64_t high, bool free, std::string_view name) {
    if (high < low) std::swap(low, high);
    if (low == high) return low;
    std::int64_t span = high - low;
    if (span > 32766) {
        context.report(auth::codes::invalid_request, std::string(name) + " range exceeds 32766 steps; clamped");
        span = 32766;
    }
    std::uint32_t mask = 1;
    while (mask < static_cast<std::uint64_t>(span)) mask = (mask << 1U) | 1U;
    while (true) {
        const std::uint32_t candidate = step(context, free) & mask;
        if (candidate <= static_cast<std::uint64_t>(span)) return low + candidate;
    }
}

// RGC-195: GameRandom and GameRandom.Free_Random take the same forms. No argument: one 15-bit draw of the game stream
// (also for Free_Random). One number n: 0..n. Two numbers: a..b. Numbers truncate to integers. A non-number first
// argument is a script error that still draws over the default range; a non-number second argument one over 0..first.
inline core::Result<auth::ValueList> random_call(
    auth::BindingContext& context, const auth::ValueList& arguments, bool free, std::string_view name) {
    if (arguments.size() > 2)
        return core::Result<auth::ValueList>::failure(fault(std::string(name) + " expects zero, one or two numbers"));
    const auto truncated = [](const Number& value) { return static_cast<std::int64_t>(static_cast<std::int32_t>(value)); };
    const auto default_draw = [&] { return one_number(Number(static_cast<std::int64_t>(step(context, false)))); };
    if (arguments.empty()) return default_draw();
    const Number* first = number_at(arguments, 0);
    if (first == nullptr) {
        context.report(auth::codes::invalid_request, std::string(name) + " expects numbers; the first argument is not a number");
        return default_draw();
    }
    const Number* second = number_at(arguments, 1);
    if (arguments.size() == 2 && second == nullptr)
        context.report(auth::codes::invalid_request, std::string(name) + " expects numbers; the second argument is not a number");
    const std::int64_t low = arguments.size() == 2 && second != nullptr ? truncated(*first) : 0;
    const std::int64_t high = arguments.size() == 2 && second != nullptr ? truncated(*second) : truncated(*first);
    return one_number(Number(integer_draw(context, low, high, free, name)));
}

} // namespace detail

// Registers the natives the prelude calls, the prelude itself and GameRandom / GameRandom.Free_Random on a story
// scheduler whose manifest already holds distribution_module. `multiplayer`: RGC-196, Free_Random warns and performs
// the plain GameRandom call on the synchronised stream.
[[nodiscard]] inline core::Result<void> register_story_random(auth::ScriptScheduler& scheduler, bool multiplayer) {
    using namespace detail;
    // RGC-191: weights narrow to 32-bit floats; a weight that is zero, negative or NaN is not stored (it narrows to 0 here).
    if (auto added = scheduler.register_binding("EAWR_Distribution.Weight", [](auth::BindingContext&, const auth::ValueList& arguments) {
            const Number* weight = number_at(arguments, 0);
            if (arguments.size() != 1 || weight == nullptr)
                return core::Result<auth::ValueList>::failure(fault("EAWR_Distribution.Weight expects one number"));
            const Number narrowed = script::foc::ai::to_single(*weight);
            return one_number(narrowed > Number(0) ? narrowed : Number(0));
        }); !added) return added;
    // RGC-191: every entry keeps a running total, a 32-bit float sum.
    if (auto added = scheduler.register_binding("EAWR_Distribution.Add", [](auth::BindingContext&, const auth::ValueList& arguments) {
            const Number* left = number_at(arguments, 0);
            const Number* right = number_at(arguments, 1);
            if (arguments.size() != 2 || left == nullptr || right == nullptr)
                return core::Result<auth::ValueList>::failure(fault("EAWR_Distribution.Add expects two numbers"));
            return one_number(script::foc::ai::to_single(*left + *right));
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
            const std::uint32_t draw = step(context, false);
            const Number fraction = script::foc::ai::to_single(Number(draw) / Number(32767));
            const Number threshold = script::foc::ai::to_single(fraction * totals.back());
            std::size_t index = 0;
            while (index + 1 < totals.size() && totals[index] < threshold) ++index;
            return one_number(Number(static_cast<std::uint64_t>(index + 1)));
        }); !added) return added;
    if (auto added = scheduler.register_prelude(distribution_module); !added) return added;
    if (auto added = scheduler.register_binding("GameRandom", [](auth::BindingContext& context, const auth::ValueList& arguments) {
            return random_call(context, arguments, false, "GameRandom");
        }); !added) return added;
    return scheduler.register_free_random([multiplayer](auth::BindingContext& context, const auth::ValueList& arguments) {
        if (multiplayer) {
            context.report(auth::codes::script_warning,
                "GameRandom.Free_Random cannot give non-synchronised numbers from script in multiplayer; using GameRandom");
            return random_call(context, arguments, false, "GameRandom.Free_Random");
        }
        return random_call(context, arguments, true, "GameRandom.Free_Random");
    });
}

} // namespace eawr::campaign::story_random
