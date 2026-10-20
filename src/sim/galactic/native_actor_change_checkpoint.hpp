#pragma once

#include "eawr/sim/galactic/session.hpp"

#include <vector>

namespace eawr::sim::galactic::session_detail {
// Prepared notifications have already been staged by the private owner. Live
// replay may coalesce or erase existing entries as well as append duplicates.
class NativeActorChangeCheckpoint {
public:
    using Changes = std::vector<GalacticSession::NativeActorChange>;
    NativeActorChangeCheckpoint(Changes& pending, bool prepared)
        : pending_(pending), prepared_(prepared), before_(prepared ? pending : Changes{}) {}

    void restore() const {
        if (prepared_) pending_ = before_;
    }

private:
    Changes& pending_;
    bool prepared_;
    Changes before_;
};
} // namespace eawr::sim::galactic::session_detail
