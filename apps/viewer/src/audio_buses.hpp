#pragma once

// #3165: the viewer's one audio bus layout (BA-45), made once per run and shared by the front
// end's menu sound and the battle's BattleAudio. Four buses: SFX (3D and UI one-shots), Voice
// (localized speech), Music, and the Mix bus at the master slider that feeds Master. A channel's
// heard level is its slider times the master's. Presentation only.

#include "eawr/presentation/audio/sfx.hpp"

#include <godot_cpp/classes/audio_effect_capture.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace eawr::presentation::godot_backend::audio_buses {

enum Bus : std::size_t { sfx, voice, music, mix, count };

inline constexpr std::array<const char*, count> names{"EAWR_SFX", "EAWR_Voice", "EAWR_Music", "EAWR_Mix"};

// Makes the buses, or takes the existing ones by name, with their sends, the Mix limiter and
// every bus at the default slider. With `captures`, each bus also gets the capture effect the
// battle report meters. Returns the bus indices (-1 without an audio server).
std::array<std::int32_t, count> open(std::array<godot::Ref<godot::AudioEffectCapture>, count>* captures = nullptr);

// True when all four buses exist.
[[nodiscard]] bool exist();

// The bus volumes for the given levels; buses that do not exist are left alone. Returns how
// many were set.
std::size_t set_levels(const audio::MixLevels& levels);

// Opens the buses when they are missing, then sets the levels.
void ensure(const audio::MixLevels& levels);

[[nodiscard]] float decibels(double linear);

} // namespace eawr::presentation::godot_backend::audio_buses
