#pragma once

// EvaluatePerception for galactic object scripts (#6473, GOL-17). An object script may ask any data-authored perception
// equation for a player and a planet. The equations are the mounted data's own (data/xml/ai/perceptualequations); this host
// answers their token lookups from the completed world, so the answer does not depend on whether the galactic AI runs, on
// how many workers the step uses, or on any AI lane.

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include "../script/foc/ai_equations.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::campaign {

class GalacticObjectPerception final {
public:
    // The perception equations of the mounted files. A file that does not load is named in notes(), never fatal.
    [[nodiscard]] static core::Result<std::shared_ptr<const GalacticObjectPerception>> load(const vfs::Vfs&);
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
    [[nodiscard]] std::size_t equation_count() const noexcept { return equations_.size(); }
    // Identity of the loaded equation text, for the content fingerprint of a saved game.
    [[nodiscard]] const std::string& digest() const noexcept { return digest_; }
    // The equation's value for `player` with `target` as the planet under evaluation, read from the completed world.
    // nullopt: no value; `why` then says which equation or lookup could not be answered.
    [[nodiscard]] std::optional<script::foc::ai::Real> evaluate(std::string_view name, sim::galactic::PlayerId player,
        std::optional<sim::galactic::PlanetId> target, const sim::galactic::GalacticSnapshot&, std::uint64_t seed,
        std::string& why, std::uint64_t& visited) const;

private:
    script::foc::ai::EquationSet equations_;
    std::vector<std::string> notes_;
    std::string digest_;
};

} // namespace eawr::campaign
