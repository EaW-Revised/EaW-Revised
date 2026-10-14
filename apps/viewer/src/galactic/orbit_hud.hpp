#pragma once
#include "eawr/sim/galactic/snapshot.hpp"
#include "galaxy_view.hpp"
#include <godot_cpp/classes/texture_button.hpp>
#include <map>
#include <cmath>
#include <array>
#include <cstdint>

namespace eawr::presentation::godot_backend {
// GFC-47: this clock supplies admitted presentation frame advances, independently
// of the galaxy's mode frame. Native device/render admission remains GFS-U10.
struct StationDisplayClock {
    double elapsed_seconds{};
    [[nodiscard]] float advance(double seconds, double admission_rate, bool synchronizer_paused) {
        if (synchronizer_paused || !std::isfinite(seconds) || seconds <= 0.0
            || !std::isfinite(admission_rate) || admission_rate <= 0.0) return 0.0F;
        // GFC-47: integer-millisecond period, one admission per call. Retain
        // sub-period carry but discard a complete backlog after a hitch.
        const double interval = std::floor(1000.0 / admission_rate) / 1000.0;
        elapsed_seconds += seconds;
        if (elapsed_seconds < interval) return 0.0F;
        elapsed_seconds -= interval;
        if (elapsed_seconds >= interval) elapsed_seconds = 0.0;
        return 1.0F;
    }
    [[nodiscard]] static float render_delta(float frames, float frames_per_second) {
        const float delta = frames * frames_per_second / 1000.0F;
        return delta > 0.0F ? delta : 0.0F;
    }
};
struct OrbitHudState {
    struct Slot {
        std::string name;
        godot::TextureButton* button{};
        sim::EntityId object{};
        sim::galactic::TypeId type{};
    };
    std::map<std::size_t, Slot> slots;
    viewer::galactic::ObjectId planet{};
    sim::galactic::TypeId station_type{};
    std::string model;
    viewer::galactic::Vec3 position;
    StationDisplayClock display_clock{};
    std::array<float, 5> rotation_probe_advances{};
    float rotation_rate{}, rotation{}, display_delta{};
    std::uint64_t rotation_updates{};
    bool shown{}, structures_known{};
    std::size_t capacity{};
};
} // namespace eawr::presentation::godot_backend
