#pragma once

#include "world_ui_view.hpp"

#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace eawr::presentation::godot_backend::world_ui_detail {
using namespace godot;
namespace tactical = sim::tactical;



using Vec3 = std::array<float, 3>;
void bar(const RID item, const float centre_x, const float centre_y, const float width, const float height,
         const float fraction, const ui::Rgb& fill);

[[nodiscard]] inline float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

[[nodiscard]] inline Color colour(const ui::Rgb& rgb, const float alpha = 1.0F) {
    return {static_cast<float>(rgb[0]) / 255.0F, static_cast<float>(rgb[1]) / 255.0F, static_cast<float>(rgb[2]) / 255.0F,
            alpha};
}

[[nodiscard]] inline const sim::tactical::TacticalInstance* instance_of(const sim::tactical::TacticalSnapshot* snapshot,
                                                                 const sim::EntityId entity) {
    if (snapshot == nullptr) return nullptr;
    const auto instances = snapshot->instances();
    const auto found = std::lower_bound(instances.begin(), instances.end(), entity,
        [](const sim::tactical::TacticalInstance& instance, const sim::EntityId value) { return instance.entity_id < value; });
    return found != instances.end() && found->entity_id == entity ? &*found : nullptr;
}
} // namespace eawr::presentation::godot_backend::world_ui_detail
