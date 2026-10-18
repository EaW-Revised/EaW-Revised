#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Ordered fragments preserve the session's original member lifetime order.
struct SessionStoreEnvironment {
    AsteroidScratch asteroid_scratch; // retained scratch, never canonical state
    NebulaScratch nebula_scratch;
};

} // namespace eawr::sim::tactical::session_detail
