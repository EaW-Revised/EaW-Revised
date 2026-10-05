#pragma once

#include "eawr/core/load_profile.hpp"
#include "battle_audio.hpp"
#include "audio_output.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/skirmish/start.hpp"

#include <godot_cpp/classes/audio_effect_hard_limiter.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace eawr::presentation::godot_backend::battle_audio_detail {
using namespace godot;
namespace tactical = sim::tactical;


[[nodiscard]] std::string trim(std::string_view text);
[[nodiscard]] bool same(const std::string_view a, const std::string_view b);
[[nodiscard]] std::string tag(const data::EffectiveObject& object, const std::string_view name);
[[nodiscard]] float decibels(const double linear);
} // namespace eawr::presentation::godot_backend::battle_audio_detail
