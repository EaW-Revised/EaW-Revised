#pragma once

#include "../battle_content.hpp"
#include "../memory_attribution.hpp"

namespace eawr::presentation::godot_backend {
namespace gc_memory_detail {
inline std::weak_ptr<const BattleContent> setup_content;
}

// A diagnostic reference only: the menu remains the owner of this content.
// Register immediately before a solo campaign starts; never retain it for a run.
inline void register_gc_setup_memory(const std::shared_ptr<const BattleContent>& content) {
    if (MemoryAttribution::enabled()) gc_memory_detail::setup_content = content;
}
} // namespace eawr::presentation::godot_backend
