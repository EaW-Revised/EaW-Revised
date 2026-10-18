#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreInitialization {
    std::optional<core::Diagnostic> initialization_error;
};

struct SessionStoreTickWork {
    TacticalSession::TickWork tick_work{};
};

} // namespace eawr::sim::tactical::session_detail
