#include "station_allocations.hpp"
#include <cstdlib>
#include <new>

// Count actual allocations made inside production callbacks. Keep replacements in their
// own translation unit so compiler inlining cannot mistake paired malloc/free for a mismatch.
namespace allocation_probe {
thread_local bool enabled{};
thread_local std::size_t count{};
}
void* operator new(const std::size_t size) {
    if (allocation_probe::enabled) ++allocation_probe::count;
    if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }
