#pragma once

#include "eawr/units/unit_tables.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::units::detail {

[[nodiscard]] std::string trim(std::string_view value);

// #71: the X and Y half extents of a model's collision bounds in the bind pose, before
// Scale_Factor (research E71-18); +-1 without a collidable mesh. Converts the ALO's binary32
// bounds here, in the units converter file.
[[nodiscard]] std::optional<std::pair<Fixed, Fixed>> collision_half_extents(
    const assets::Model& model, const std::vector<sim::math::Mat3x4>& frames);
[[nodiscard]] std::string lower(std::string_view value);
[[nodiscard]] std::string upper(std::string_view value);
[[nodiscard]] bool iequals(std::string_view left, std::string_view right) noexcept;

// Tokens of a list-shaped value: split on commas and whitespace, empties dropped.
[[nodiscard]] std::vector<std::string> tokens(std::string_view text);
// An authored decimal. One trailing C float suffix (`1.0f`, `-3f`) is accepted.
[[nodiscard]] std::optional<Fixed> number(std::string_view text);
[[nodiscard]] std::optional<bool> boolean(std::string_view text);

// `data/art/models/<name>` in lower case, with `.alo` added when absent.
[[nodiscard]] std::string model_path(std::string_view name);

// Collects unresolved rows, notes and input files while loading.
struct Report final {
    std::vector<Unresolved> unresolved;
    std::vector<Unresolved> notes;
    std::map<std::string, InputFile> inputs;

    void missing(std::string owner, std::string field, std::string value, std::string reason) {
        add(unresolved, {std::move(owner), std::move(field), std::move(value), std::move(reason)});
    }
    void note(std::string owner, std::string field, std::string value, std::string reason) {
        add(notes, {std::move(owner), std::move(field), std::move(value), std::move(reason)});
    }
    // A row found again (a list tag read twice) is kept once.
    static void add(std::vector<Unresolved>& rows, Unresolved row) {
        for (const auto& existing : rows) {
            if (existing.owner == row.owner && existing.field == row.field && existing.value == row.value &&
                existing.reason == row.reason) {
                return;
            }
        }
        rows.push_back(std::move(row));
    }
    void input(InputFile file) {
        auto key = file.logical_path;
        inputs.insert_or_assign(std::move(key), std::move(file));
    }
};

// A resolved object plus the raw variant layers (derived first), so list
// tags can be read with the list rule in docs/unit-data.md.
struct Object final {
    data::EffectiveObject effective;
    std::vector<const data::Definition*> layers;
    std::set<std::string> read_single; // lower-case tags read as one value

    [[nodiscard]] const data::XmlNode* single(std::string_view tag);
    [[nodiscard]] std::string text(std::string_view tag);
    [[nodiscard]] std::optional<Fixed> fixed(std::string_view tag, Report& report, bool required);
    // Every occurrence from the most-derived layer that authors the tag.
    [[nodiscard]] std::vector<const data::XmlNode*> list(std::string_view tag, Report& report) const;
    // Notes for single-value tags authored more than once in one layer.
    void note_duplicates(Report& report) const;
};

[[nodiscard]] std::optional<Object> resolve(const data::Catalog& catalog, std::string_view id,
                                            data::Category category, std::string_view owner,
                                            std::string_view field, Report& report);

// Bone-name lookup, ASCII case-insensitive, first match in bone order.
[[nodiscard]] std::optional<std::size_t> bone_index(const assets::Model& model, std::string_view bone);
[[nodiscard]] Vec3 translation(const sim::math::Mat3x4& frame) noexcept;
// A frame's x, y and z axes: its rotation columns.
[[nodiscard]] std::array<Vec3, 3> axes(const sim::math::Mat3x4& frame) noexcept;
// The union of the model's collidable meshes, each placed by its bone's bind frame; nothing when
// the model has no collidable mesh (#74).
[[nodiscard]] std::optional<CollisionBounds> collision_bounds(
    const assets::Model& model, const std::vector<sim::math::Mat3x4>& frames);
// #536: the model's collidable meshes with their triangles, each vertex placed by its bone's bind
// frame and then by `base` (the owner's attachment frame for a hardpoint's model), appended in
// mesh order; `hardpoint` tags the meshes of a hardpoint's attached model.
void append_collision_meshes(const assets::Model& model, const std::vector<sim::math::Mat3x4>& frames,
    const sim::math::Mat3x4& base, std::uint32_t hardpoint, std::vector<CollisionMesh>& out);

// The GameObjectCategoryType and GameObjectPropertiesType dynamic enums (#270).
void load_enums(const vfs::Vfs& filesystem, UnitTables& tables, Report& report);
// A case-insensitive enum lookup; nullopt when the name is not in the enum.
[[nodiscard]] std::optional<std::uint64_t> enum_value(const std::vector<EnumValue>& values, std::string_view name);
// The OR of named enum bits; each unknown name is reported and adds nothing.
[[nodiscard]] std::uint64_t enum_bits(const std::vector<EnumValue>& values, const std::vector<std::string>& names,
                                      const std::string& owner, std::string_view field, Report& report);
// Needs load_enums first: Attack_Priorities names are classified against both enums.
void load_priority_sets(const vfs::Vfs& filesystem, const std::vector<std::string>& wanted,
                        UnitTables& tables, Report& report);
void load_constants(const vfs::Vfs& filesystem, const std::set<std::string>& damage_types,
                    const std::set<std::string>& armor_types, UnitTables& tables, Report& report);

} // namespace eawr::units::detail
