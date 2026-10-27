#include "session_impl.hpp"
#include "checkpoint_fields_internal.hpp"
#include "land_checkpoint_fields_internal.hpp"
#include "tactical_internal.hpp"
#include "eawr/checkpoint/checkpoint.hpp"

namespace eawr::sim::tactical {
namespace {
// Coordinator-reserved section 18, version 6. Also frames the land-only canonical tail.
constexpr auto section_id = static_cast<std::uint16_t>(checkpoint::SectionId::land_state);
constexpr std::uint16_t section_version = 6;
}

namespace {
// Appends rows to a retained buffer through the same field lists the whole-section writer uses.
template <class Rows>
void encode_rows(std::vector<std::uint8_t>& buffer, const Rows& rows, const std::size_t begin, const std::size_t end) {
    checkpoint_detail::Writer out;
    out.bytes.swap(buffer);
    for (auto i = begin; i < end; ++i) out(rows[i]);
    out.bytes.swap(buffer);
}
void append_parts(std::vector<std::uint8_t>& bytes, const std::array<std::vector<std::uint8_t>, tick_partition_count>& parts) {
    for (const auto& part : parts) {
        // Same aggregate budget the serial writer enforces byte by byte.
        if (part.size() > checkpoint_detail::byte_limit || bytes.size() > checkpoint_detail::byte_limit - part.size())
            throw checkpoint_detail::Invalid("state exceeds byte budget");
        bytes.insert(bytes.end(), part.begin(), part.end());
    }
}
}

// Worker phase: partition-owned slices of the large land record vectors, in ascending index order.
void TacticalSession::Impl::encode_land_partition(const std::size_t partition) const {
    auto& state = *land;
    const auto rows = state.occupancy.occupations();
    const auto& moving = state.movement.state();
    state.occupation_parts[partition].clear(); state.squad_parts[partition].clear(); state.member_parts[partition].clear();
    auto range = partition_range(partition, rows.size());
    encode_rows(state.occupation_parts[partition], rows, range.begin, range.end);
    range = partition_range(partition, moving.squads.size());
    encode_rows(state.squad_parts[partition], moving.squads, range.begin, range.end);
    range = partition_range(partition, moving.members.size());
    encode_rows(state.member_parts[partition], moving.members, range.begin, range.end);
}

// Byte layout is the field list (clear_bit, occupancy{occupations, changed, revision}, movement{...}, requests, searches).
// With `partitioned` the record vectors come from encode_land_partition, concatenated in ascending partition order.
std::vector<std::uint8_t> TacticalSession::Impl::land_bytes(const bool partitioned) const {
    if (!land) return {};
    auto& state = *land;
    const auto rows = state.occupancy.occupations();
    const auto& moving = state.movement.state();
    checkpoint_detail::Writer out;
    out.bytes.reserve(state.canonical_size_hint);
    const auto records = [&](const auto& all, const auto& parts) {
        out(static_cast<std::uint64_t>(all.size()));
        if (partitioned) { append_parts(out.bytes, parts); return; }
        for (const auto& row : all) out(row);
    };
    out(section_id, section_version, state.occupancy.terrain().identity(), state.content.clear_bit);
    records(rows, state.occupation_parts);
    const auto changed = state.occupancy.changed_cells();
    out(static_cast<std::uint64_t>(changed.size()));
    for (const auto cell : changed) out(cell);
    out(state.occupancy.revision(), moving.tick);
    records(moving.squads, state.squad_parts);
    records(moving.members, state.member_parts);
    out(moving.contact_settings, moving.contacts, moving.contact_complexity, moving.last_contact_frame,
        moving.contacts_serviced, state.requests, state.combat_state);
    out(static_cast<std::uint64_t>(state.searches.size()));
    for (const auto& [id, search] : state.searches) out(id, LandPathService::checkpoint(search));
    state.canonical_size_hint = out.bytes.size();
    return std::move(out.bytes);
}

core::Result<std::vector<std::uint8_t>> TacticalSession::save_land_checkpoint_state() const {
    try { return core::Result<std::vector<std::uint8_t>>::success(impl_->land_bytes(false)); }
    catch (const checkpoint_detail::Invalid& error) {
        return core::Result<std::vector<std::uint8_t>>::failure(detail::diagnostic(diagnostic_codes::resource_limit, error.what()));
    }
}

core::Result<void> TacticalSession::Impl::restore_land(const std::span<const std::uint8_t> bytes) {
    using checkpoint_detail::Invalid;
    try {
        if (!land || bytes.empty()) throw Invalid("land section and session domain disagree");
        checkpoint_detail::Reader in(bytes);
        std::uint16_t id{}, version{};
        std::array<std::uint8_t, 32> terrain_identity{};
        std::uint32_t clear_bit{};
        LandOccupancy::Checkpoint occupancy;
        LandMovement::Checkpoint moving;
        std::map<EntityId, LandPathRequest> requests;
        session_detail::LandCombatState combat_state;
        in(id, version, terrain_identity, clear_bit, occupancy, moving, requests, combat_state);
        if (id != section_id || version != section_version || terrain_identity != land->occupancy.terrain().identity()
            || clear_bit != land->content.clear_bit || moving.tick != completed_tick)
            throw Invalid("land section terrain, version or tick disagrees with session");
        const auto original = land->movement.checkpoint();
        if (moving.members.size() > original.members.size() || moving.squads.size() > original.squads.size()
            || moving.contact_settings != land->content.contacts || requests.size() != moving.squads.size()
            || combat_state.targets.size() != moving.squads.size())
            throw Invalid("land section roster disagrees with content");
        const auto live = sorted_live();
        const auto alive = [&](EntityId object) {
            const auto unit = std::lower_bound(live.begin(), live.end(), object,
                [](const auto& row, EntityId id) { return row.state.entity_id < id; });
            return unit != live.end() && unit->state.entity_id == object;
        };
        // Version 6 permits deaths, never new identities or rebindings. Immutable slots survive
        // squad-index compaction; compare the bound squad identity rather than its old index.
        for (const auto& expected : land->content.occupations) {
            const auto saved = std::find_if(occupancy.occupations.begin(), occupancy.occupations.end(),
                [&](const auto& row) { return row.object == expected.object; });
            if (alive(expected.object) ? saved == occupancy.occupations.end() || *saved != expected
                                      : saved != occupancy.occupations.end())
                throw Invalid("land static occupation disagrees with live content");
        }
        for (const auto& saved : occupancy.occupations) {
            const auto member = std::find_if(moving.members.begin(), moving.members.end(),
                [&](const auto& row) { return row.object == saved.object; });
            const auto fixed = std::find_if(land->content.occupations.begin(), land->content.occupations.end(),
                [&](const auto& row) { return row.object == saved.object; });
            if (!alive(saved.object) || (member == moving.members.end() && fixed == land->content.occupations.end()))
                throw Invalid("land section contains an unbound occupation");
        }
        EntityId last_member{};
        for (const auto& saved : moving.members) {
            const auto source = std::lower_bound(original.members.begin(), original.members.end(), saved.object,
                [](const auto& row, EntityId id) { return row.object < id; });
            if (saved.object <= last_member || source == original.members.end() || source->object != saved.object
                || saved.squad >= moving.squads.size() || saved.source != source->source
                || moving.squads[saved.squad].object != original.squads[source->squad].object
                || saved.profile != source->profile || saved.mask != source->mask || saved.slot != source->slot)
                throw Invalid("land member binding disagrees with content");
            last_member = saved.object;
        }
        for (const auto& saved : moving.squads) {
            const auto source = std::find_if(original.squads.begin(), original.squads.end(),
                [&](const auto& row) { return row.object == saved.object; });
            const auto request = requests.find(saved.object);
            const auto bound = land->requests.find(saved.object);
            const auto target = combat_state.targets.find(saved.object);
            const auto index = static_cast<std::size_t>(&saved - moving.squads.data());
            if (source == original.squads.end() || saved.maximum_speed != source->maximum_speed
                || saved.has_formation != source->has_formation || saved.generic_team != source->generic_team
                || request == requests.end() || bound == land->requests.end() || target == combat_state.targets.end()
                || (target->second.first && !land->bindings.contains(target->second.first))
                || std::none_of(moving.members.begin(), moving.members.end(),
                    [index](const auto& row) { return row.squad == index; })
                || request->second.entity != saved.object || request->second.mask != bound->second.mask
                || request->second.radius != bound->second.radius || request->second.width != bound->second.width)
                throw Invalid("land squad/path/combat binding disagrees with content");
        }
        auto field = LandOccupancy::from_checkpoint(land->occupancy.terrain_handle(), clear_bit, occupancy);
        // Terrain is immutable; searches below reference the new stable field, never the old host.
        if (!field) throw Invalid(field.error().message);
        auto restored = std::make_unique<session_detail::LandSession>(land->content, std::move(field).value());
        auto movement = LandMovement::from_checkpoint(std::move(moving));
        if (!movement) throw Invalid("invalid retained land movement or contacts");
        restored->movement = std::move(*movement); restored->requests = std::move(requests);
        restored->combat_state = std::move(combat_state);
        restored->target_query_margin = land->target_query_margin;
        restored->bindings = land->bindings;
        for (const auto& [object, pose] : land->static_roots)
            if (alive(object)) restored->static_roots.emplace(object, pose);
        const auto count = in.count();
        EntityId previous{};
        for (std::uint64_t i = 0; i < count; ++i) {
            EntityId object{};
            LandSearch::Checkpoint search;
            in(object, search);
            const auto request = restored->requests.find(object);
            if (object <= previous || request == restored->requests.end() || search.request != request->second)
                throw Invalid("land search has no matching squad request");
            previous = object;
            auto result = LandPathService::restore(restored->occupancy, restored->arena, search);
            if (!result) throw Invalid(result.error().message);
            restored->searches.emplace(object, std::move(result).value());
        }
        if (in.remaining()) throw Invalid("trailing land section bytes");
        if (live.size() != restored->movement.members().size() + restored->movement.squads().size() + restored->static_roots.size())
            throw Invalid("land section and child-state object counts disagree");
        for (const auto& unit : live) {
            const auto binding = restored->bindings.find(unit.state.entity_id);
            if (binding == restored->bindings.end() || binding->second != std::pair{unit.state.type_id, unit.state.owner})
                throw Invalid("land child-state identity disagrees with company expansion");
            if (const auto source = restored->static_roots.find(unit.state.entity_id); source != restored->static_roots.end()) {
                if (source->second.position != unit.state.position || source->second.rotation != unit.state.rotation)
                    throw Invalid("land static pose disagrees with setup");
            }
        }
        const auto check_position = [&](const auto& row) {
            const auto unit = std::lower_bound(live.begin(), live.end(), row.object,
                [](const auto& value, EntityId key) { return value.state.entity_id < key; });
            if (unit == live.end() || unit->state.entity_id != row.object
                || unit->state.position.x != row.position.x || unit->state.position.y != row.position.y)
                throw Invalid("land section and child-state poses disagree");
        };
        for (const auto& member : restored->movement.members()) check_position(member);
        for (const auto& squad : restored->movement.squads()) check_position(squad);
        land = std::move(restored);
        return core::Result<void>::success();
    } catch (const Invalid& error) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, error.what()));
    }
}

core::Result<void> TacticalSession::load_land_checkpoint_state(const std::span<const std::uint8_t> bytes) {
    if (impl_->object_turn_running) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
        "land checkpoint restore during object-turn service"));
    return impl_->restore_land(bytes);
}

} // namespace eawr::sim::tactical
