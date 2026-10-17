#pragma once
#include "galactic_ai_world.hpp"

// Perception component bytes (domain "eawr-galactic-perception", version 3).
// The generic whole-state encoder is the canonical form: capture, restore
// validation and the cold digest oracle use it. The single-record encoders below
// write exactly the bytes the generic encoder gives a state holding only that
// record, so the incrementally maintained digest equals the cold one.
namespace eawr::script::foc::ai {

// Borrowed spelling of one cache record's key; nothing here owns storage.
struct PerceptionKeyView {
    tactical::PlayerId player{};
    std::optional<tactical::PlayerId> enemy{}, human{};
    galactic::PlanetId planet{};
    const std::vector<std::string>* tokens{};
    const std::vector<Binding>* bindings{};
};

// Whole-state canonical encoding: records sorted, markup payload appended.
[[nodiscard]] std::vector<std::uint8_t> encode_perception_state(const GalacticPerceptionState& state,
    std::span<const std::uint8_t> markup);

// Single-record encodings. `out` is cleared first and keeps its capacity, so a
// caller that reuses one buffer allocates nothing once the buffer has grown.
void encode_cache_record(std::vector<std::uint8_t>& out, const PerceptionKeyView& key,
    Real value, Real interval, std::uint64_t frame);
void encode_visibility_record(std::vector<std::uint8_t>& out, const GalacticVisibility& row);
void encode_flag_record(std::vector<std::uint8_t>& out, const GalacticStoryFlag& row);
void encode_hint_record(std::vector<std::uint8_t>& out, const GalacticHint& row);

} // namespace eawr::script::foc::ai
