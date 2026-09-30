#include "eawr/presentation/space/fog_field.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::presentation::space {
namespace {

constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
constexpr std::uint32_t max_cells = 4096;
// FW-09: fade values 239 to 255 are a cell fading in; 16 and up below that show clear.
constexpr double ramp_in_floor = 239.0;
constexpr double clear_floor = 16.0;

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// The last child of that name wins, as later GameConstants entries do.
[[nodiscard]] const data::XmlNode* last_child(const data::XmlNode& root, const std::string_view name) noexcept {
    const data::XmlNode* found = nullptr;
    for (const data::XmlNode& child : root.children) {
        if (child.name == name) found = &child;
    }
    data::tag_trace::used(found);
    return found;
}

// A number such as "-80.0" or "6.0" (a trailing f as GameConstants sometimes writes is kept out).
[[nodiscard]] std::optional<double> number(std::string_view text) noexcept {
    text = trim(text);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.remove_suffix(1);
    double result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(result)) return std::nullopt;
    return result;
}

// "r, g, b, a" with 0 to 255 each.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>> colour(const std::string_view text) noexcept {
    std::array<std::uint8_t, 4> parts{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::size_t comma = text.find(',', start);
        if ((comma == std::string_view::npos) != (index + 1 == parts.size())) return std::nullopt;
        const auto value = number(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!value || *value < 0.0 || *value > 255.0 || *value != std::floor(*value)) return std::nullopt;
        parts[index] = static_cast<std::uint8_t>(*value);
        start = comma + 1;
    }
    return parts;
}

void fallback(FogLooks& looks, const std::string_view tag, const std::string_view why) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::fog_looks);
    diagnostic.severity = core::Severity::warning;
    diagnostic.message = std::string(tag) + " " + std::string(why) + "; the FoC value is used";
    diagnostic.logical_path = std::string(constants_path);
    looks.diagnostics.push_back(std::move(diagnostic));
}

// FW-09: a held cell's fade value one advance on.
[[nodiscard]] double held_value(double value, const double frames) noexcept {
    if (value < ramp_in_floor) {
        // A newly held cell that was fogged fades in from 239 one step a frame; a cell still
        // fading out resumes at the matching point of the fade-in; one still shown clear stays
        // clear.
        if (value >= clear_floor) return 255.0;
        if (value <= 0.0) return ramp_in_floor;
        return std::floor(255.0 - value);
    }
    return std::min(255.0, value + frames);
}

} // namespace

FogLooks fog_looks(const data::XmlNode& game_constants) {
    FogLooks looks;
    const auto scalar = [&](const std::string_view tag, double& target, const bool positive) {
        const data::XmlNode* node = last_child(game_constants, tag);
        if (node == nullptr) return fallback(looks, tag, "is absent");
        const auto parsed = number(node->raw_text);
        if (!parsed || (positive && *parsed <= 0.0)) return fallback(looks, tag, positive ? "is not a positive number" : "is not a number");
        target = *parsed;
    };
    if (const data::XmlNode* node = last_child(game_constants, "SpaceFOWColor")) {
        if (const auto parsed = colour(node->raw_text)) looks.colour = *parsed;
        else fallback(looks, "SpaceFOWColor", "is not a colour");
    } else {
        fallback(looks, "SpaceFOWColor", "is absent");
    }
    scalar("SpaceFOWHeight", looks.height, false);
    scalar("DesiredSpaceFOWCellSize", looks.cell_size, true);
    scalar("SpaceFOWRegrowTime", looks.regrow_seconds, true);
    if (const data::XmlNode* node = last_child(game_constants, "SpaceReinforceFOWColor")) {
        if (const auto parsed = colour(node->raw_text)) looks.reinforce_colour = *parsed;
        else fallback(looks, "SpaceReinforceFOWColor", "is not a colour");
    } else {
        fallback(looks, "SpaceReinforceFOWColor", "is absent");
    }
    return looks;
}

double fog_fade_step_per_frame(const double regrow_seconds) noexcept {
    // FW-05: the service step is int(238 / (seconds x 30 / 16)); each of a service's 16 frames
    // takes a sixteenth of it.
    if (!(regrow_seconds > 0.0)) return 238.0 / 16.0;
    const double step = std::floor(238.0 / (regrow_seconds * 30.0 / 16.0));
    return std::max(step, 1.0) / 16.0;
}

std::optional<FogFieldLayout> fog_field_layout(const double min_x, const double max_x, const double min_y,
                                               const double max_y, const double cell) {
    if (!std::isfinite(min_x) || !std::isfinite(max_x) || !std::isfinite(min_y) || !std::isfinite(max_y)
        || !std::isfinite(cell) || !(cell > 0.0) || !(max_x > min_x) || !(max_y > min_y)) {
        return std::nullopt;
    }
    const double wide = std::ceil((max_x - min_x) / cell);
    const double tall = std::ceil((max_y - min_y) / cell);
    if (wide > max_cells || tall > max_cells) return std::nullopt;
    FogFieldLayout layout;
    layout.cell = cell;
    layout.wide = static_cast<std::uint32_t>(wide);
    layout.tall = static_cast<std::uint32_t>(tall);
    layout.left = (min_x + max_x) / 2.0 - wide * cell / 2.0;
    layout.top = (min_y + max_y) / 2.0 + tall * cell / 2.0;
    return layout;
}

std::uint8_t fog_intensity(const double value) noexcept {
    // FW-09: FoC's table, indexed by the whole fade value.
    const double index = std::clamp(std::floor(value), 0.0, 255.0);
    if (index >= ramp_in_floor) return static_cast<std::uint8_t>(static_cast<int>((index - 237.0) / 18.0 * 255.0));
    return static_cast<std::uint8_t>(static_cast<int>(std::clamp(index * 0.0625, 0.0, 1.0) * 255.0));
}

FogField::FogField(const FogFieldLayout layout, const FogLooks& looks)
    : layout_(layout), fade_step_(fog_fade_step_per_frame(looks.regrow_seconds)) {
    // FW-11: intensity 0 takes SpaceFOWColor, 255 is clear (0, 0, 0, 0), linear between.
    for (std::size_t index = 0; index < ramp_.size(); ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            ramp_[index][channel] = static_cast<std::uint8_t>(looks.colour[channel] * (255U - index) / 255U);
            overlay_ramp_[index][channel] =
                static_cast<std::uint8_t>(looks.reinforce_colour[channel] * (255U - index) / 255U);
        }
    }
    const std::size_t cells = static_cast<std::size_t>(layout_.wide) * layout_.tall;
    values_.assign(cells, 0.0);
    held_.assign(cells, 0U);
    intensities_.assign(cells, 0U);
    blurred_.assign(cells, 0U);
    texels_.resize(cells * 4U);
    for (std::size_t cell = 0; cell < cells; ++cell) {
        std::copy(ramp_[0].begin(), ramp_[0].end(), texels_.begin() + static_cast<std::ptrdiff_t>(cell * 4U));
    }
}

void FogField::advance(const std::span<const FogFieldRevealer> revealers, double frames) {
    if (!std::isfinite(frames) || frames < 0.0) frames = 0.0;
    const std::uint32_t wide = layout_.wide;
    const std::uint32_t tall = layout_.tall;
    // FW-08: a cell is held while its centre is within a revealer's planar range (inclusive).
    std::fill(held_.begin(), held_.end(), std::uint8_t{0});
    for (const FogFieldRevealer& revealer : revealers) {
        if (!std::isfinite(revealer.x) || !std::isfinite(revealer.y) || !std::isfinite(revealer.range)
            || revealer.range < 0.0) {
            continue;
        }
        const double first_column = std::ceil((revealer.x - revealer.range - layout_.left) / layout_.cell - 0.5);
        const double last_column = std::floor((revealer.x + revealer.range - layout_.left) / layout_.cell - 0.5);
        const double first_row = std::ceil((layout_.top - (revealer.y + revealer.range)) / layout_.cell - 0.5);
        const double last_row = std::floor((layout_.top - (revealer.y - revealer.range)) / layout_.cell - 0.5);
        if (last_column < 0.0 || last_row < 0.0 || first_column >= wide || first_row >= tall) continue;
        const auto c0 = static_cast<std::uint32_t>(std::max(first_column, 0.0));
        const auto c1 = static_cast<std::uint32_t>(std::min(last_column, static_cast<double>(wide - 1U)));
        const auto r0 = static_cast<std::uint32_t>(std::max(first_row, 0.0));
        const auto r1 = static_cast<std::uint32_t>(std::min(last_row, static_cast<double>(tall - 1U)));
        const double range_squared = revealer.range * revealer.range;
        for (std::uint32_t row = r0; row <= r1; ++row) {
            const double dy = layout_.top - (row + 0.5) * layout_.cell - revealer.y;
            for (std::uint32_t column = c0; column <= c1; ++column) {
                const double dx = layout_.left + (column + 0.5) * layout_.cell - revealer.x;
                if (dx * dx + dy * dy <= range_squared) held_[static_cast<std::size_t>(row) * wide + column] = 1U;
            }
        }
    }
    held_count_ = static_cast<std::size_t>(std::count(held_.begin(), held_.end(), std::uint8_t{1}));
    for (std::size_t cell = 0; cell < values_.size(); ++cell) {
        double& value = values_[cell];
        if (held_[cell] != 0U) {
            value = held_value(value, frames);
        } else {
            // FW-10: without the session's cells a released cell fades out at the regrow ramp's
            // rate from where it shows as just clear (value 16), or from the matching point of an
            // unfinished fade-in: there is no linger to draw.
            if (value >= ramp_in_floor) value = fog_intensity(value) / 255.0 * clear_floor;
            value = std::max(0.0, value - frames * fade_step_);
        }
    }
    present();
}

void FogField::advance(const std::span<const std::uint8_t> cells, double since_service, double frames) {
    if (cells.size() != values_.size()) return;
    if (!std::isfinite(frames) || frames < 0.0) frames = 0.0;
    if (!std::isfinite(since_service) || since_service < 0.0) since_service = 0.0;
    held_count_ = 0;
    for (std::size_t cell = 0; cell < values_.size(); ++cell) {
        double& value = values_[cell];
        const std::uint8_t source = cells[cell];
        if (source == 255U) {
            // FW-08: held (or just flashed, V-19).
            value = held_value(value, frames);
            ++held_count_;
        } else if (source == 0U) {
            value = 0.0;
        } else if (value >= ramp_in_floor && value < 255.0) {
            // FW-10: a lingering cell finishes an unfinished fade-in first.
            value = std::min(255.0, value + frames);
        } else {
            // FW-10: a lingering cell shows its grid value, lowered by the FW-05 step for each
            // logical frame since the grid's last service, so the drop of a service is spread
            // over its 16 frames; it shows clear down to 16 and fades from there.
            value = std::max(0.0, static_cast<double>(source) - since_service * fade_step_);
        }
    }
    present();
}

void FogField::present() {
    const std::uint32_t wide = layout_.wide;
    const std::uint32_t tall = layout_.tall;
    for (std::size_t cell = 0; cell < values_.size(); ++cell) intensities_[cell] = fog_intensity(values_[cell]);
    if (overlay_ && blocked_) {
        // FW-22: a cell that shows more than a quarter clear (intensity above 64) is asked whether
        // it is blocked: blocked becomes fully fogged, the rest fully clear.
        for (std::uint32_t row = 0; row < tall; ++row) {
            for (std::uint32_t column = 0; column < wide; ++column) {
                std::uint8_t& intensity = intensities_[static_cast<std::size_t>(row) * wide + column];
                if (intensity <= 64U) continue;
                const double x = layout_.left + (column + 0.5) * layout_.cell;
                const double y = layout_.top - (row + 0.5) * layout_.cell;
                intensity = blocked_(x, y) ? std::uint8_t{0} : std::uint8_t{255};
            }
        }
    }
    // FW-12: the texture's outermost ring is fogged, then the inner cells take FoC's 3x3
    // (1, 2, 1) x (1, 2, 1) / 16 blur, rounded.
    std::fill(blurred_.begin(), blurred_.end(), std::uint8_t{0});
    if (wide >= 3 && tall >= 3) {
        for (std::uint32_t x = 0; x < wide; ++x) {
            intensities_[x] = 0U;
            intensities_[static_cast<std::size_t>(tall - 1U) * wide + x] = 0U;
        }
        for (std::uint32_t y = 0; y < tall; ++y) {
            intensities_[static_cast<std::size_t>(y) * wide] = 0U;
            intensities_[static_cast<std::size_t>(y) * wide + wide - 1U] = 0U;
        }
        constexpr std::array<std::uint32_t, 3> weights{1U, 2U, 1U};
        for (std::uint32_t y = 1; y + 1 < tall; ++y) {
            for (std::uint32_t x = 1; x + 1 < wide; ++x) {
                std::uint32_t sum = 8U;
                for (std::uint32_t dy = 0; dy < 3; ++dy) {
                    for (std::uint32_t dx = 0; dx < 3; ++dx) {
                        sum += weights[dy] * weights[dx]
                            * intensities_[static_cast<std::size_t>(y + dy - 1U) * wide + (x + dx - 1U)];
                    }
                }
                blurred_[static_cast<std::size_t>(y) * wide + x] = static_cast<std::uint8_t>(sum >> 4U);
            }
        }
    }
    changed_ = false;
    for (std::size_t cell = 0; cell < blurred_.size(); ++cell) {
        const auto& colour = (overlay_ ? overlay_ramp_ : ramp_)[blurred_[cell]];
        auto* texel = texels_.data() + cell * 4U;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (texel[channel] != colour[channel]) {
                texel[channel] = colour[channel];
                changed_ = true;
            }
        }
    }
}

void FogField::set_deployment_overlay(const bool on, FogBlockedPoint blocked) {
    overlay_ = on;
    blocked_ = std::move(blocked);
    present();
    changed_ = true;
}

std::uint8_t FogField::intensity(const std::uint32_t column, const std::uint32_t row) const noexcept {
    if (column >= layout_.wide || row >= layout_.tall) return 0U;
    return blurred_[static_cast<std::size_t>(row) * layout_.wide + column];
}

double FogField::value(const std::uint32_t column, const std::uint32_t row) const noexcept {
    if (column >= layout_.wide || row >= layout_.tall) return 0.0;
    return values_[static_cast<std::size_t>(row) * layout_.wide + column];
}

std::size_t FogField::fogged_cells() const noexcept {
    return static_cast<std::size_t>(std::count(blurred_.begin(), blurred_.end(), std::uint8_t{0}));
}

} // namespace eawr::presentation::space
