#include "audio_buses.hpp"

#include <godot_cpp/classes/audio_effect_hard_limiter.hpp>
#include <godot_cpp/classes/audio_server.hpp>

#include <algorithm>
#include <cmath>

namespace eawr::presentation::godot_backend::audio_buses {

float decibels(const double linear) {
    return linear <= 1.0e-4 ? -80.0F : static_cast<float>(20.0 * std::log10(linear));
}

std::array<std::int32_t, count> open(std::array<godot::Ref<godot::AudioEffectCapture>, count>* captures) {
    std::array<std::int32_t, count> indices{-1, -1, -1, -1};
    godot::AudioServer* server = godot::AudioServer::get_singleton();
    if (server == nullptr) return indices;
    const auto open_bus = [&](const std::size_t which, const char* send, const bool limit) {
        const char* name = names[which];
        std::int32_t index = server->get_bus_index(name);
        if (index < 0) {
            index = server->get_bus_count();
            server->add_bus();
            server->set_bus_name(index, name);
        }
        indices[which] = index;
        server->set_bus_send(index, send);
        server->set_bus_volume_db(index, decibels(audio::default_volume_slider));
        while (server->get_bus_effect_count(index) > 0) server->remove_bus_effect(index, 0);
        if (limit) {
            // A viewer choice, not FoC's (BA-45): a hard limiter just under full scale keeps the
            // rare peaks of many shots over loud music from clipping the output.
            godot::Ref<godot::AudioEffectHardLimiter> limiter;
            limiter.instantiate();
            limiter->set_ceiling_db(-0.3F);
            server->add_bus_effect(index, limiter);
        }
        if (captures != nullptr) {
            // The report's RMS: the bus's signal after its effects, before its own slider.
            auto& capture = (*captures)[which];
            capture.instantiate();
            capture->set_buffer_length(0.5F);
            server->add_bus_effect(index, capture);
        }
    };
    // A bus sends only to a bus before it: the mix first.
    open_bus(mix, "Master", true);
    for (std::size_t which = 0; which < mix; ++which) open_bus(which, names[mix], false);
    return indices;
}

bool exist() {
    godot::AudioServer* server = godot::AudioServer::get_singleton();
    if (server == nullptr) return false;
    return std::all_of(names.begin(), names.end(), [server](const char* name) { return server->get_bus_index(name) >= 0; });
}

std::size_t set_levels(const audio::MixLevels& levels) {
    godot::AudioServer* server = godot::AudioServer::get_singleton();
    if (server == nullptr) return 0;
    const std::array<double, count> gains{levels.sfx, levels.speech, levels.music, levels.master};
    std::size_t set = 0;
    for (std::size_t which = 0; which < count; ++which) {
        const std::int32_t index = server->get_bus_index(names[which]);
        if (index < 0) continue;
        server->set_bus_volume_db(index, decibels(std::clamp(gains[which], 0.0, 1.0)));
        ++set;
    }
    return set;
}

void ensure(const audio::MixLevels& levels) {
    if (!exist()) (void)open();
    (void)set_levels(levels);
}

} // namespace eawr::presentation::godot_backend::audio_buses
