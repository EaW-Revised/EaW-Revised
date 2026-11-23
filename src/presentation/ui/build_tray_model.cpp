#include "eawr/presentation/ui/build_tray_model.hpp"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <utility>

namespace eawr::presentation::ui::build_tray {
namespace {

constexpr int kMemberDepth = 3;

char lower(const char value) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

std::string lowered(const std::string_view text) {
    std::string result(text);
    for (auto& character : result) character = lower(character);
    return result;
}

bool equal(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) { return lower(a) == lower(b); });
}

bool starts_with(const std::string_view text, const std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && equal(text.substr(0, prefix.size()), prefix);
}

bool contains(const std::vector<std::string>& names, const std::string_view name) noexcept {
    return std::any_of(names.begin(), names.end(), [&](const auto& item) { return equal(item, name); });
}

void append_unique(std::vector<std::string>& out, const std::vector<std::string>& names) {
    for (const auto& name : names) if (!contains(out, name)) out.push_back(name);
}

void collect_categories(const TypeIndex& index, const std::string_view name, const int depth,
    std::vector<std::string>& out) {
    const auto* facts = index.find(name);
    if (!facts) return;
    if (!facts->members.empty() && depth < kMemberDepth) {
        std::vector<std::string> from_members;
        for (const auto& member : facts->members) collect_categories(index, member, depth + 1, from_members);
        if (!from_members.empty()) { append_unique(out, from_members); return; }
    }
    append_unique(out, facts->categories);
}

ClassFacts find_class(const TypeIndex& index, const std::string_view name, const int depth) {
    const auto* facts = index.find(name);
    if (!facts) return {};
    if (!facts->encyclopedia_class.empty()) return {facts->encyclopedia_class, facts->class_text};
    if (depth >= kMemberDepth) return {};
    for (const auto& member : facts->members)
        if (auto found = find_class(index, member, depth + 1); !found.key.empty()) return found;
    return {};
}

// GBT-05: the header of a class group. The mod's class text often reads "Class: Droid" (the card's line); the header
// keeps what follows the first colon, language-independent punctuation, so no word of any language is known to the
// code. A class whose text the mod lacks shows its key. Empty: the type has no class.
std::string class_label(const ClassFacts& facts) {
    if (facts.key.empty()) return {};
    if (facts.text.empty()) return facts.key;
    const auto colon = facts.text.find(':');
    if (colon == std::string::npos) return facts.text;
    auto rest = facts.text.substr(colon + 1);
    const auto first = rest.find_first_not_of(' ');
    if (first == std::string::npos) return facts.text;
    rest.erase(0, first);
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();
    return rest;
}

bool never_a_category(const std::string_view name) noexcept {
    return equal(name, "All") || equal(name, "None");
}

const std::string* caption_of(const GroupDefinition& definition, const std::string_view name) noexcept {
    for (const auto& [group, key] : definition.captions) if (equal(group, name)) return &key;
    return nullptr;
}

// The matching assignment: an exact name wins over any prefix, the longest prefix over a shorter one,
// the earlier entry over a later one.
const TypeAssignment* match_assignment(const GroupDefinition& definition, const std::string_view type) noexcept {
    const TypeAssignment* best = nullptr;
    std::size_t best_length{};
    for (const auto& assignment : definition.assignments) {
        const std::string_view pattern = assignment.type;
        if (!pattern.empty() && pattern.back() == '*') {
            const auto prefix = pattern.substr(0, pattern.size() - 1);
            if (starts_with(type, prefix) && (!best || prefix.size() > best_length)) {
                best = &assignment;
                best_length = prefix.size();
            }
        } else if (equal(pattern, type)) {
            return &assignment;
        }
    }
    return best;
}

} // namespace

bool is_building_behavior(const std::string_view behavior) noexcept {
    return equal(behavior, "DUMMY_GROUND_STRUCTURE") || equal(behavior, "DUMMY_ORBITAL_STRUCTURE")
        || equal(behavior, "DUMMY_STAR_BASE") || equal(behavior, "DUMMY_GROUND_BASE");
}

bool has_building_behavior(const std::span<const std::string> behaviors) noexcept {
    return std::any_of(behaviors.begin(), behaviors.end(),
        [](const auto& behavior) { return is_building_behavior(behavior); });
}

void TypeIndex::add(TypeFacts facts) {
    auto key = lowered(facts.name);
    types_.insert_or_assign(std::move(key), std::move(facts));
}

const TypeFacts* TypeIndex::find(const std::string_view name) const noexcept {
    const auto found = types_.find(lowered(name));
    return found == types_.end() ? nullptr : &found->second;
}

bool TypeIndex::any_with_prefix(const std::string_view prefix) const noexcept {
    const auto wanted = lowered(prefix);
    return std::any_of(types_.begin(), types_.end(), [&](const auto& entry) {
        return entry.first.compare(0, wanted.size(), wanted) == 0;
    });
}

std::vector<std::string> TypeIndex::categories_of(const std::string_view name) const {
    std::vector<std::string> result;
    collect_categories(*this, name, 0, result);
    return result;
}

ClassFacts TypeIndex::class_of(const std::string_view name) const {
    return find_class(*this, name, 0);
}

std::optional<std::size_t> CategoryEnum::index_of(const std::string_view name) const noexcept {
    if (never_a_category(name)) return std::nullopt;
    for (std::size_t index = 0; index < names.size(); ++index) if (equal(names[index], name)) return index;
    return std::nullopt;
}

std::optional<std::string> choose_category(const std::span<const std::string> names, const CategoryEnum& categories,
    const GroupDefinition& definition) {
    std::optional<std::size_t> best;
    for (const auto& name : names) {
        const auto index = categories.index_of(name);
        if (index && (!best || *index < *best)) best = index;
    }
    if (!best) return std::nullopt;
    for (const auto& listed : definition.category_priority) {
        const auto index = categories.index_of(listed);
        if (!index) continue;
        if (std::any_of(names.begin(), names.end(), [&](const auto& name) { return equal(name, categories.names[*index]); }))
            return categories.names[*index];
    }
    return categories.names[*best];
}

std::string derived_caption(const std::string_view name) {
    std::string words;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        if (character == '_') {
            if (!words.empty() && words.back() != ' ') words += ' ';
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(character)) != 0;
        const bool after_lower = index > 0 && std::islower(static_cast<unsigned char>(name[index - 1])) != 0;
        if (upper && after_lower) words += ' ';
        words += words.empty() ? character : (upper ? lower(character) : character);
    }
    return words;
}

std::string handle_label(const std::string_view caption, const std::size_t omitted) {
    std::string label(caption);
    if (omitted != 0) return label + std::to_string(omitted);
    while (!label.empty() && std::isspace(static_cast<unsigned char>(label.back())) != 0) label.pop_back();
    return label;
}

std::vector<Group> classify(const std::span<const std::string> choice_types, const TypeIndex& types,
    const CategoryEnum& categories, const GroupDefinition& definition,
    const std::span<const std::pair<std::string, std::string>> category_headers) {
    struct Placed final {
        Group group;
        std::uint64_t order{};
        struct Entry final {
            std::size_t choice{};
            std::optional<std::int64_t> sort_key;
            std::string class_label; // the class group's header; empty: no class
        };
        std::vector<Entry> entries;
    };
    constexpr std::uint64_t step = 1'000'000;
    std::vector<Placed> units;
    std::optional<Placed> misc;
    std::optional<Placed> buildings;

    const auto caption = [&](const std::string_view name) {
        const auto* key = caption_of(definition, name);
        return key ? *key : std::string{};
    };
    const auto make = [&](const GroupKind kind, std::string name, const std::uint64_t order) {
        Placed placed;
        placed.group.kind = kind;
        placed.group.caption_key = caption(name);
        placed.group.name = std::move(name);
        placed.order = order;
        return placed;
    };
    const auto find_unit = [&](const GroupKind kind, const std::string_view name) -> Placed* {
        for (auto& placed : units) if (placed.group.kind == kind && equal(placed.group.name, name)) return &placed;
        return nullptr;
    };
    const auto category_group = [&](const std::string_view name) -> Placed& {
        const auto index = *categories.index_of(name);
        if (auto* found = find_unit(GroupKind::category, categories.names[index])) return *found;
        units.push_back(make(GroupKind::category, categories.names[index], (index + 1) * step));
        return units.back();
    };
    const auto extra_group = [&](const std::string_view name) -> Placed* {
        if (auto* found = find_unit(GroupKind::extra, name)) return found;
        std::uint64_t sequence{};
        for (const auto& extra : definition.extra_groups) {
            if (!equal(extra.name, name)) { ++sequence; continue; }
            std::uint64_t order = (categories.names.size() + 2) * step + sequence;
            if (extra.where == ExtraGroup::Where::start) order = sequence;
            else if (extra.where == ExtraGroup::Where::after) {
                if (const auto at = categories.index_of(extra.after)) order = (*at + 1) * step + step / 2 + sequence;
            }
            auto placed = make(GroupKind::extra, extra.name, order);
            if (placed.group.caption_key.empty()) placed.group.caption_key = extra.caption;
            units.push_back(std::move(placed));
            return &units.back();
        }
        return nullptr;
    };
    const auto buildings_group = [&]() -> Placed& {
        if (!buildings) buildings = make(GroupKind::buildings, "Buildings", 0);
        return *buildings;
    };
    const auto misc_group = [&]() -> Placed& {
        if (!misc) misc = make(GroupKind::misc, "Misc", 0);
        return *misc;
    };

    for (std::size_t choice = 0; choice < choice_types.size(); ++choice) {
        const auto& type = choice_types[choice];
        std::optional<std::int64_t> sort_key;
        Placed* target = nullptr;
        if (const auto* assignment = match_assignment(definition, type)) {
            if (equal(assignment->group, "Buildings")) target = &buildings_group();
            else if (auto* extra = extra_group(assignment->group)) target = extra;
            else if (categories.index_of(assignment->group)) target = &category_group(assignment->group);
            if (target) sort_key = assignment->sort_key;
        }
        if (!target) {
            const auto* facts = types.find(type);
            const auto names = types.categories_of(type);
            const auto category = choose_category(names, categories, definition);
            if ((facts && facts->building_role)
                || (category && contains(definition.building_categories, *category))) target = &buildings_group();
            else if (category) target = &category_group(*category);
            else target = &misc_group();
        }
        target->entries.push_back({choice, sort_key, class_label(types.class_of(type))});
    }

    // GBT-05 (legacy #8160): a category group whose choices carry more than one encyclopedia class splits into one group
    // per class (classes that read the same are one), in the order each first appears in the choice list, headed by the class text; choices
    // without a class stay in the category's own group after them. A group with one class, or none, stays whole.
    std::vector<Placed> parts;
    for (auto& placed : units) {
        if (placed.group.kind != GroupKind::category) continue;
        std::vector<std::string> classes;
        for (const auto& entry : placed.entries) {
            if (entry.class_label.empty()) continue;
            if (std::none_of(classes.begin(), classes.end(), [&](const auto& label) { return equal(label, entry.class_label); }))
                classes.push_back(entry.class_label);
        }
        if (classes.size() < 2) continue;
        std::vector<Placed::Entry> rest;
        std::vector<Placed> split(classes.size());
        for (std::size_t index = 0; index < classes.size(); ++index) {
            split[index].group.kind = GroupKind::category;
            split[index].group.name = placed.group.name;
            split[index].group.label = classes[index];
            split[index].order = placed.order + 1 + index;
        }
        for (auto& entry : placed.entries) {
            const auto at = std::find_if(classes.begin(), classes.end(),
                [&](const auto& label) { return equal(label, entry.class_label); });
            if (at == classes.end()) { rest.push_back(std::move(entry)); continue; }
            auto& part = split[static_cast<std::size_t>(at - classes.begin())];
            part.entries.push_back(std::move(entry));
        }
        placed.entries = std::move(rest);
        placed.order += step / 4; // the choices without a class follow the classes
        for (auto& part : split) parts.push_back(std::move(part));
    }
    for (auto& part : parts) units.push_back(std::move(part));

    std::vector<Group> result;
    const auto finish = [&](Placed& placed) {
        std::stable_sort(placed.entries.begin(), placed.entries.end(), [](const auto& left, const auto& right) {
            if (left.sort_key.has_value() != right.sort_key.has_value()) return left.sort_key.has_value();
            return left.sort_key && *left.sort_key < *right.sort_key;
        });
        for (const auto& entry : placed.entries) placed.group.choices.push_back(entry.choice);
        result.push_back(std::move(placed.group));
    };
    std::stable_sort(units.begin(), units.end(), [](const auto& left, const auto& right) { return left.order < right.order; });
    // GBT-05: a class group and another category group whose headers read the same are one group, so no two identical headers
    // sit in one tray. Only a class group triggers it: category groups without classes keep their own headers. The merged
    // entries go back in choice list order (the sort keys then put the keyed ones first, as for any group).
    const auto header_of = [&](const Placed& placed) {
        if (!placed.group.label.empty()) return placed.group.label;
        for (const auto& [name, text] : category_headers) if (equal(name, placed.group.name)) return text;
        return derived_caption(placed.group.name);
    };
    for (std::size_t later = 1; later < units.size(); ++later) {
        auto& current = units[later];
        if (current.group.kind != GroupKind::category || current.entries.empty()) continue;
        for (std::size_t earlier = 0; earlier < later; ++earlier) {
            auto& target = units[earlier];
            if (target.group.kind != GroupKind::category || target.entries.empty()) continue;
            if (target.group.label.empty() && current.group.label.empty()) continue;
            if (!equal(header_of(target), header_of(current))) continue;
            for (auto& entry : current.entries) target.entries.push_back(std::move(entry));
            current.entries.clear();
            std::stable_sort(target.entries.begin(), target.entries.end(),
                [](const auto& left, const auto& right) { return left.choice < right.choice; });
            break;
        }
    }
    for (auto& placed : units) if (!placed.entries.empty()) finish(placed);
    if (misc && !misc->entries.empty()) finish(*misc);
    if (buildings && !buildings->entries.empty()) finish(*buildings);
    return result;
}

std::size_t entries_per_line(const Metrics& metrics) noexcept {
    const float inner = metrics.panel_width - 2.0F * metrics.padding;
    const float unit = metrics.entry_width + metrics.entry_gap;
    if (unit <= 0.0F || inner <= 0.0F) return 1;
    return std::max<std::size_t>(1, static_cast<std::size_t>((inner + metrics.entry_gap) / unit));
}

namespace {

float inner_width(const Metrics& metrics) noexcept {
    return metrics.panel_width - 2.0F * metrics.padding;
}

float span_width(const Metrics& metrics, const std::size_t count) noexcept {
    return count == 0 ? 0.0F
        : static_cast<float>(count) * metrics.entry_width + static_cast<float>(count - 1) * metrics.entry_gap;
}

// Places `groups` (indices into `all`) in order on rows; `own_row` makes the last one start a new row.
void place_groups(const std::vector<Group>& all, const std::vector<std::size_t>& order, const Metrics& metrics,
    std::vector<Row>& rows, const bool last_on_own_row) {
    float used{};
    const float inner = inner_width(metrics);
    const auto line_capacity = entries_per_line(metrics);
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto index = order[position];
        const auto& group = all[index];
        const auto count = group.choices.size();
        if (count == 0) continue;
        const float width = std::max(span_width(metrics, count), group.min_width);
        const bool own = last_on_own_row && position + 1 == order.size();
        if (width <= inner) {
            if (!own && !rows.empty() && used > 0.0F && used + metrics.group_gap + width <= inner) {
                rows.back().runs.push_back({index, 0, count, used + metrics.group_gap, true});
                used += metrics.group_gap + width;
            } else {
                rows.push_back({{{index, 0, count, 0.0F, true}}});
                used = width;
            }
            continue;
        }
        // Wider than a full row: wrap; the next group may use what is left of the last line.
        std::size_t first{};
        bool header = true;
        while (first < count) {
            const auto take = std::min(line_capacity, count - first);
            rows.push_back({{{index, first, take, 0.0F, header}}});
            used = span_width(metrics, take);
            first += take;
            header = false;
        }
        if (own) used = inner; // nothing shares a misc row
    }
}

float height_of(const Metrics& metrics, const std::vector<Row>& rows) noexcept {
    if (rows.empty()) return 0.0F;
    const float category_gap = category_gap_of(metrics, rows);
    std::size_t headed{};
    for (std::size_t index = 1; index < rows.size(); ++index) headed += has_header(rows[index]);
    return 2.0F * metrics.padding + metrics.top_reserve + static_cast<float>(rows.size()) * metrics.row_height()
        + static_cast<float>(rows.size() - 1) * metrics.row_gap + static_cast<float>(headed) * category_gap;
}

} // namespace

bool has_header(const Row& row) noexcept {
    return std::any_of(row.runs.begin(), row.runs.end(), [](const Run& run) { return run.header; });
}

float category_gap_of(const Metrics& metrics, const std::vector<Row>& rows) noexcept {
    std::size_t headed{};
    for (std::size_t index = 1; index < rows.size(); ++index) headed += has_header(rows[index]);
    return headed == 0 ? 0.0F : std::max(0.0F, metrics.category_gap);
}

float header_lift_of(const Metrics& metrics, const float category_gap, const bool first_row) noexcept {
    // The first row's label may rise into the strip kept for the close icon, but never past the row gap that separates that
    // icon's band from the row (the reserve puts the first row one row gap under the icon): never onto the close icon (GBT-07).
    const float room = first_row ? (metrics.top_reserve > 0.0F ? metrics.row_gap : metrics.padding)
                                 : metrics.row_gap + category_gap;
    return std::max(0.0F, std::min(metrics.header_lift, room));
}

Packed pack(std::vector<Group> groups, const Metrics& metrics, const PackOptions& options) {
    Packed packed;
    std::vector<Group> units;
    std::optional<Group> buildings;
    for (auto& group : groups) {
        if (group.choices.empty()) continue;
        if (group.kind == GroupKind::buildings) buildings = std::move(group);
        else units.push_back(std::move(group));
    }

    std::vector<Row> building_rows;
    if (buildings) {
        std::vector<Group> one;
        one.push_back(*buildings);
        place_groups(one, std::vector<std::size_t>{0}, metrics, building_rows, false);
    }
    const auto building_count = building_rows.size();
    const std::size_t budget = options.max_rows > building_count ? options.max_rows - building_count : 1;

    std::vector<std::size_t> natural(units.size());
    std::iota(natural.begin(), natural.end(), std::size_t{0});
    std::vector<Row> unit_rows;
    place_groups(units, natural, metrics, unit_rows, false);

    if (unit_rows.size() <= budget) {
        packed.groups = std::move(units);
    } else {
        packed.misc_mode = true;
        const auto capacity = entries_per_line(metrics);
        std::size_t total{};
        for (const auto& group : units) total += group.choices.size();
        std::vector<std::size_t> candidates;
        for (std::size_t index = 0; index < units.size(); ++index) {
            const auto& group = units[index];
            if (group.kind == GroupKind::misc) continue;
            if (options.fold_single_choice_groups && group.choices.size() < 2) continue;
            candidates.push_back(index);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto left, const auto right) {
            return units[left].choices.size() > units[right].choices.size();
        });
        std::vector<bool> chosen(units.size(), false);
        std::size_t chosen_entries{};
        std::size_t chosen_rows{};
        const auto order_of = [&](const std::vector<bool>& flags) {
            std::vector<std::size_t> order;
            for (std::size_t index = 0; index < flags.size(); ++index) if (flags[index]) order.push_back(index);
            return order;
        };
        for (const auto candidate : candidates) {
            auto trial = chosen;
            trial[candidate] = true;
            std::vector<Row> rows;
            place_groups(units, order_of(trial), metrics, rows, false);
            const auto entries = chosen_entries + units[candidate].choices.size();
            const auto misc_entries = total - entries;
            const auto misc_rows = misc_entries == 0 ? std::size_t{0} : (misc_entries + capacity - 1) / capacity;
            if (rows.size() + std::max<std::size_t>(misc_rows, 1) > budget) continue;
            chosen = std::move(trial);
            chosen_entries = entries;
            chosen_rows = rows.size();
        }
        Group misc;
        misc.kind = GroupKind::misc;
        misc.name = "Misc";
        misc.min_width = options.misc_min_width;
        misc.caption_key = options.misc_caption_key;
        for (const auto& group : units) {
            if (group.kind == GroupKind::misc) {
                if (!group.caption_key.empty()) misc.caption_key = group.caption_key;
                misc.min_width = std::max(misc.min_width, group.min_width);
            }
        }
        std::vector<Group> kept;
        for (std::size_t index = 0; index < units.size(); ++index) {
            if (chosen[index]) { kept.push_back(units[index]); continue; }
            for (const auto choice : units[index].choices) misc.choices.push_back(choice);
        }
        const auto available = budget - chosen_rows;
        if (misc.choices.size() > available * capacity) {
            packed.omitted = misc.choices.size() - available * capacity;
            misc.choices.resize(available * capacity);
        }
        kept.push_back(std::move(misc));
        std::vector<std::size_t> order(kept.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        unit_rows.clear();
        place_groups(kept, order, metrics, unit_rows, true);
        packed.groups = std::move(kept);
    }

    packed.unit_rows = unit_rows.size();
    packed.rows = std::move(unit_rows);
    if (buildings) {
        const auto index = packed.groups.size();
        packed.groups.push_back(std::move(*buildings));
        for (auto& row : building_rows) {
            for (auto& run : row.runs) run.group = index;
            packed.rows.push_back(std::move(row));
        }
    }
    packed.building_rows = building_count;
    packed.height = height_of(metrics, packed.rows);
    return packed;
}

FitResult fit_to_height(const std::vector<Group>& groups, const Metrics& full, const Metrics& compact,
    const PackOptions& options, const float available_height) {
    FitResult result;
    result.packed = pack(groups, full, options);
    if (result.packed.height <= available_height) return result;
    result.compact = true;
    result.packed = pack(groups, compact, options);
    if (result.packed.height <= available_height) return result;
    const auto buildings = result.packed.building_rows;
    const bool units = result.packed.unit_rows > 0;
    const std::size_t minimum = buildings + (units ? 1 : 0);
    for (auto rows = result.packed.rows.size(); rows-- > minimum;) {
        auto narrowed = options;
        narrowed.max_rows = std::max<std::size_t>(rows, minimum);
        auto attempt = pack(groups, compact, narrowed);
        const bool fits = attempt.height <= available_height;
        result.packed = std::move(attempt);
        if (fits) break;
    }
    return result;
}

float automatic_width(const std::vector<Group>& groups, Metrics metrics, const PackOptions& options,
    const float min_width, float max_width, const float max_height) {
    max_width = std::max(max_width, min_width);
    const float pitch = std::max(1.0F, metrics.entry_width + metrics.entry_gap);
    for (float width = min_width;; width = std::min(width + pitch, max_width)) {
        metrics.panel_width = width;
        const auto packed = pack(groups, metrics, options);
        if (width >= max_width || (!packed.misc_mode && packed.omitted == 0 && packed.height <= max_height)) return width;
    }
}

} // namespace eawr::presentation::ui::build_tray
