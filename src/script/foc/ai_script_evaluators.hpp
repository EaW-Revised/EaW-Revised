#pragma once

// Generic Script_* perception evaluators (#1702, WGA-453, RDP-E09/E10, RDP-L01): the evaluator
// modules of Data/Scripts/Evaluators/ (stock or mod) are loaded as Lua instances, keep their own
// script state, and answer the Script_<Module>.Evaluate perception lookups of the AI equations.
// The host knows no evaluator names and no formulas: each call binds the context player to
// PlayerObject and the context target to Target, calls the module's Evaluate with the script
// parameters, and then runs Evaluator_Clean_Up. Only a numeric result succeeds; an error or a
// non-numeric result fails the lookup (it is never zero).

#include "ai_equations.hpp"
#include "host.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::foc::ai {
struct Target;
}

namespace eawr::script::foc::tactical_ai_detail {

class ScriptEvaluators {
public:
    struct Outcome {
        std::optional<ai::Real> value; // set only for a numeric result (as a binary32 value)
        std::string note;              // why there is no value (never part of a decision)
    };
    struct Stats {
        std::uint64_t calls{}, memo_hits{}, instructions{}, failures{}, loaded{};
    };

    // `modules`: the frozen script corpus by logical path. Evaluators are the modules directly
    // under Data/Scripts/Evaluators/; the catalogue is their upper-case stems, sorted, so an
    // evaluator's instance ID depends on the corpus only, never on the order of use.
    [[nodiscard]] static core::Result<std::shared_ptr<ScriptEvaluators>> create(
        const std::shared_ptr<detail::Host>& host, const std::map<std::string, std::string>& modules);
    ~ScriptEvaluators();

    [[nodiscard]] bool known(std::string_view upper_name) const;
    [[nodiscard]] std::vector<std::string> names() const;

    // The evaluation of one Script_<name>.Evaluate lookup. Serial only: it refuses (no value)
    // while the partitioned script service or an object turn runs, and when called recursively.
    [[nodiscard]] Outcome evaluate(std::string_view upper_name, sim::tactical::PlayerId player,
        const ai::Target* target, const std::vector<ai::Binding>& bindings);

    [[nodiscard]] const Stats& stats() const noexcept;

private:
    struct Impl;
    explicit ScriptEvaluators(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::script::foc::tactical_ai_detail
