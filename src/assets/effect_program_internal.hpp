#pragma once

#include "eawr/assets/effect.hpp"

namespace eawr::assets::detail {
// Shares the effect reader's budgets across all stage payloads.
[[nodiscard]] core::Result<EffectProgram> read_effect_program(
    std::span<const std::byte> bytes, const Source& source, std::size_t byte_offset,
    std::size_t& metadata_left, std::size_t& work_left);
[[nodiscard]] core::Result<EffectExpression> read_effect_expression(
    std::span<const std::byte> bytes, const Source& source, std::size_t byte_offset,
    std::size_t& metadata_left, std::size_t& work_left);
}
