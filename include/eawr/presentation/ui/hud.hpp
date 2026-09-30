#pragma once

#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <functional>
#include <memory>

namespace eawr::presentation::ui {

// Top-left, tightly packed alpha. A nonzero texel is part of the faceplate.
// The same nearest, repeat sampling must be used by the canvas renderer.
struct ShellAlphaMask {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> alpha;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool opaque(double u, double v) const noexcept;
};
[[nodiscard]] core::Result<ShellAlphaMask> shell_alpha_mask(const assets::Texture& texture);
using ShellMaskLookup = std::function<std::shared_ptr<const ShellAlphaMask>(std::string_view)>;

struct HudComponent {
    std::string name;
    data::ui::ReferenceRect rect;
    bool visible{true};
};
struct HudFaceplate {
    data::ui::ShellTriangle triangle;
    std::shared_ptr<const ShellAlphaMask> mask;
};

// Owning snapshot at one animation time. No simulation or engine dependency.
// Missing faceplate masks are diagnosed and omitted, never replaced by mesh bounds.
struct HudViewModel {
    std::vector<HudComponent> components;
    std::vector<HudFaceplate> faceplates;
    std::optional<data::ui::ReferenceRect> visible_extent;
    std::vector<core::Diagnostic> diagnostics;

    // Contiguous two-digit indices starting at 00, present in catalogue AND shell.
    // Includes temporarily hidden slots, so animation cannot change capacity.
    [[nodiscard]] std::size_t family_size(std::string_view stem) const;
    [[nodiscard]] ShellPlacement placement(const ReferenceSpace& space) const noexcept;
    [[nodiscard]] bool hit_test(ReferencePoint point) const noexcept;
    [[nodiscard]] bool hit_test_screen(ReferencePoint point, const ReferenceSpace& space) const noexcept;
};
[[nodiscard]] HudViewModel hud_view_model(const data::ui::ShellAnchors& shell,
    const data::ui::CommandBarCatalog& catalog, std::uint32_t variant, const ShellMaskLookup& masks);

// Reuses the validated ALA player's translation/rotation/scale interpolation.
// The player must have been created for this exact model. Callers can retain
// it and sample repeatedly without reparsing assets.
[[nodiscard]] core::Result<data::ui::ShellAnchorLoad> animated_shell_anchors(
    const assets::Model& model, const animation::Player& player, const animation::SampleRequest& request);

} // namespace eawr::presentation::ui
