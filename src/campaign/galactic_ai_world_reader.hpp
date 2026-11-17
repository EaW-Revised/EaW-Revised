#pragma once

#include <cstdint>
#include <memory>

#include "eawr/sim/galactic/session.hpp"

namespace eawr::campaign {
// Async step 3 (a): what the galactic AI reads of the world. Outside an AI window this is the session as it stands. Inside a
// window it is the window's input (galactic_ai_runtime.cpp): the values copied at the window cut and, while the window's lane
// barrier is open, that barrier's own view (the world as the window has decided it so far). The window body reads the world
// only through a reader, so it can later run apart from the live session.
class GalacticAiWorldReader {
public:
    virtual ~GalacticAiWorldReader() = default;
    [[nodiscard]] virtual std::shared_ptr<const sim::galactic::GalacticSnapshot> snapshot() const = 0;
    [[nodiscard]] virtual const sim::galactic::PlanetGraph* graph() const = 0;
    [[nodiscard]] virtual std::uint64_t completed_tick() const = 0;
    [[nodiscard]] virtual sim::EntityId next_entity_id() const = 0;
};
// The session read as it stands (tests, restore and everything outside a window).
class GalacticAiSessionReader final : public GalacticAiWorldReader {
public:
    explicit GalacticAiSessionReader(const sim::galactic::GalacticSession& session) noexcept : session_(&session) {}
    [[nodiscard]] std::shared_ptr<const sim::galactic::GalacticSnapshot> snapshot() const override { return session_->snapshot(); }
    [[nodiscard]] const sim::galactic::PlanetGraph* graph() const override { return session_->graph(); }
    [[nodiscard]] std::uint64_t completed_tick() const override { return session_->completed_tick(); }
    [[nodiscard]] sim::EntityId next_entity_id() const override { return session_->next_entity_id(); }
private:
    const sim::galactic::GalacticSession* session_;
};
} // namespace eawr::campaign
