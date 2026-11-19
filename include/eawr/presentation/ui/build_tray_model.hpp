#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Galactic build tray grouping and row packing (docs/ui/galactic-build-tray.md, GBT-04 to GBT-06 and
// GBT-12). Engine-free and presentation only: it turns the build choice list the bar already computes
// into ordered groups and rows. It never changes the choice list, the bar's own mapping or any game
// data, and it never hides a choice on its own: only the capacity limit of the row budget can omit
// entries, and it reports how many.
//
// Everything a mod can change is an input: the category enum order, the priority and building
// category lists, the captions, extra groups and type assignments (the groups file, GBT-12), the row
// budget and the sizes (the layout file, GBT-11). No category, type or mod name is compiled in, except
// the four engine Behavior role names a building is recognised by (GBT-04).
namespace eawr::presentation::ui::build_tray {

// GBT-04: the Behavior tags that mark a structure or a base (ground special structure, orbital special
// structure, star base level, ground base level).
[[nodiscard]] bool is_building_behavior(std::string_view behavior) noexcept;
[[nodiscard]] bool has_building_behavior(std::span<const std::string> behaviors) noexcept;

// ---- The groups file (GBT-12), already validated by the loader ------------------------------------

struct ExtraGroup final {
    enum class Where : std::uint8_t { start, end, after };
    std::string name;     // not a category name, "Buildings" or "Misc"
    std::string caption;  // text key, may be empty
    Where where{Where::end};
    std::string after;    // the category name when where == after
};

struct TypeAssignment final {
    std::string type;                      // exact name, or a prefix when it ends with '*'
    std::string group;                     // extra group, category name, or "Buildings"
    std::optional<std::int64_t> sort_key;
};

struct GroupDefinition final {
    std::vector<std::string> category_priority;   // highest first (GBT-05)
    std::vector<std::string> building_categories; // GBT-04
    std::vector<std::pair<std::string, std::string>> captions; // group name -> text key
    std::vector<ExtraGroup> extra_groups;
    std::vector<TypeAssignment> assignments;
};

// ---- What the game data says about each type ------------------------------------------------------

struct TypeFacts final {
    std::string name;
    std::vector<std::string> categories;  // authored CategoryMask names
    std::vector<std::string> members;     // squadron and company member type names
    bool building_role{};                 // has_building_behavior
};

// Case-insensitive by type name. The category names of a squadron or company are the union of its
// members' (PL-13, GBT-05); a member that is itself a squadron or company counts through its own
// members, to a bounded depth.
class TypeIndex final {
public:
    void add(TypeFacts facts);
    [[nodiscard]] const TypeFacts* find(std::string_view name) const noexcept;
    [[nodiscard]] bool any_with_prefix(std::string_view prefix) const noexcept;
    [[nodiscard]] std::vector<std::string> categories_of(std::string_view name) const;

private:
    std::unordered_map<std::string, TypeFacts> types_;
};

// The category enum in authored order. `All` and `None` are never a category (GBT-05); a name the enum
// does not define is ignored.
struct CategoryEnum final {
    std::vector<std::string> names;
    [[nodiscard]] std::optional<std::size_t> index_of(std::string_view name) const noexcept;
};

// GBT-05: the first listed name of `names` in the priority list, else the one earliest in the enum.
[[nodiscard]] std::optional<std::string> choose_category(std::span<const std::string> names,
    const CategoryEnum& categories, const GroupDefinition& definition);

// GBT-05 header text without a caption key: the name split into words ("SuperCapital" -> "Super
// capital", "Warlord_Choices" -> "Warlord choices").
[[nodiscard]] std::string derived_caption(std::string_view name);

// ---- Groups ---------------------------------------------------------------------------------------

enum class GroupKind : std::uint8_t { category, extra, misc, buildings };

struct Group final {
    GroupKind kind{GroupKind::category};
    std::string name;                 // category or extra group name; "Misc"; "Buildings"
    std::string caption_key;          // empty: the caller shows derived_caption(name)
    std::vector<std::size_t> choices; // indices into the choice list, in tray order
    float min_width{};                // the header's width, set by the caller before packing
};

// GBT-04, GBT-05, GBT-12: every choice lands in exactly one group. Order of rules: a type assignment
// (exact name before the longest prefix), then a building role or building category, then the
// category, then misc. Unit groups come first (category groups in enum order with extra groups at
// their positions, misc last), buildings last. Empty groups are dropped. Inside a group the entries
// keep the choice list order, except that those with a sort key come first in ascending key order.
[[nodiscard]] std::vector<Group> classify(std::span<const std::string> choice_types, const TypeIndex& types,
    const CategoryEnum& categories, const GroupDefinition& definition);

// ---- Row packing (GBT-06) -------------------------------------------------------------------------

struct Metrics final {
    float entry_width{};
    float entry_height{};
    float header_height{};
    float entry_gap{};
    float group_gap{};
    float row_gap{};
    float padding{};
    float panel_width{};
    float top_reserve{};  // a strip kept free above the first row (the close icon's, GBT-07)
    [[nodiscard]] float row_height() const noexcept { return header_height + entry_height; }
};

struct PackOptions final {
    std::size_t max_rows{7};          // including the buildings rows
    bool fold_single_choice_groups{true};
    float misc_min_width{};           // header width of a misc group the packer has to create
    std::string misc_caption_key;     // its caption text key (the definition's Misc caption)
};

// One run of entries of a group on one row. A group wider than a row continues on the next row as a
// further run without a header.
struct Run final {
    std::size_t group{};
    std::size_t first{};  // position in Group::choices
    std::size_t count{};
    float x{};            // from the panel's left padding edge
    bool header{};
};

struct Row final {
    std::vector<Run> runs;
};

struct Packed final {
    std::vector<Group> groups;
    std::vector<Row> rows;            // top to bottom: unit rows, then the buildings rows
    std::size_t unit_rows{};
    std::size_t building_rows{};
    bool misc_mode{};                 // the groups did not fit: the last unit rows are misc
    std::size_t omitted{};            // entries cut from the end of misc by the capacity limit (not seen in any checked mod)
    float height{};
};

[[nodiscard]] std::size_t entries_per_line(const Metrics& metrics) noexcept;

// GBT-06: place groups in order on the current row when they fit, else start a new row; never
// backfill earlier rows. Buildings take their own rows at the bottom and are always kept. When the unit
// groups need more rows than the budget leaves, the last unit rows become misc: it keeps the largest
// groups (single-choice groups excluded when folded) in the rows above and takes every other entry.
[[nodiscard]] Packed pack(std::vector<Group> groups, const Metrics& metrics, const PackOptions& options);

struct FitResult final {
    bool compact{};       // the one smaller size step was used
    Packed packed;
};

// GBT-06 height cap: use the full size when the packed rows fit `available_height`; else the compact
// size; else the compact size with fewer rows, never fewer than the buildings rows plus one unit row.
[[nodiscard]] FitResult fit_to_height(const std::vector<Group>& groups, const Metrics& full,
    const Metrics& compact, const PackOptions& options, float available_height);

// GBT-06, Width_Mode Auto: the narrowest panel width from `min_width` up to `max_width`, in steps of one entry
// pitch, at which the groups pack within the row budget (`options.max_rows`) without the misc fold and within
// `max_height`: the rows fill to the budget first and the panel widens only when they do not hold every group.
// `max_width` when no width does. `metrics.panel_width` is ignored; `max_width` below `min_width` reads as `min_width`.
[[nodiscard]] float automatic_width(const std::vector<Group>& groups, Metrics metrics, const PackOptions& options,
    float min_width, float max_width, float max_height);

} // namespace eawr::presentation::ui::build_tray
