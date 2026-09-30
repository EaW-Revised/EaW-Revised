#include "eawr/presentation/fog/fog.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace eawr::presentation::fog {
namespace {

using sim::fog::FogGrid;
using sim::fog::FogGridDesc;

[[nodiscard]] core::Diagnostic fog_diagnostic(const std::string_view code, std::string message) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("fog-stub-v1"),
    };
}

[[nodiscard]] std::string describe(const StreamTeam key) {
    return "stream " + std::to_string(key.stream) + " team " + std::to_string(key.team);
}

// Index of `value` in the half-open run [origin, origin + count*cell), or
// nullopt. Unsigned subtraction avoids signed overflow once value >= origin.
[[nodiscard]] std::optional<std::uint32_t> axis_index(
    const std::int64_t value,
    const std::int64_t origin,
    const std::int64_t cell,
    const std::uint32_t count) noexcept {
    if (value < origin || cell <= 0) return std::nullopt;
    const auto delta = static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(origin);
    const auto index = delta / static_cast<std::uint64_t>(cell);
    if (index >= count) return std::nullopt;
    return static_cast<std::uint32_t>(index);
}

[[nodiscard]] TextureSpec spec_for(const FogGrid& grid) noexcept {
    return {grid.desc().width, grid.desc().height};
}

[[nodiscard]] bool same_pixels(const FogGrid& left, const FogGrid& right) noexcept {
    const auto a = left.cells();
    const auto b = right.cells();
    return a.data() == b.data() || std::equal(a.begin(), a.end(), b.begin(), b.end());
}

} // namespace

FogMapping mapping_for(const FogGridDesc& desc) noexcept {
    return FogMapping{
        .origin_x = q24_to_double(desc.origin_x_raw),
        .origin_y = q24_to_double(desc.origin_y_raw),
        .extent_x = q24_to_double(desc.cell_x_raw) * static_cast<double>(desc.width),
        .extent_y = q24_to_double(desc.cell_y_raw) * static_cast<double>(desc.height),
        .width = desc.width,
        .height = desc.height,
    };
}

std::optional<CellIndex> cell_at_raw(
    const FogGridDesc& desc,
    const std::int64_t source_x_raw,
    const std::int64_t source_y_raw) noexcept {
    const auto x = axis_index(source_x_raw, desc.origin_x_raw, desc.cell_x_raw, desc.width);
    const auto y = axis_index(source_y_raw, desc.origin_y_raw, desc.cell_y_raw, desc.height);
    if (!x || !y) return std::nullopt;
    return CellIndex{*x, *y};
}

std::uint8_t attenuation_at_raw(
    const FogGrid& grid,
    const std::int64_t source_x_raw,
    const std::int64_t source_y_raw) noexcept {
    const auto cell = cell_at_raw(grid.desc(), source_x_raw, source_y_raw);
    if (!cell) return 0;
    return grid.cell(cell->x, cell->y).value_or(0);
}

std::uint8_t attenuation_at(const FogGrid& grid, const SourcePoint source) noexcept {
    const FogMapping mapping = mapping_for(grid.desc());
    const double u = (source.x - mapping.origin_x) / mapping.extent_x;
    const double v = (source.y - mapping.origin_y) / mapping.extent_y;
    // Bounds first, exactly as the synthetic shader; only then clamp the index.
    if (!(u >= 0.0 && v >= 0.0 && u < 1.0 && v < 1.0)) return 0;
    const auto x = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(std::floor(u * mapping.width)), mapping.width - 1);
    const auto y = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(std::floor(v * mapping.height)), mapping.height - 1);
    return grid.cell(x, y).value_or(0);
}

std::string_view to_string(const SubmitAction action) noexcept {
    switch (action) {
    case SubmitAction::unchanged: return "unchanged";
    case SubmitAction::created: return "created";
    case SubmitAction::updated: return "updated";
    case SubmitAction::recreated: return "recreated";
    case SubmitAction::metadata_only: return "metadata_only";
    case SubmitAction::reselected: return "reselected";
    }
    return "unknown";
}

TextureCache::TextureCache(TextureBackend& backend) noexcept : backend_(backend) {}

TextureCache::~TextureCache() {
    reset();
}

const FogGrid* TextureCache::accepted(const StreamTeam key) const noexcept {
    const auto found = entries_.find(key);
    return found == entries_.end() ? nullptr : &found->second.grid;
}

TextureHandle TextureCache::texture(const StreamTeam key) const noexcept {
    const auto found = entries_.find(key);
    return found == entries_.end() ? TextureHandle{} : found->second.texture;
}

core::Diagnostic TextureCache::reject(
    const std::string_view code,
    std::string message,
    const StreamTeam selection,
    const bool keep_binding) {
    ++stats_.rejected;
    if (!keep_binding || active_ != selection) clear_binding();
    return fog_diagnostic(code, describe(selection) + ": " + std::move(message));
}

void TextureCache::clear_binding() {
    if (bound_) {
        for (const ConsumerId consumer : consumers_) {
            backend_.unbind(consumer);
            ++stats_.unbinds;
        }
    }
    bound_.reset();
    active_.reset();
}

void TextureCache::activate(const StreamTeam selection, const Entry& entry) {
    const Binding wanted{entry.texture, mapping_for(entry.grid.desc())};
    active_ = selection;
    if (bound_ == wanted) return;
    for (const ConsumerId consumer : consumers_) {
        backend_.bind(consumer, wanted.texture, wanted.mapping);
        ++stats_.binds;
    }
    bound_ = wanted;
}

core::Result<SubmitAction> TextureCache::submit(
    const sim::fog::FogGridSet& grids,
    const StreamTeam selection) {
    using Result = core::Result<SubmitAction>;
    const FogGrid* incoming = grids.find(selection.team);
    if (incoming == nullptr) {
        return Result::failure(reject(
            diagnostic_codes::missing_team, "selected team has no grid in the submitted set",
            selection, false));
    }
    const auto pixels = incoming->cells();

    const auto found = entries_.find(selection);
    if (found == entries_.end()) {
        const TextureHandle created = backend_.create_texture(spec_for(*incoming), pixels);
        if (created == TextureHandle{}) {
            return Result::failure(reject(
                diagnostic_codes::backend_failure,
                "texture creation failed: " + backend_.failure_cause(), selection, true));
        }
        ++stats_.creates;
        ++stats_.uploads;
        stats_.upload_bytes += pixels.size();
        const auto inserted = entries_.emplace(selection, Entry{*incoming, created}).first;
        activate(selection, inserted->second);
        return Result::success(SubmitAction::created);
    }

    Entry& entry = found->second;
    const FogGrid& previous = entry.grid;
    if (incoming->revision() < previous.revision()) {
        return Result::failure(reject(
            diagnostic_codes::revision_rollback,
            "revision " + std::to_string(incoming->revision()) + " is older than accepted revision "
                + std::to_string(previous.revision()),
            selection, true));
    }
    if (incoming->revision() == previous.revision()) {
        if (!(*incoming == previous)) {
            return Result::failure(reject(
                diagnostic_codes::revision_conflict,
                "revision " + std::to_string(incoming->revision())
                    + " was already accepted with different content",
                selection, true));
        }
        if (active_ == selection) return Result::success(SubmitAction::unchanged);
        ++stats_.reselects;
        activate(selection, entry);
        return Result::success(SubmitAction::reselected);
    }

    SubmitAction action = SubmitAction::metadata_only;
    if (spec_for(*incoming) != spec_for(previous)) {
        const TextureHandle replacement = backend_.create_texture(spec_for(*incoming), pixels);
        if (replacement == TextureHandle{}) {
            return Result::failure(reject(
                diagnostic_codes::backend_failure,
                "texture recreation failed: " + backend_.failure_cause(), selection, true));
        }
        // Rebind consumers to the replacement before releasing the old texture.
        const TextureHandle retired = entry.texture;
        entry.texture = replacement;
        entry.grid = *incoming;
        activate(selection, entry);
        backend_.destroy_texture(retired);
        ++stats_.destroys;
        ++stats_.creates;
        ++stats_.recreates;
        ++stats_.uploads;
        stats_.upload_bytes += pixels.size();
        return Result::success(SubmitAction::recreated);
    }
    if (!same_pixels(*incoming, previous)) {
        if (!backend_.update_texture(entry.texture, pixels)) {
            return Result::failure(reject(
                diagnostic_codes::backend_failure,
                "texture update failed: " + backend_.failure_cause(), selection, true));
        }
        ++stats_.updates;
        ++stats_.uploads;
        stats_.upload_bytes += pixels.size();
        action = SubmitAction::updated;
    } else {
        ++stats_.metadata_only;
    }
    entry.grid = *incoming;
    activate(selection, entry);
    return Result::success(action);
}

core::Result<void> TextureCache::add_consumer(const ConsumerId consumer) {
    if (consumer == ConsumerId{} || consumers_.contains(consumer)) {
        return core::Result<void>::failure(fog_diagnostic(
            diagnostic_codes::invalid_consumer,
            "consumer " + std::to_string(consumer) + " is zero or already registered"));
    }
    consumers_.insert(consumer);
    // A late consumer receives the current binding immediately.
    if (bound_) {
        backend_.bind(consumer, bound_->texture, bound_->mapping);
        ++stats_.binds;
    }
    return core::Result<void>::success();
}

core::Result<void> TextureCache::remove_consumer(const ConsumerId consumer) {
    if (!consumers_.contains(consumer)) {
        return core::Result<void>::failure(fog_diagnostic(
            diagnostic_codes::invalid_consumer,
            "consumer " + std::to_string(consumer) + " is not registered"));
    }
    if (bound_) {
        backend_.unbind(consumer);
        ++stats_.unbinds;
    }
    consumers_.erase(consumer);
    return core::Result<void>::success();
}

void TextureCache::reset_stream(const std::uint64_t stream) {
    if (active_ && active_->stream == stream) clear_binding();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.stream != stream) {
            ++it;
            continue;
        }
        backend_.destroy_texture(it->second.texture);
        ++stats_.destroys;
        it = entries_.erase(it);
    }
}

void TextureCache::reset() {
    clear_binding();
    for (const auto& [key, entry] : entries_) {
        backend_.destroy_texture(entry.texture);
        ++stats_.destroys;
    }
    entries_.clear();
}

} // namespace eawr::presentation::fog
