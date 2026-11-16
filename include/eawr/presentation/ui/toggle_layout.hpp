#pragma once

// #5345: where a radio button or check box draws its art and caption (docs/behaviour/walks/dialog-toggles.md, DTG-1..4).
// The original sizes the glyph from the OFF (unchecked) art alone, at the control's top-left corner: the authored rectangle
// gives only the position. Every state's art (on, hover, disabled) is drawn into that one glyph box, so the glyph is the same
// size and in the same place whichever state is lit, and rows with different authored widths keep one glyph size. No
// per-texture offset: the box follows the art the theme names.

namespace eawr::presentation::ui {

struct ToggleSize {
    float width{};
    float height{};
};

struct ToggleBox {
    float x{};
    float y{};
    float width{};
    float height{};
};

// The glyph box: the OFF art's size at the control's top-left corner (an absent art gives an empty box).
[[nodiscard]] ToggleBox toggle_glyph_box(ToggleSize off_art) noexcept;

// Viewer hit-area fallback for a push-style radio: a zero authored dimension uses the OFF art.
// Positive authored dimensions keep their resource size (no minimum imposed on them).
[[nodiscard]] ToggleSize toggle_minimum_size(ToggleSize authored, ToggleSize off_art) noexcept;

// The caption's box. Push-style: the glyph box itself, the text centred on it (DTG-3). Otherwise: starts one gap after the
// glyph, as high as the glyph, so the text is vertically centred on the glyph (DTG-2); it ends at `control_width`.
[[nodiscard]] ToggleBox toggle_caption_box(const ToggleBox& glyph, bool push_style, float control_width, float gap) noexcept;

// The gap between a glyph and its caption: 30% of the glyph's width, rounded (the retail constant is a data table value the
// walk did not read; this is the viewer's long-standing value).
[[nodiscard]] float toggle_caption_gap(const ToggleBox& glyph) noexcept;

} // namespace eawr::presentation::ui
