#include "xml_internal.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/retail_values.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_set>
#include <utility>

namespace eawr::data {

core::Result<TypeMemberFlags> Catalog::member_flags(const std::string_view object_id) const {
    using Result = core::Result<TypeMemberFlags>;
    const auto* definition = find(object_id, Category::game_object);
    if (definition == nullptr) return Result::failure(diagnostic(
        diagnostic_codes::object_not_found, "member flags: unknown game object " + std::string(object_id)));
    const auto& issue = overlay_ ? overlay_->member_flags_issue : impl_->member_flags_issue;
    if (!issue.empty()) return Result::failure(diagnostic(
        diagnostic_codes::member_timeline_unavailable, "member-parse-timeline: " + issue, &definition->root.source));
    const auto& flags = overlay_ ? overlay_->member_flags : impl_->member_flags;
    const auto& indexes = impl_->by_id.at(ascii_lower(object_id));
    for (const auto index : indexes) {
        if (definition_at(index).category == Category::game_object) return Result::success(flags[index]);
    }
    return Result::failure(diagnostic(diagnostic_codes::member_timeline_unavailable,
        "member-parse-timeline: game object has no timeline slot"));
}

// WGA-363/364, WAR-84: load-only state machine. Preserve copy-time flags and
// rosters rather than closing the final variant ancestry over the final roster.
void derive_member_flags(const std::span<const Definition* const> definitions,
    std::vector<TypeMemberFlags>& flags, std::string& issue) {
    struct State {
        bool complete{};
        std::string base;
        std::array<std::vector<std::string>, 2> rosters;
    };
    flags.assign(definitions.size(), {});
    issue.clear();
    std::vector<State> states(definitions.size());
    std::vector<std::size_t> order;
    std::unordered_map<std::string, std::size_t> available;
    constexpr std::array<std::string_view, 2> tags{"Company_Units", "Squadron_Units"};
    const auto authored = [&](const std::size_t index, const bool variant) {
        for (std::size_t roster = 0; roster < tags.size(); ++roster) {
            bool first = true;
            for (const auto& node : definitions[index]->root.children) {
                if (!ascii_iequals(node.name, tags[roster]) || retail::is_placeholder(node.raw_text)) continue;
                tag_trace::used(node);
                std::vector<std::string> names;
                std::string name;
                for (const char ch : node.raw_text) {
                    if (xml_space(static_cast<unsigned char>(ch)) || ch == ',' || ch == '\f' || ch == '\v') {
                        if (!name.empty()) names.push_back(std::exchange(name, {}));
                    } else name += ch;
                }
                if (!name.empty()) names.push_back(std::move(name));
                if (names.empty()) continue;
                if (variant && first) states[index].rosters[roster].clear();
                first = false;
                auto& destination = states[index].rosters[roster];
                destination.insert(destination.end(), names.begin(), names.end());
            }
        }
    };
    const auto overlay = [&](const std::size_t index) {
        const auto found = available.find(ascii_lower(states[index].base));
        if (found == available.end() || !states[found->second].complete) return;
        flags[index] = flags[found->second];
        states[index].rosters = states[found->second].rosters;
        authored(index, true);
        states[index].complete = true;
    };
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = *definitions[index];
        if (!definition.active || definition.category != Category::game_object) continue;
        if (definition.id.empty() || !available.emplace(ascii_lower(definition.id), index).second) {
            issue = "empty or duplicate game-object identity: " + definition.id;
            return;
        }
        order.push_back(index);
        if (const auto* base = child_named(definition.root, "Variant_Of_Existing_Type");
            base && !retail::is_placeholder(base->raw_text)) {
            tag_trace::used(*base);
            states[index].base = trim(base->raw_text);
        }
        authored(index, false);
        states[index].complete = states[index].base.empty();
        if (!states[index].complete) overlay(index);
    }
    // Pass zero created types in registry/document order. Later passes replay
    // an entire file for each still-incomplete type from that file, including
    // already-complete variants in it. Copying may overwrite earlier marks.
    for (std::size_t pass = 0; pass < 10; ++pass) {
        if (pass != 0) {
            for (std::size_t start = 0; start < order.size();) {
                std::size_t end = start + 1;
                const auto file = definitions[order[start]]->registry_order;
                while (end < order.size() && definitions[order[end]]->registry_order == file) ++end;
                for (std::size_t candidate = start; candidate < end; ++candidate) {
                    if (states[order[candidate]].complete) continue;
                    for (std::size_t entry = start; entry < end; ++entry)
                        if (!states[order[entry]].base.empty()) overlay(order[entry]);
                }
                start = end;
            }
        }
        // All current rosters participate, including an incomplete variant's
        // authored roster. Writes only set true; the fix-up never clears bits.
        for (const auto index : order) {
            for (std::size_t roster = 0; roster < tags.size(); ++roster) {
                for (const auto& member : states[index].rosters[roster]) {
                    const auto found = available.find(ascii_lower(member));
                    if (found == available.end()) {
                        issue = "unresolved " + std::string(tags[roster]) + " member " + member
                            + " in " + definitions[index]->id;
                        return;
                    }
                    auto& target = flags[found->second];
                    (roster == 0 ? target.ground_company_member : target.squadron_member) = true;
                }
            }
        }
        if (std::all_of(order.begin(), order.end(), [&](const auto index) { return states[index].complete; })) return;
    }
    issue = "variant loading incomplete after ten passes";
}

const EffectiveValue* EffectiveObject::value(const std::string_view tag_name) const noexcept {
    const EffectiveValue* result = nullptr;
    for (const auto& item : values) if (ascii_iequals(item.value.name, tag_name)) result = &item;
    if (result != nullptr) tag_trace::used(result->value);
    return result;
}

namespace {

// #628: every definition of a resolved object's variant chain is part of what the loaders touch,
// and its variant link is read, on every resolve (a cached one too).
void trace_chain(const Catalog& catalog, const EffectiveObject& object, const std::optional<Category> category) {
    if (!tag_trace::enabled()) return;
    for (const auto& name : object.chain) {
        const auto* definition = category ? catalog.find(name, *category) : catalog.find(name);
        if (definition == nullptr) continue;
        tag_trace::object(definition->root, object.object_id);
        tag_trace::used(child_named(definition->root, "Variant_Of_Existing_Type"));
    }
}

} // namespace

core::Result<EffectiveObject> Catalog::resolve(const std::string_view object_id) const {
    return resolve_in(object_id, std::nullopt);
}

core::Result<EffectiveObject> Catalog::resolve(const std::string_view object_id, const Category category) const {
    return resolve_in(object_id, category);
}

core::Result<EffectiveObject> Catalog::resolve_in(const std::string_view object_id,
    const std::optional<Category> category) const {
    if (!impl_) return core::Result<EffectiveObject>::failure(diagnostic(
        diagnostic_codes::object_not_found, "catalog is empty"));
    const auto key = (category ? std::string(to_string(*category)) : std::string("untyped")) + ":" + ascii_lower(object_id);
    // An overlay (with_overrides) caches apart from the load it shares.
    auto& cache_mutex = overlay_ ? overlay_->cache_mutex : impl_->cache_mutex;
    auto& resolve_cache = overlay_ ? overlay_->resolve_cache : impl_->resolve_cache;
    std::optional<EffectiveObject> cached;
    {
        const std::scoped_lock lock(cache_mutex);
        const auto found = resolve_cache.find(key);
        if (found != resolve_cache.end()) cached = found->second;
    }
    if (cached) {
        trace_chain(*this, *cached, category);
        return core::Result<EffectiveObject>::success(std::move(*cached));
    }
    const Definition* current = category ? find(object_id, *category) : find(object_id);
    if (current == nullptr) return core::Result<EffectiveObject>::failure(diagnostic(
        diagnostic_codes::object_not_found, "object not found: " + std::string(object_id)));

    std::vector<const Definition*> chain;
    std::vector<std::string> chain_names;
    std::unordered_set<std::string> visited;
    while (current != nullptr) {
        const auto current_key = ascii_lower(current->id);
        if (!visited.insert(current_key).second) {
            chain_names.push_back(current->id);
            return core::Result<EffectiveObject>::failure(diagnostic(
                diagnostic_codes::variant_cycle,
                "variant cycle: " + join_chain(chain_names),
                &current->root.source));
        }
        chain.push_back(current);
        chain_names.push_back(current->id);
        const auto* parent = child_named(current->root, "Variant_Of_Existing_Type");
        if (parent == nullptr || trim(parent->raw_text).empty()) break;
        const auto parent_id = trim(parent->raw_text);
        current = category ? find(parent_id, *category) : find(parent_id);
        if (current == nullptr) {
            chain_names.push_back(parent_id);
            return core::Result<EffectiveObject>::failure(diagnostic(
                diagnostic_codes::variant_missing_base,
                "missing variant base: " + join_chain(chain_names),
                &parent->source));
        }
    }

    struct Group {
        std::string key;
        std::vector<EffectiveValue> occurrences;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> group_index;
    const auto& top = *chain.front();
    for (auto layer = chain.rbegin(); layer != chain.rend(); ++layer) {
        const auto& definition = **layer;
        for (const auto& tag : definition.root.children) {
            if (ascii_iequals(tag.name, "Variant_Of_Existing_Type")) continue;
            const auto tag_key = ascii_lower(tag.name);
            auto found = group_index.find(tag_key);
            const bool existed = found != group_index.end() && !groups[found->second].occurrences.empty();
            if (tag.merge_mode == MergeMode::ignored && &definition != chain.back()) continue;
            if (found == group_index.end()) {
                found = group_index.emplace(tag_key, groups.size()).first;
                groups.push_back(Group{tag_key, {}});
            }
            auto& occurrences = groups[found->second].occurrences;
            if (tag.merge_mode == MergeMode::merge && tag.multiple_allowed) {
                EffectiveValue value{tag, ValueProvenance::inherited, definition.id, std::nullopt};
                if (chain.size() == 1) {
                    value.provenance = ValueProvenance::own;
                } else if (&definition == &top) {
                    value.provenance = existed ? ValueProvenance::merged : ValueProvenance::added;
                    if (existed) value.displaced_value = occurrences.front().value;
                }
                occurrences.push_back(std::move(value));
                continue;
            }
            XmlNode effective = tag;
            std::optional<XmlNode> displaced;
            if (existed) displaced = occurrences.back().value;
            if (tag.merge_mode == MergeMode::merge && existed) {
                effective.raw_text = token_merge(occurrences.back().value.raw_text, tag.raw_text);
            }
            ValueProvenance provenance = ValueProvenance::inherited;
            if (chain.size() == 1) provenance = ValueProvenance::own;
            else if (&definition == &top) {
                provenance = !existed ? ValueProvenance::added
                    : tag.merge_mode == MergeMode::merge ? ValueProvenance::merged
                    : ValueProvenance::overridden;
            }
            occurrences.assign(1, EffectiveValue{
                std::move(effective), provenance, definition.id,
                provenance == ValueProvenance::overridden || provenance == ValueProvenance::merged
                    ? std::move(displaced) : std::nullopt});
        }
    }

    EffectiveObject result{
        .object_id = top.id,
        .type_name = top.type_name,
        .category = top.category,
        .chain = std::move(chain_names),
        .values = {},
        .catalog_generation = generation(),
    };
    for (auto& group : groups) {
        for (auto& occurrence : group.occurrences) result.values.push_back(std::move(occurrence));
    }
    {
        const std::scoped_lock lock(cache_mutex);
        resolve_cache.insert_or_assign(key, result);
    }
    trace_chain(*this, result, category);
    return core::Result<EffectiveObject>::success(std::move(result));
}


} // namespace eawr::data
