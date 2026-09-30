#include "xml_internal.hpp"
#include "eawr/data/tag_trace.hpp"

#include <optional>
#include <unordered_set>
#include <utility>

namespace eawr::data {

const EffectiveValue* EffectiveObject::value(const std::string_view tag_name) const noexcept {
    const EffectiveValue* result = nullptr;
    for (const auto& item : values) if (ascii_iequals(item.value.name, tag_name)) result = &item;
    if (result != nullptr) tag_trace::used(result->value);
    return result;
}

namespace {

// #628: every definition of a resolved object's variant chain is part of what the loaders touch,
// and its variant link is read, on every resolve (a cached one too).
void trace_chain(const Catalog& catalog, const EffectiveObject& object) {
    if (!tag_trace::enabled()) return;
    for (const auto& name : object.chain) {
        const auto* definition = catalog.find(name);
        if (definition == nullptr) continue;
        tag_trace::object(definition->root, object.object_id);
        tag_trace::used(child_named(definition->root, "Variant_Of_Existing_Type"));
    }
}

} // namespace

core::Result<EffectiveObject> Catalog::resolve(const std::string_view object_id) const {
    if (!impl_) return core::Result<EffectiveObject>::failure(diagnostic(
        diagnostic_codes::object_not_found, "catalog is empty"));
    const auto key = ascii_lower(object_id);
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
        trace_chain(*this, *cached);
        return core::Result<EffectiveObject>::success(std::move(*cached));
    }
    const Definition* current = find(object_id);
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
        current = find(parent_id);
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
    trace_chain(*this, result);
    return core::Result<EffectiveObject>::success(std::move(result));
}


} // namespace eawr::data
