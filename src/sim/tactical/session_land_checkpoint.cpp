#include "session_impl.hpp"
#include "checkpoint_fields_internal.hpp" // include-guard: keep (checkpoint archive definitions)
#include "land_checkpoint_fields_internal.hpp"
#include "session_land_garrison.hpp"
#include "tactical_internal.hpp"
#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/checkpoint/tactical.hpp"

namespace eawr::sim::tactical {
namespace {
// Coordinator-reserved section 18; the version word inside the section is the container version (one source of truth).
// Also frames the land-only canonical tail.
constexpr auto section_id = static_cast<std::uint16_t>(checkpoint::SectionId::land_state);
constexpr std::uint16_t section_version = checkpoint::land_state_section_version;
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
    // Contacts are the largest vector of the serial tail in a dense fight; same row encoder, same order.
    state.contact_parts[partition].clear();
    range = partition_range(partition, moving.contacts.size());
    encode_rows(state.contact_parts[partition], moving.contacts, range.begin, range.end);
}

// Byte layout is the field list (clear_bit, occupancy{occupations, changed, revision}, movement{...}, requests, searches).
// With `partitioned` the record vectors come from encode_land_partition, concatenated in ascending partition order.
std::vector<std::uint8_t> TacticalSession::Impl::land_bytes(const bool partitioned) const {
    if (!land) return {};
    auto& state = *land;
    const auto rows = state.occupancy.occupations();
    const auto& moving = state.movement.state();
    checkpoint_detail::Writer out;
    out.fire_range_bonuses = true; out.script_invulnerable = true; // hidden occupants carry whole live units (version 16)
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
    out(moving.contact_settings);
    records(moving.contacts, state.contact_parts);
    out(moving.contact_complexity, moving.last_contact_frame, moving.contacts_serviced, state.requests,
        state.combat_state, state.dynamic_children, state.formations, state.landings, state.landed, state.next_landing,
        state.walking_formations, state.garrison_entries);
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
        in.fire_range_bonuses = true; in.script_invulnerable = true;
        std::uint16_t id{}, version{};
        std::array<std::uint8_t, 32> terrain_identity{};
        std::uint32_t clear_bit{};
        LandOccupancy::Checkpoint occupancy;
        LandMovement::Checkpoint moving;
        std::map<EntityId, LandPathRequest> requests;
        session_detail::LandCombatState combat_state;
        std::vector<EntityId> dynamic_children;
        std::map<EntityId, LandFormationOrder> formations;
        std::vector<session_detail::LandLanding> landings;
        std::vector<EntityId> landed;
        std::uint64_t next_landing{};
        std::map<EntityId, session_detail::LandWalkingFormation> walking_formations;
        std::map<EntityId, session_detail::LandGarrisonEntry> garrison_entries;
        in(id, version, terrain_identity, clear_bit, occupancy, moving, requests, combat_state, dynamic_children, formations,
            landings, landed, next_landing, walking_formations, garrison_entries);
        if (id != section_id || version != section_version || terrain_identity != land->occupancy.terrain().identity()
            || clear_bit != land->content.clear_bit || moving.tick != completed_tick)
            throw Invalid("land section terrain, version or tick disagrees with session");
        const auto original = land->movement.checkpoint();
        // Version 11: the landed companies (LC-51) add members and squads to the setup roster; they are named by `landed`.
        if (!std::is_sorted(landed.begin(), landed.end()) || std::adjacent_find(landed.begin(), landed.end()) != landed.end()
            || moving.members.size() > original.members.size() + landed.size()
            || moving.squads.size() > original.squads.size() + landed.size()
            || moving.contact_settings != land->content.contacts || requests.size() != moving.squads.size()
            || combat_state.targets.size() != moving.squads.size())
            throw Invalid("land section roster disagrees with content");
        const auto live = sorted_live();
        const auto alive = [&](EntityId object) {
            const auto unit = std::lower_bound(live.begin(), live.end(), object,
                [](const auto& row, EntityId id) { return row.state.entity_id < id; });
            return unit != live.end() && unit->state.entity_id == object;
        };
        // Version 11 permits deaths and the economy's created children (dynamic_children), never other new identities or rebindings. Slots are mutable (WLPA-84) and are not
        // compared; the authored extents and spacing gap are. Compare the bound squad identity rather than its old index.
        for (const auto& expected : land->content.occupations) {
            const auto saved = std::find_if(occupancy.occupations.begin(), occupancy.occupations.end(),
                [&](const auto& row) { return row.object == expected.object; });
            if (alive(expected.object) ? saved == occupancy.occupations.end() || *saved != expected
                                      : saved != occupancy.occupations.end())
                throw Invalid("land static occupation disagrees with live content");
        }
        // LC-55: the economy's created objects are archived as ids; each must be a live unit that is neither a
        // setup identity nor a member or squad, and carries no occupation of its own (LE-OCCUPY).
        if (!std::is_sorted(dynamic_children.begin(), dynamic_children.end())
            || std::adjacent_find(dynamic_children.begin(), dynamic_children.end()) != dynamic_children.end())
            throw Invalid("land dynamic children are not strictly ascending");
        const auto dynamic = [&](EntityId object) {
            return std::binary_search(dynamic_children.begin(), dynamic_children.end(), object);
        };
        for (const auto child : dynamic_children) {
            const auto member = std::find_if(moving.members.begin(), moving.members.end(),
                [&](const auto& row) { return row.object == child; });
            const auto squad = std::find_if(moving.squads.begin(), moving.squads.end(),
                [&](const auto& row) { return row.object == child; });
            if (!alive(child) || member != moving.members.end() || squad != moving.squads.end()
                || land->bindings.contains(child) || land->static_roots.contains(child))
                throw Invalid("land dynamic child disagrees with live content");
        }
        for (const auto& saved : occupancy.occupations) {
            const auto member = std::find_if(moving.members.begin(), moving.members.end(),
                [&](const auto& row) { return row.object == saved.object; });
            const auto fixed = std::find_if(land->content.occupations.begin(), land->content.occupations.end(),
                [&](const auto& row) { return row.object == saved.object; });
            if (!alive(saved.object) || dynamic(saved.object)
                || (member == moving.members.end() && fixed == land->content.occupations.end()))
                throw Invalid("land section contains an unbound occupation");
        }
        // Landing records: strictly ascending, owned by a declared player, bound to a company, timed within the session.
        std::uint64_t last_landing{};
        for (const auto& landing : landings) {
            if (landing.sequence <= last_landing || landing.sequence >= next_landing || !player_of(landing.owner)
                || std::none_of(land->content.companies.begin(), land->content.companies.end(),
                    [&](const auto& row) { return row.type == landing.company; })
                || landing.finalize_frame <= landing.created_frame || landing.created_frame > moving.tick
                || landing.finalize_frame < moving.tick)
                throw Invalid("land landing record disagrees with content");
            last_landing = landing.sequence;
        }
        const auto landed_object = [&](EntityId object) { return std::binary_search(landed.begin(), landed.end(), object); };
        const auto company_member = [&](EntityId object) {
            const auto unit = std::lower_bound(live.begin(), live.end(), object,
                [](const auto& row, EntityId key) { return row.state.entity_id < key; });
            return unit != live.end() && unit->state.entity_id == object
                && std::any_of(land->content.companies.begin(), land->content.companies.end(), [&](const auto& company) {
                    return std::any_of(company.members.begin(), company.members.end(),
                        [&](const auto& member) { return member.type == unit->state.type_id; }); });
        };
        EntityId last_member{};
        for (const auto& saved : moving.members) {
            if (landed_object(saved.object)) {
                if (saved.object <= last_member || saved.squad >= moving.squads.size() || !alive(saved.object)
                    || !company_member(saved.object) || !landed_object(moving.squads[saved.squad].object))
                    throw Invalid("land landed member disagrees with content");
                last_member = saved.object;
                continue;
            }
            const auto source = std::lower_bound(original.members.begin(), original.members.end(), saved.object,
                [](const auto& row, EntityId id) { return row.object < id; });
            if (saved.object <= last_member || source == original.members.end() || source->object != saved.object
                || saved.squad >= moving.squads.size() || saved.source != source->source
                || moving.squads[saved.squad].object != original.squads[source->squad].object
                || saved.profile != source->profile || saved.mask != source->mask || saved.hard_x != source->hard_x
                || saved.hard_y != source->hard_y || saved.gap != source->gap)
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
            if (landed_object(saved.object)) {
                // A landed squad has no setup counterpart; its request is the saved one.
                if (!alive(saved.object) || request == requests.end() || target == combat_state.targets.end()
                    || request->second.entity != saved.object
                    || std::none_of(moving.members.begin(), moving.members.end(),
                        [index](const auto& row) { return row.squad == index; }))
                    throw Invalid("land landed squad disagrees with content");
                continue;
            }
            if (source == original.squads.end() || saved.maximum_speed != source->maximum_speed
                || saved.has_formation != source->has_formation || saved.generic_team != source->generic_team
                || saved.collision_class != source->collision_class || saved.jiggle != source->jiggle
                || request == requests.end() || bound == land->requests.end() || target == combat_state.targets.end()
                || (target->second.first && !land->bindings.contains(target->second.first)
                    && (next_id == invalid_entity_id || target->second.first >= next_id))
                || std::none_of(moving.members.begin(), moving.members.end(),
                    [index](const auto& row) { return row.squad == index; })
                || request->second.entity != saved.object || request->second.mask != bound->second.mask
                || request->second.radius != bound->second.radius || request->second.width != bound->second.width)
                throw Invalid("land squad/path/combat binding disagrees with content");
        }
        // LMF-07: a pending formation names a living lead whose centre request it is, and members that are squads.
        for (const auto& [lead, order] : formations) {
            const auto is_squad = [&](EntityId object) {
                return std::any_of(moving.squads.begin(), moving.squads.end(), [object](const auto& row) { return row.object == object; });
            };
            std::vector<EntityId> slot_ids;
            for (const auto& slot : order.slots) slot_ids.push_back(slot.id);
            std::sort(slot_ids.begin(), slot_ids.end());
            if (order.lead != lead || order.request.entity != lead || order.slots.empty() || !is_squad(lead)
                || std::any_of(order.slots.begin(), order.slots.end(), [&](const auto& slot) { return !is_squad(slot.id); })
                || std::adjacent_find(slot_ids.begin(), slot_ids.end()) != slot_ids.end())
                throw Invalid("land formation order disagrees with its squads");
        }
        // A walking formation can outlive its original lead, but every retained slot must still name its own live squad.
        std::vector<EntityId> walking_ids;
        for (const auto& [lead, formation] : walking_formations) {
            if (!lead || formation.centre.status != LandPathStatus::found || formation.centre.nodes.size() < 2
                || formation.slots.empty()) throw Invalid("invalid walking formation route");
            EntityId previous{};
            for (const auto& entry : formation.slots) {
                const auto& slot = entry.slot;
                const auto squad = std::find_if(moving.squads.begin(), moving.squads.end(),
                    [&](const auto& row) { return row.object == slot.id; });
                if (slot.id <= previous || squad == moving.squads.end() || squad->formation != lead
                    || squad->formation_slot != slot.offset || entry.centre_segments.size() != squad->path.nodes.size()
                    || entry.current_segment >= formation.centre.nodes.size() - 1
                    || !std::is_sorted(entry.centre_segments.begin(), entry.centre_segments.end())
                    || std::any_of(entry.centre_segments.begin(), entry.centre_segments.end(),
                        [&](const auto segment) { return segment >= formation.centre.nodes.size() - 1; }))
                    throw Invalid("walking formation disagrees with its squad");
                previous = slot.id;
                walking_ids.push_back(slot.id);
            }
        }
        std::sort(walking_ids.begin(), walking_ids.end());
        if (std::adjacent_find(walking_ids.begin(), walking_ids.end()) != walking_ids.end())
            throw Invalid("squad belongs to multiple walking formations");
        // Version 16 (G-04..G-09): an entry names a living host that accepts it and an entrant that is either a living squad
        // walking in (moving, fading) or a detached roster whose live rows are kept in the entry (inside, waiting_exit). The
        // members of a resident entry are nested on the host, and the host's committed slots are not exceeded.
        {
            std::map<EntityId, std::uint64_t> used;
            std::uint64_t hidden_members = 0, carried_roots = 0;
            for (const auto& saved_entry : garrison_entries) {
                const auto entrant = saved_entry.first;
                const auto& entry = saved_entry.second;
                const auto* host_data = session_detail::land_garrison_data(*land, entry.host_type);
                const auto host = std::lower_bound(live.begin(), live.end(), entry.host,
                    [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                const bool host_alive = host != live.end() && host->state.entity_id == entry.host;
                const bool resident = entry.phase == session_detail::LandGarrisonPhase::inside
                    || entry.phase == session_detail::LandGarrisonPhase::waiting_exit;
                if (entrant == invalid_entity_id || entry.entrant != entrant || entrant == entry.host
                    || static_cast<std::uint8_t>(entry.phase) > static_cast<std::uint8_t>(session_detail::LandGarrisonPhase::waiting_exit)
                    || host_data == nullptr || !host_data->host || !host_alive || host->state.type_id != entry.host_type
                    || entry.destroy_with_host != host_data->hovering || entry.fade_frames > session_detail::land_garrison_fade_frames)
                    throw Invalid("land garrison record disagrees with its host");
                TypeId entrant_type{};
                PlayerId entrant_owner{};
                if (!resident) {
                    const auto unit = std::lower_bound(live.begin(), live.end(), entrant,
                        [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                    const bool squad = std::any_of(moving.squads.begin(), moving.squads.end(),
                        [entrant](const auto& row) { return row.object == entrant; });
                    if (unit == live.end() || unit->state.entity_id != entrant || !squad || !entry.hidden.empty() || entry.detached
                        || !requests.contains(entrant) || (entry.phase == session_detail::LandGarrisonPhase::moving && entry.fade_frames != 0))
                        throw Invalid("land garrison walker disagrees with its squad");
                    entrant_type = unit->state.type_id; entrant_owner = unit->state.owner;
                } else {
                    if (!entry.detached || entry.detached->squad.object != entrant || entry.detached->members.empty()
                        || entry.hidden.size() != entry.detached->members.size() + 1U || entry.fade_frames != 0
                        || entry.request.entity != entrant || requests.contains(entrant) || combat_state.targets.contains(entrant))
                        throw Invalid("land garrison occupant disagrees with its roster");
                    std::vector<EntityId> expected{entrant};
                    for (const auto& member : entry.detached->members) expected.push_back(member.object);
                    std::sort(expected.begin(), expected.end());
                    for (std::size_t i = 0; i < entry.hidden.size(); ++i) {
                        const auto& unit = entry.hidden[i].state;
                        const auto bound = land->bindings.find(unit.entity_id);
                        if (unit.entity_id != expected[i] || (i > 0 && unit.entity_id <= entry.hidden[i - 1].state.entity_id)
                            || alive(unit.entity_id) || unit.owner != entry.hidden.front().state.owner
                            || (bound != land->bindings.end() ? bound->second != std::pair{unit.type_id, unit.owner}
                                                              : !landed_object(unit.entity_id)))
                            throw Invalid("land garrison occupant identity disagrees with content");
                        if (unit.entity_id == entrant) { entrant_type = unit.type_id; entrant_owner = unit.owner; }
                    }
                    for (const auto& member : entry.detached->members) {
                        const auto carried = std::any_of(host->state.contained.begin(), host->state.contained.end(),
                            [&member](const CarriedObject& rider) { return rider.entity_id == member.object; });
                        if (!carried) throw Invalid("land garrison occupant is not carried by its host");
                    }
                    used[entry.host] += entry.value;
                    hidden_members += entry.detached->members.size();
                }
                const auto* entrant_data = session_detail::land_garrison_data(*land, entrant_type);
                if (entrant_data == nullptr || !entrant_data->entrant || entrant_data->value != entry.value
                    || entrant_owner != host->state.owner || !units::land_garrison_compatible(*host_data, *entrant_data))
                    throw Invalid("land garrison entrant disagrees with the host rules");
            }
            for (const auto& [host_id, total] : used) {
                const auto host = std::lower_bound(live.begin(), live.end(), host_id,
                    [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                const auto* host_data = session_detail::land_garrison_data(*land, host->state.type_id);
                if (host_data == nullptr || total > host_data->slots) throw Invalid("land garrison host is over capacity");
            }
            for (const auto& unit : live) carried_roots += unit.state.contained.size();
            if (carried_roots != hidden_members) throw Invalid("land garrison carried objects disagree with the records");
        }
        auto field = LandOccupancy::from_checkpoint(land->occupancy.terrain_handle(), clear_bit, occupancy);
        // Terrain is immutable; searches below reference the new stable field, never the old host.
        if (!field) throw Invalid(field.error().message);
        auto restored = std::make_unique<session_detail::LandSession>(land->content, std::move(field).value());
        auto movement = LandMovement::from_checkpoint(std::move(moving));
        if (!movement) throw Invalid("invalid retained land movement or contacts");
        restored->movement = std::move(*movement); restored->requests = std::move(requests);
        restored->combat_state = std::move(combat_state);
        restored->formations = std::move(formations);
        restored->walking_formations = std::move(walking_formations);
        restored->target_query_margin = land->target_query_margin;
        restored->bindings = land->bindings;
        for (const auto& [object, pose] : land->static_roots)
            if (alive(object)) restored->static_roots.emplace(object, pose);
        for (const auto child : dynamic_children) {
            const auto unit = std::lower_bound(live.begin(), live.end(), child,
                [](const auto& row, EntityId key) { return row.state.entity_id < key; });
            restored->bindings.emplace(child, std::pair{unit->state.type_id, unit->state.owner});
            restored->static_roots.emplace(child, unit->state);
        }
        restored->dynamic_children = std::move(dynamic_children);
        for (const auto object : landed) {
            const auto unit = std::lower_bound(live.begin(), live.end(), object,
                [](const auto& row, EntityId key) { return row.state.entity_id < key; });
            if (unit != live.end() && unit->state.entity_id == object)
                restored->bindings.emplace(object, std::pair{unit->state.type_id, unit->state.owner});
        }
        for (const auto& pair : garrison_entries)
            for (const auto& hidden : pair.second.hidden)
                restored->bindings.emplace(hidden.state.entity_id, std::pair{hidden.state.type_id, hidden.state.owner});
        restored->garrison_entries = std::move(garrison_entries);
        restored->landings = std::move(landings);
        restored->landed = std::move(landed);
        restored->next_landing = next_landing;
        const auto count = in.count();
        EntityId previous{};
        for (std::uint64_t i = 0; i < count; ++i) {
            EntityId object{};
            LandSearch::Checkpoint search;
            in(object, search);
            const auto request = restored->requests.find(object);
            // A formation lead's search is the formation's centre request (LMF-07), not the squad's own request.
            const auto formation = restored->formations.find(object);
            if (object <= previous || request == restored->requests.end()
                || search.request != (formation != restored->formations.end() ? formation->second.request : request->second))
                throw Invalid("land search has no matching squad request");
            previous = object;
            auto result = LandPathService::restore(restored->occupancy, restored->arena, search);
            if (!result) throw Invalid(result.error().message);
            restored->searches.emplace(object, std::move(result).value());
        }
        if (in.remaining()) throw Invalid("trailing land section bytes");
        for (const auto& [lead, order] : restored->formations)
            if (!restored->searches.contains(lead)) throw Invalid("land formation order has no centre path search");
        if (live.size() != restored->movement.members().size() + restored->movement.squads().size() + restored->static_roots.size())
            throw Invalid("land section and child-state object counts disagree");
        for (const auto& unit : live) {
            const auto binding = restored->bindings.find(unit.state.entity_id);
            // LC-55: the pad service moves a pad between players (WBP-28), so a pad row's owner is its own state, not
            // a setup identity; every other object, including each company member, keeps the exact comparison.
            const bool pad = economy.pads.point(unit.state.type_id) != nullptr;
            if (binding == restored->bindings.end() || binding->second.first != unit.state.type_id
                || (!pad && binding->second.second != unit.state.owner))
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
