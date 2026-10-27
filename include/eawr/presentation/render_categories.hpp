#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <ostream>

namespace eawr::presentation::render_categories {

// Presentation provenance, independent of asset identity and simulation state.
enum class Role : std::uint8_t { unknown, hull, hardpoint, effect, particle, sky, map, debris, wreck, count };
inline constexpr std::size_t role_count = static_cast<std::size_t>(Role::count);
inline constexpr std::array<std::string_view, role_count> names{
    "unknown", "hull", "hardpoint", "effect", "particle", "sky", "map", "debris", "wreck"};

struct Counts final {
    std::uint64_t submitted{}, retained{}, eligible_casters{}, retained_surfaces{};
};
struct Sample final {
    std::uint64_t frame{};
    std::uint64_t viewport{};
    std::array<Counts, role_count> roles{};
    // Actual root viewport draws from the completed engine frame, not role estimates.
    std::array<std::uint64_t, 3> viewport_draws{}; // visible, shadow, canvas/UI
    double sample_ms{};
    void record(Role role, bool submitted, bool casts, std::uint64_t surfaces) noexcept {
        const auto index = static_cast<std::size_t>(role);
        auto& counts = roles[index < role_count ? index : 0];
        ++counts.retained;
        counts.submitted += submitted ? 1U : 0U;
        counts.eligible_casters += casts ? 1U : 0U;
        counts.retained_surfaces += surfaces;
    }
};

// Main/render-submission thread only, like the renderer APIs these diagnostics observe.
// Intrusive registration allocates nothing; disabled frames never enumerate sources.
inline bool enabled{};
inline Sample latest{};
inline void write_json(std::ostream& out, const Sample& sample) {
    out << "{\"frame\":" << sample.frame << ",\"sample_ms\":" << sample.sample_ms
        << ",\"viewport\":" << sample.viewport
        << ",\"viewport_draws\":{\"visible\":" << sample.viewport_draws[0]
        << ",\"shadow\":" << sample.viewport_draws[1] << ",\"canvas\":" << sample.viewport_draws[2]
        << "},\"roles\":{";
    for (std::size_t index = 0; index < role_count; ++index) {
        const auto& c = sample.roles[index];
        out << (index ? "," : "") << '"' << names[index] << "\":{\"submitted\":" << c.submitted
            << ",\"retained\":" << c.retained << ",\"eligible_casters\":" << c.eligible_casters
            << ",\"retained_surfaces\":" << c.retained_surfaces << '}';
    }
    out << "}}";
}
class Source final {
public:
    using Reader = void (*)(const void*, Sample&);
    using Scope = std::uint64_t (*)(const void*);
    Source(const void* owner, Reader reader, Scope scope) noexcept
        : owner_(owner), reader_(reader), scope_(scope), next_(head_) { head_ = this; }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    ~Source() {
        auto** link = &head_;
        while (*link && *link != this) link = &(*link)->next_;
        if (*link) *link = next_;
    }
    [[nodiscard]] static Sample capture(std::uint64_t frame, std::uint64_t viewport) {
        Sample sample;
        sample.frame = frame;
        sample.viewport = viewport;
        if (enabled) for (auto* source = head_; source; source = source->next_)
            if (source->scope_(source->owner_) == viewport) source->reader_(source->owner_, sample);
        return sample;
    }
private:
    const void* owner_;
    Reader reader_;
    Scope scope_;
    Source* next_;
    static inline Source* head_{};
};

} // namespace eawr::presentation::render_categories
