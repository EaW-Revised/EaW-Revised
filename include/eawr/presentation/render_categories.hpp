#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <ostream>
#include <cmath>
#include <unordered_set>

namespace eawr::presentation::render_categories {

// Presentation provenance, independent of asset identity and simulation state.
enum class Role : std::uint8_t { unknown, hull, hardpoint, effect, particle, sky, map, debris, wreck, count };
enum class HullPart : std::uint8_t { unclassified, body, turret };
inline constexpr std::size_t role_count = static_cast<std::size_t>(Role::count);
inline constexpr std::array<std::string_view, role_count> names{
    "unknown", "hull", "hardpoint", "effect", "particle", "sky", "map", "debris", "wreck"};

// Startup-only diagnostic selection; never changes authoritative visibility.
// `none` is the instrumented control. Environment/HUD/projectile paths are not separable.
inline std::string_view ablation{"off"};
inline constexpr std::array<std::string_view, 19> experiments{
    "fighter-control", "fighter-hull", "fighter-all", "fighter-shadows",
    "fighter-impostor-control", "fighter-impostor-008",
    "hardpoint-band-005", "hardpoint-band-010", "hardpoint-band-020",
    "particle-band-005", "particle-band-010", "particle-band-020",
    "shadows-band-005", "shadows-band-010", "shadows-band-020", "groups", "integrated-turret", "other-hull", "hull-census"};
[[nodiscard]] inline double band_cutoff() noexcept {
    if (ablation.ends_with("-band-005")) return 0.005;
    if (ablation.ends_with("-band-010")) return 0.01;
    if (ablation.ends_with("-band-020")) return 0.02;
    return 0;
}
// Ten percent exit hysteresis; unknown/near-plane bounds always remain visible.
[[nodiscard]] inline bool band_hidden(double coverage, bool previous) noexcept {
    const auto cutoff = band_cutoff();
    return cutoff > 0 && std::isfinite(coverage) && coverage > 0
        && coverage < cutoff * (previous ? 1.1 : 1.0);
}
[[nodiscard]] inline double projected_diameter(double radius, double depth, double near_plane, double tangent,
                                              double lateral = 0) noexcept {
    if (!std::isfinite(depth) || !std::isfinite(radius) || !std::isfinite(near_plane)
        || !std::isfinite(lateral) || lateral < 0 || radius <= 0 || depth <= radius
        || depth - radius <= near_plane || tangent <= 0 || !std::isfinite(tangent)) return -1;
    // Bound perspective expansion away from the optical axis as well as at
    // nearest depth; a centered-sphere estimate alone can underestimate it.
    return radius * (1 + lateral / depth) / ((depth - radius) * tangent);
}
[[nodiscard]] inline bool valid_ablation(std::string_view name) noexcept {
    for (const auto experiment : experiments) if (name == experiment) return true;
    return name == "none" || name == "hull" || name == "hardpoint" || name == "effect"
        || name == "particle" || name == "debris" || name == "wreck" || name == "shadows";
}
[[nodiscard]] inline bool engine_visible(Role role, bool requested) noexcept {
    const auto index = static_cast<std::size_t>(role);
    return requested && (ablation == "off" || index >= role_count || ablation != names[index]);
}
[[nodiscard]] inline bool engine_casts(bool requested) noexcept {
    return requested && ablation != "shadows" && ablation != "fighter-shadows";
}

[[nodiscard]] inline bool fighter_experiment() noexcept { return ablation.starts_with("fighter-"); }
[[nodiscard]] inline bool fighter_impostor_experiment() noexcept { return ablation.starts_with("fighter-impostor-"); }
// Diagnostic threshold only: composed conservative sphere diameter in pixels.
// Unknown/near-plane bounds never qualify. This is not a production LOD policy.
[[nodiscard]] inline bool fighter_impostor_small(double coverage, std::uint32_t height) noexcept {
    return height > 0 && std::isfinite(coverage) && coverage > 0 && coverage * height < 8.0;
}
[[nodiscard]] inline bool fighter_hidden(bool fighter, Role role) noexcept {
    return fighter && (ablation == "fighter-all" || (ablation == "fighter-hull" && role == Role::hull));
}
// Authored tokens only; never infer a class from an object/model name.
[[nodiscard]] inline bool authored_fighter(std::string_view categories) noexcept {
    while (!categories.empty()) {
        const auto begin = categories.find_first_not_of(" ,\t\r\n");
        if (begin == std::string_view::npos) return false;
        categories.remove_prefix(begin);
        const auto end = categories.find_first_of(" ,\t\r\n");
        const auto token = categories.substr(0, end);
        const auto equal = [token](std::string_view word) {
            if (token.size() != word.size()) return false;
            for (std::size_t i = 0; i < token.size(); ++i) {
                const char c = token[i] >= 'a' && token[i] <= 'z' ? static_cast<char>(token[i] - 'a' + 'A') : token[i];
                if (c != word[i]) return false;
            }
            return true;
        };
        if (equal("FIGHTER") || equal("BOMBER")) return true;
        if (end == std::string_view::npos) return false;
        categories.remove_prefix(end);
    }
    return false;
}
// Simulation entity IDs, explicitly populated from the presentation slot binding.
// A renderer owner slot must never be assumed to be a simulation entity ID.
inline std::unordered_set<std::uint64_t> fighter_entities;
struct FighterWork final {
    std::uint64_t idle_samples{}, clip_samples{};
    double sample_cpu_ms{}, palette_cpu_ms{}, binding_cpu_ms{};
    std::uint64_t bindings{}, unit_emitters{};
};
inline FighterWork fighter_work;
inline std::uint64_t fighter_work_frame{~std::uint64_t{0}};
inline void begin_fighter_frame(std::uint64_t frame) noexcept {
    if (fighter_work_frame != frame) { fighter_work = {}; fighter_work_frame = frame; }
}

struct Counts final {
    std::uint64_t submitted{}, retained{}, eligible_casters{}, retained_surfaces{}, retained_triangles{}, material_incidence{}, potential_shadow_surfaces{};
};
struct Sample final {
    FighterWork fighter_pose{};
    std::uint64_t fighter_units{}, fighter_hull_surfaces{}, fighter_attachment_surfaces{}, fighter_effect_surfaces{}, fighter_requested_casters{}, fighter_particle_emitters{};
    std::uint64_t fighter_impostor_candidates{}, fighter_impostor_quads{};
    std::uint64_t eligible_units{}, compatible_groups{}, grouped_units{}, potential_colour_saving{}, group_hidden_units{};
    std::uint64_t band_hidden_instances{}, unclassified_hull_surfaces{}, named_hull_surfaces{};
    std::uint64_t declared_turret_surfaces{}, other_hull_surfaces{};
    std::uint64_t frame{};
    std::uint64_t viewport{};
    std::array<Counts, role_count> roles{};
    // Actual root viewport draws from the completed engine frame, not role estimates.
    std::array<std::uint64_t, 3> viewport_draws{}; // visible, shadow, canvas/UI
    double sample_ms{};
    void record(Role role, bool submitted, bool casts, std::uint64_t surfaces, std::uint64_t triangles = 0, std::uint64_t materials = 0) noexcept {
        const auto index = static_cast<std::size_t>(role);
        auto& counts = roles[index < role_count ? index : 0];
        ++counts.retained;
        counts.submitted += submitted ? 1U : 0U;
        counts.eligible_casters += casts ? 1U : 0U;
        counts.retained_surfaces += surfaces;
        counts.retained_triangles += triangles;
        counts.material_incidence += materials;
        if (casts) counts.potential_shadow_surfaces += surfaces;
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
            << ",\"retained_surfaces\":" << c.retained_surfaces;
        if (ablation != "off") out << ",\"retained_triangles\":" << c.retained_triangles
            << ",\"material_incidence\":" << c.material_incidence
            << ",\"potential_shadow_surfaces\":" << c.potential_shadow_surfaces;
        out << '}';
    }
    out << "}";
    if (fighter_experiment()) out << ",\"fighter_pose\":{\"idle_samples\":" << sample.fighter_pose.idle_samples
        << ",\"clip_samples\":" << sample.fighter_pose.clip_samples << ",\"sample_cpu_ms\":" << sample.fighter_pose.sample_cpu_ms
        << ",\"palette_cpu_ms\":" << sample.fighter_pose.palette_cpu_ms
        << ",\"binding_cpu_ms\":" << sample.fighter_pose.binding_cpu_ms << ",\"bindings\":" << sample.fighter_pose.bindings
        << ",\"particle_emitters\":" << sample.fighter_particle_emitters
        << ",\"unit_emitters\":" << sample.fighter_pose.unit_emitters
        << ",\"units\":" << sample.fighter_units << ",\"hull_surfaces\":" << sample.fighter_hull_surfaces
        << ",\"attachment_surfaces\":" << sample.fighter_attachment_surfaces << ",\"effect_surfaces\":" << sample.fighter_effect_surfaces
        << ",\"requested_casters\":" << sample.fighter_requested_casters
        << ",\"impostor_candidates\":" << sample.fighter_impostor_candidates
        << ",\"impostor_quads\":" << sample.fighter_impostor_quads << '}';
    if (ablation != "off") out << ",\"ablation\":\"" << ablation << '"';
    if (ablation != "off") out << ",\"diagnostic\":{\"cutoff\":" << band_cutoff()
        << ",\"band_hidden_instances\":" << sample.band_hidden_instances
        << ",\"eligible_units\":" << sample.eligible_units
        << ",\"compatible_groups\":" << sample.compatible_groups
        << ",\"grouped_units\":" << sample.grouped_units
        << ",\"potential_colour_saving\":" << sample.potential_colour_saving
        << ",\"group_hidden_units\":" << sample.group_hidden_units
        << ",\"unclassified_hull_surfaces\":" << sample.unclassified_hull_surfaces
        << ",\"named_hull_surfaces\":" << sample.named_hull_surfaces
        << ",\"declared_turret_surfaces\":" << sample.declared_turret_surfaces
        << ",\"other_hull_surfaces\":" << sample.other_hull_surfaces << '}';
    out << "}";
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
        if (fighter_work_frame == frame) sample.fighter_pose = fighter_work;
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
