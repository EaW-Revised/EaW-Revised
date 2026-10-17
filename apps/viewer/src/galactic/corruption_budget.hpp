#pragma once

#include "eawr/presentation/particles/render.hpp"

#include <algorithm>

namespace eawr::viewer::galactic {

// Presentation budget per owner (each occupied layer, and the line owner).
// Charge reserved CPU slots, rather than the maximum offered to each effect.
class CorruptionParticleBudget final {
public:
    explicit CorruptionParticleBudget(std::size_t limit = 262144) : limit_(limit) {}
    void begin_frame() { refused_ = 0; }
    [[nodiscard]] core::Result<std::optional<presentation::particles::EffectHandle>> spawn(
        presentation::particles::EffectRegistry& registry,
        const presentation::particles::SystemDefinition& definition, std::uint32_t seed) {
        using Result = core::Result<std::optional<presentation::particles::EffectHandle>>;
        const auto capacity = std::min<std::size_t>(4096, limit_ - used_);
        if (!capacity) { ++refused_; return Result::success(std::nullopt); }
        auto spawned = registry.spawn(definition, seed, capacity);
        if (!spawned) return Result::failure(spawned.error());
        used_ += registry.reserved_particles(spawned.value());
        return Result::success(spawned.value());
    }
    [[nodiscard]] core::Result<void> release(presentation::particles::EffectRegistry& registry,
        presentation::particles::EffectHandle handle) {
        const auto reserved = registry.reserved_particles(handle);
        auto released = registry.release(handle);
        if (released) used_ -= reserved;
        return released;
    }
    [[nodiscard]] std::size_t used() const { return used_; }
    [[nodiscard]] std::size_t refused() const { return refused_; }
private:
    std::size_t limit_, used_{}, refused_{};
};

} // namespace eawr::viewer::galactic
