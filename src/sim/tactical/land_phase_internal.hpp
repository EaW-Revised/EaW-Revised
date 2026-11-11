#pragma once

#include "eawr/sim/world.hpp"

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace eawr::sim::tactical {

// A partition phase takes its body as a `std::function`. libstdc++ keeps a closure inside the function object only up to
// 16 bytes (two pointers) and heap-allocates a larger one; the MSVC STL keeps 48 bytes inline. A land phase runs every
// tick and the land step promises no heap use once warm, so its closure may hold two pointers at most: anything more
// travels in a context struct that the closure points to (#6131). The static_assert fails the build on every compiler.
template <class Body>
inline constexpr bool land_phase_body_fits = sizeof(Body) <= 2 * sizeof(void*) && std::is_trivially_copyable_v<Body>;

template <class Body>
[[nodiscard]] core::Result<void> land_phase(const PartitionExecutor& executor, std::string_view name, std::size_t partitions,
    const Body& body) {
    static_assert(land_phase_body_fits<Body>, "a land phase closure holds at most two pointers: put the rest in a context struct");
    return executor.execute_phase(name, partitions, body);
}

template <class Body>
[[nodiscard]] core::Result<void> land_partitions(const PartitionExecutor& executor, std::size_t partitions, const Body& body) {
    static_assert(land_phase_body_fits<Body>, "a land phase closure holds at most two pointers: put the rest in a context struct");
    return executor.execute(partitions, body);
}

} // namespace eawr::sim::tactical
