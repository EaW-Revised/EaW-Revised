#pragma once

// The facts of a model that the unit-table loader keeps, as a record body: every bone, and for each collidable mesh its
// name, bone, bounds and collision geometry (and, optionally, the file's SHA-256: the field exists, slice 1 leaves it empty). Positions and bounds are stored as the raw IEEE
// bits the decoder produced, so a record rebuilds the same simulation projection a full decode gives. The float handling
// lives here (the units layer carries no floating point); the loader reads the rebuilt model unchanged.

#include "eawr/assets/assets.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::assets {

// The part of a decoded model the unit-table loader reads: all bones (name, parent, transform) and the collidable meshes
// with their bounds, bone, vertex positions, first skin bone index, indices and skin palette.
[[nodiscard]] Model simulation_projection(const Model& full);

// Record body. The facts of `model` (a full decode or a projection): bones, then collidable meshes. Nothing when a count
// or a name exceeds the format's range (the model is then simply not recorded). `sha256_hex` is empty or 64 hex digits.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_model_facts(const Model& model, std::string_view sha256_hex);

struct DecodedModelFacts final {
    Model model; // the simulation projection, with `source` as given
    std::string sha256_hex; // empty when the record carries no hash
};
// nullopt for any body that is not a well-formed record (the caller then decodes the file as if there were no record).
[[nodiscard]] std::optional<DecodedModelFacts> decode_model_facts(std::span<const std::uint8_t> body, Source source);

inline constexpr std::uint8_t model_facts_kind = 1;
// The store-wide facts format (the segment header carries it): bump it when the layout or the meaning of any kind's record
// changes in a way the epoch's code hash does not see (a change outside the directories hashed by cmake/EawrFactsCodeId.cmake).
inline constexpr std::uint32_t model_facts_format = 1;

} // namespace eawr::assets
