#pragma once

#include "eawr/presentation/renderer.hpp"

#include <algorithm>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

struct RoutedTransform final {
    PresentationTransform transform;
    RenderPass pass{RenderPass::opaque};
};

// This order is executed by GodotRenderer::submit before it creates or updates
// any instance RID. EntityId breaks ties deterministically; Godot subsequently
// owns depth and blend ordering within a material class.
inline void order_pass_submissions(std::vector<RoutedTransform>& submissions) {
    std::stable_sort(submissions.begin(), submissions.end(), [](
        const RoutedTransform& left, const RoutedTransform& right) {
        const std::int32_t left_priority = render_pass_priority(left.pass);
        const std::int32_t right_priority = render_pass_priority(right.pass);
        if (left_priority != right_priority) return left_priority < right_priority;
        return left.transform.entity_id < right.transform.entity_id;
    });
}

} // namespace eawr::presentation::godot_backend::detail
