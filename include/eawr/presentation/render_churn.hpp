#pragma once

#include "eawr/presentation/render_categories.hpp"

#include <array>
#include <bit>
#include <compare>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <ostream>

namespace eawr::presentation::render_churn {

// Diagnostic ownership, never a draw or lifetime policy. Shared model resources
// are separate because one upload can serve a hull, a wreck and a debris prop.
enum class Owner : std::uint8_t {
    particle, instance, skeleton, debris, debris_skeleton, wreck, wreck_skeleton,
    model_resource, debris_particle, scene_resource, count
};
inline constexpr std::array names{"particle", "instance", "skeleton", "debris", "debris_skeleton",
    "wreck", "wreck_skeleton", "model_resource", "debris_particle", "scene_resource"};
// Physical RID event counts in every column. Fresh/rebuilt RIDs are not restores.
struct Counts final { std::uint64_t freed{}, retained{}, restored{}; };
using Sample = std::array<Counts, static_cast<std::size_t>(Owner::count)>;
// Caller/submission thread only. Particle prepare() workers do not create,
// release, retain or restore RIDs; their ordered caller commit owns these hooks.
inline Sample frame{}, total{};
enum class Event : std::uint8_t { freed, retained, restored };
inline void record(Owner owner, Event event, std::uint64_t count = 1) noexcept {
    if (!render_categories::enabled) return;
    const auto add = [event, count](Counts& value) {
        if (event == Event::freed) value.freed += count;
        else if (event == Event::retained) value.retained += count;
        else value.restored += count;
    };
    const auto index = static_cast<std::size_t>(owner);
    add(frame[index]); add(total[index]);
}
template<class Server, class Rid>
inline void free_rid(Server& server, const Rid& rid, Owner provenance) {
    if (rid.is_valid()) record(provenance, Event::freed);
    server.free_rid(rid);
}
[[nodiscard]] inline Owner owner(render_categories::Role role, bool skeleton = false) noexcept {
    if (role == render_categories::Role::debris) return skeleton ? Owner::debris_skeleton : Owner::debris;
    if (role == render_categories::Role::wreck) return skeleton ? Owner::wreck_skeleton : Owner::wreck;
    return skeleton ? Owner::skeleton : Owner::instance;
}
inline void write_json(std::ostream& out, const Sample& sample) {
    out << '{';
    for (std::size_t i = 0; i < sample.size(); ++i)
        out << (i ? "," : "") << '"' << names[i] << "\":{\"freed\":" << sample[i].freed
            << ",\"retained\":" << sample[i].retained << ",\"restored\":" << sample[i].restored << '}';
    out << '}';
}
inline void write_csv_header(std::ostream& out) {
    for (const auto name : names)
        for (const auto metric : {"freed", "retained", "restored"}) out << ",rid_" << name << '_' << metric;
}
inline void write_csv_row(std::ostream& out, const Sample& sample) {
    for (const auto& counts : sample) out << ',' << counts.freed << ',' << counts.retained << ',' << counts.restored;
}

// A census key describes actual submitted state. Exact float bits deliberately
// split even signed zero; this is a conservative batching census, not a policy.
struct CohortKey final {
    std::uint64_t asset{}, material{}, fog{};
    std::uint32_t pass{};
    std::int32_t priority{};
    bool skinned{}, billboard{}, casts{};
    std::array<std::uint32_t, 7> colour_opacity_light{};
    auto operator<=>(const CohortKey&) const = default;
    [[nodiscard]] bool rigid() const noexcept { return !skinned && !billboard; }
};
[[nodiscard]] inline std::uint32_t bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }

// Construct only at a report phase, never as frame scratch.
struct Census final {
    std::map<CohortKey, std::uint64_t> cohorts;
    std::uint64_t active{}, rigid{}, excluded_skin{}, excluded_billboard{}, excluded_any{};
    void add(const CohortKey& key) {
        ++cohorts[key]; ++active;
        rigid += key.rigid() ? 1U : 0U;
        excluded_skin += key.skinned ? 1U : 0U;
        excluded_billboard += key.billboard ? 1U : 0U;
        excluded_any += key.rigid() ? 0U : 1U;
    }
};

} // namespace eawr::presentation::render_churn
