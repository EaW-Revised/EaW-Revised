#pragma once
#include <cstddef>

namespace allocation_probe {
extern thread_local bool enabled;
extern thread_local std::size_t count;
}
