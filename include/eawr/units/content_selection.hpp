#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include <algorithm>
#include <limits>
#include <map>

namespace eawr::units {

// AG-12: one source-order winner per name CRC, shared by every derived table.
// Equal source ordinals (including synthetic tables) retain input order.
class ContentTypeSelection {
public:
    explicit ContentTypeSelection(const UnitTables& tables) {
        for (const auto& unit : tables.units) {
            const auto id = assets::object_type_crc(unit.id);
            auto [at, inserted] = winners_.try_emplace(id, &unit);
            if (!inserted && unit.table_order < at->second->table_order) at->second = &unit;
        }
    }
    [[nodiscard]] const UnitType* find(const std::string_view name) const {
        const auto at = winners_.find(assets::object_type_crc(name));
        return at == winners_.end() ? nullptr : at->second;
    }
    [[nodiscard]] bool keeps(const UnitType& unit) const { return find(unit.id) == &unit; }
    void emit_notes(const UnitTables& tables, const std::string_view diagnostic_code, core::LoadNoteBuffer* load_notes) const {
        if (!load_notes) return;
        for (const auto& unit : tables.units) {
            if (keeps(unit)) continue;
            core::LoadNote note;
            note.code = "EAWR-LOAD-0251";
            note.object = unit.id;
            note.tag = "name";
            note.authored_value = unit.id;
            note.effective_value = find(unit.id)->id;
            note.action = core::LoadAction::skipped_object;
            note.diagnostic_code = diagnostic_code;
            note.message = "two unit types share type ID " + std::to_string(assets::object_type_crc(unit.id));
            load_notes->append(std::move(note));
        }
    }
private:
    std::map<std::uint32_t, const UnitType*> winners_;
};

// One CRC winner for all placement footprint and presentation consumers.
class FootprintTypeSelection {
public:
    struct Entry {
        const UnitType* unit{};
        const ObstacleType* obstacle{};
        std::uint64_t order{};
        [[nodiscard]] const std::string& name() const { return unit ? unit->id : obstacle->id; }
    };
    explicit FootprintTypeSelection(const UnitTables& tables) {
        for (const auto& unit : tables.units) add({&unit, nullptr, unit.table_order});
        for (const auto& obstacle : tables.obstacles) add({nullptr, &obstacle, obstacle.table_order});
    }
    [[nodiscard]] const Entry* find(std::string_view name) const {
        const auto it = winners_.find(assets::object_type_crc(name));
        return it == winners_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool keeps(const UnitType& type) const { return find(type.id)->unit == &type; }
    [[nodiscard]] bool keeps(const ObstacleType& type) const { return find(type.id)->obstacle == &type; }
    [[nodiscard]] const auto& winners() const { return winners_; }
private:
    void add(Entry entry) {
        auto [at, inserted] = winners_.try_emplace(assets::object_type_crc(entry.name()), entry);
        if (!inserted && entry.order < at->second.order) at->second = entry;
    }
    std::map<std::uint32_t, Entry> winners_;
};

// AG-12: power arithmetic saturates only after a checked operation reports overflow.
// Preserve that diagnostic in the load note; stock arithmetic retains its exact result.
inline Fixed saturated_power(const core::Result<Fixed>& result, const bool negative,
    const std::string& object, const std::string& expression, const std::string& context,
    const std::string_view diagnostic_code, core::LoadNoteBuffer* notes) {
    if (result) return result.value();
    const auto effective = Fixed::from_raw(negative ? std::numeric_limits<std::int64_t>::min()
                                                   : std::numeric_limits<std::int64_t>::max());
    if (notes) {
        core::LoadNote note;
        note.code = "EAWR-LOAD-0250";
        note.object = object;
        note.tag = "AI_Combat_Power";
        note.authored_value = expression;
        note.effective_value = std::to_string(effective.raw());
        note.action = core::LoadAction::clamped;
        note.diagnostic_code = diagnostic_code;
        note.message = context + result.error().message;
        notes->append(std::move(note));
    }
    return effective;
}
inline Fixed add_power(const Fixed left, const Fixed right, const std::string& object,
    const std::string& context, const std::string_view diagnostic_code, core::LoadNoteBuffer* notes = nullptr) {
    return saturated_power(sim::math::add(left, right), right.raw() < 0, object,
        std::to_string(left.raw()) + " + " + std::to_string(right.raw()), context, diagnostic_code, notes);
}
inline Fixed multiply_power(const Fixed value, const std::int32_t count, const std::string& object,
    const std::string& context, const std::string_view diagnostic_code, core::LoadNoteBuffer* notes = nullptr) {
    // Every int32 count fits the integer part of Q24 fixed storage.
    const auto factor = Fixed::from_integer(count);
    return saturated_power(sim::math::multiply(value, factor.value()), (value.raw() < 0) != (count < 0), object,
        std::to_string(value.raw()) + " * " + std::to_string(count), context, diagnostic_code, notes);
}

inline Fixed bounded_power(const Fixed value, const std::string& object, const std::string& message,
    const std::string_view diagnostic_code, core::LoadNoteBuffer* notes) {
    const auto raw = std::clamp(value.raw(), std::int64_t{0},
        sim::tactical::max_weapon_combat_power * Fixed::scale);
    if (raw != value.raw() && notes) {
        core::LoadNote note;
        note.code = "EAWR-LOAD-0252";
        note.object = object;
        note.tag = "AI_Combat_Power";
        note.authored_value = std::to_string(value.raw());
        note.effective_value = std::to_string(raw);
        note.action = core::LoadAction::clamped;
        note.diagnostic_code = diagnostic_code;
        note.message = message;
        notes->append(std::move(note));
    }
    return Fixed::from_raw(raw);
}

} // namespace eawr::units
