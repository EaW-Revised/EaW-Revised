#pragma once

// Where a declared value came from.
struct ValueSite final {
    std::string tag;
    std::string declared;
    std::string canonical;
    std::string provenance;
    std::string source_object_id;
    data::SourceLocation source;
    std::string source_sha256;
};

struct Evidence final {
    std::string object_id;
    std::string type_name;
    std::string category;
    std::vector<std::string> chain;
    ValueSite model;
    ValueSite animation_set;
};

struct Observation final {
    std::string kind;
    std::string object_id;
    bool winner{true};
    ValueSite site;
};

struct Unresolved final {
    std::string object_id;
    std::string code;
    std::string message;
    std::vector<ValueSite> sites;
};

struct Conflict final {
    std::string object_id;
    std::string reason;
    std::vector<ValueSite> sites;
};

struct PairResult final {
    PinnedPair pair;
    std::string animation_set;
    std::string disposition;
    std::vector<Evidence> evidence;
    std::vector<Conflict> conflicts;
    std::vector<Unresolved> unresolved;
    std::vector<Observation> observations;
};

struct CatalogSummary final {
    bool loaded{};
    std::string load_error;
    std::string profile;
    std::size_t definitions{};
    std::size_t objects{};
    std::size_t resolved{};
    std::map<std::string, std::size_t> unresolved_by_code;
    std::map<std::string, std::size_t> diagnostics_by_code;
    std::size_t registry_files{};
    std::size_t registry_files_loaded{};
};

struct Audit final {
    CatalogSummary catalog;
    std::vector<PairResult> pairs;
    std::map<std::string, std::size_t> dispositions;
    std::size_t evidence_records{};
    std::size_t observation_records{};
};

namespace detail {

[[nodiscard]] inline ValueSite site_of(const data::XmlNode& node, const std::string_view provenance,
    const std::string& source_object_id, const SourceHash& source_hash) {
    return ValueSite{node.name, node.raw_text, is_link_field(node.name) ? canonical_model_path(node.raw_text) : std::string{},
        std::string(provenance), source_object_id, node.source,
        source_hash ? source_hash(node.source.logical_path) : std::string{}};
}

[[nodiscard]] inline auto site_key(const ValueSite& site) {
    return std::tie(site.source.logical_path, site.source.line, site.source.column, site.tag, site.declared);
}

[[nodiscard]] inline bool site_less(const ValueSite& left, const ValueSite& right) {
    return site_key(left) < site_key(right);
}

// The land model the scene layer would load: Land_Model_Name, else Model_Name.
[[nodiscard]] inline const data::EffectiveValue* land_model(const data::EffectiveObject& object) {
    if (const auto* value = object.value("Land_Model_Name")) return value;
    return object.value("Model_Name");
}

// Tokens of a value, split on commas and whitespace, folded.
[[nodiscard]] inline std::vector<std::string> tokens(const std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char character : text) {
        if (character == ',' || character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            if (!current.empty()) result.push_back(fold(current));
            current.clear();
        } else current.push_back(character);
    }
    if (!current.empty()) result.push_back(fold(current));
    return result;
}

// Every distinct canonical value one definition declares, own children only,
// per link tag.  Two different values of one tag in one definition are
// equally authoritative: the resolver keeps the last, the probe refuses both.
[[nodiscard]] inline std::map<std::string, std::vector<const data::XmlNode*>> own_link_values(
    const data::Definition& definition) {
    std::map<std::string, std::vector<const data::XmlNode*>> result;
    for (const auto& child : definition.root.children) {
        if (is_link_field(child.name)) result[fold(child.name)].push_back(&child);
    }
    return result;
}

// The last own child with a tag, as the resolver reads Variant_Of_Existing_Type.
[[nodiscard]] inline const data::XmlNode* last_child(const data::XmlNode& node, const std::string_view tag) {
    const data::XmlNode* result = nullptr;
    for (const auto& child : node.children)
        if (iequals(child.name, tag)) result = &child;
    return result;
}

// Walk the variant chain as far as it resolves, for an object whose resolve
// failed, collecting every definition reached.
[[nodiscard]] inline std::vector<const data::Definition*> partial_chain(
    const data::Catalog& catalog, const data::Definition& start) {
    std::vector<const data::Definition*> chain;
    std::set<std::string> visited;
    const data::Definition* current = &start;
    while (current != nullptr && visited.insert(fold(current->id)).second) {
        chain.push_back(current);
        const data::XmlNode* parent = last_child(current->root, "Variant_Of_Existing_Type");
        if (parent == nullptr || trimmed(parent->raw_text).empty()) break;
        current = catalog.find(trimmed(parent->raw_text));
    }
    return chain;
}

// The winner of an ID first, then every other active definition of it that
// is equally authoritative: same layer, same registry file.
[[nodiscard]] inline std::vector<const data::Definition*> equally_authoritative(
    const data::Catalog& catalog, const std::string_view object_id) {
    std::vector<const data::Definition*> result;
    const data::Definition* winner = catalog.find(object_id);
    if (winner == nullptr) return result;
    result.push_back(winner);
    for (const data::Definition* definition : catalog.find_all(object_id)) {
        if (definition != winner && definition->active
            && iequals(definition->root.source.layer_id, winner->root.source.layer_id)
            && definition->registry_order == winner->registry_order)
            result.push_back(definition);
    }
    return result;
}

// A land-model or override value and the definition that declared it.
struct LinkSource final {
    const data::XmlNode* node{};
    std::string definition_id;
};

// The link values one way of resolving an object gives it: each definition
// with its own values over its own Variant_Of_Existing_Type chain.
struct LinkOutcome final {
    LinkSource land;
    LinkSource model;
    LinkSource set;
    // Some definition on the way is a non-winning, equally authoritative duplicate.
    bool alternative{};
};

struct LinkOutcomes final {
    std::vector<LinkOutcome> outcomes;
    // Why some way of resolving could not be followed to its end.
    std::set<std::string> undetermined;
    // Every land-model, model or override value any walked definition declares.
    std::vector<LinkSource> reachable;
};

inline constexpr std::size_t max_link_outcomes = 64;

// Every way `definition` can resolve when each variant base may be its winner
// or any equally authoritative duplicate, as the resolver would fold it
// (replace mode, last own value of a tag wins).  What cannot be followed (a
// missing base, a cycle, a non-replace field, too many ways) is recorded in
// `state.undetermined`, never guessed.
[[nodiscard]] inline std::vector<LinkOutcome> link_outcomes(const data::Catalog& catalog,
    const data::Definition& definition, std::vector<std::string>& visiting, LinkOutcomes& state) {
    const std::string key = fold(definition.id);
    if (std::find(visiting.begin(), visiting.end(), key) != visiting.end()) {
        state.undetermined.insert("variant cycle through " + definition.id);
        return {};
    }
    std::vector<LinkOutcome> outcomes{LinkOutcome{}};
    const data::XmlNode* parent = last_child(definition.root, "Variant_Of_Existing_Type");
    if (parent != nullptr && !trimmed(parent->raw_text).empty()) {
        const std::string parent_id = trimmed(parent->raw_text);
        const auto bases = equally_authoritative(catalog, parent_id);
        if (bases.empty()) state.undetermined.insert("missing variant base " + parent_id + " of " + definition.id);
        outcomes.clear();
        visiting.push_back(key);
        for (const data::Definition* base : bases) {
            for (LinkOutcome outcome : link_outcomes(catalog, *base, visiting, state)) {
                outcome.alternative = outcome.alternative || base != bases.front();
                outcomes.push_back(std::move(outcome));
            }
        }
        visiting.pop_back();
    }
    LinkSource land;
    LinkSource model;
    LinkSource set;
    for (const auto& child : definition.root.children) {
        LinkSource* target = iequals(child.name, "Land_Model_Name") ? &land
            : iequals(child.name, "Model_Name")                    ? &model
            : iequals(child.name, override_tag)                    ? &set
                                                                   : nullptr;
        if (target == nullptr) continue;
        if (child.merge_mode != data::MergeMode::replace)
            state.undetermined.insert(child.name + " of " + definition.id + " is not a replace-mode field");
        *target = LinkSource{&child, definition.id};
        state.reachable.push_back(*target);
    }
    for (LinkOutcome& outcome : outcomes) {
        if (land.node != nullptr) outcome.land = land;
        if (model.node != nullptr) outcome.model = model;
        if (set.node != nullptr) outcome.set = set;
    }
    if (outcomes.size() > max_link_outcomes) {
        state.undetermined.insert("more than " + std::to_string(max_link_outcomes) + " ways to resolve " + definition.id);
        outcomes.resize(max_link_outcomes);
    }
    return outcomes;
}

// The canonical land model (Land_Model_Name, else Model_Name) and set of an outcome.
[[nodiscard]] inline std::pair<std::string, std::string> outcome_link(const LinkOutcome& outcome) {
    const LinkSource& land = outcome.land.node != nullptr ? outcome.land : outcome.model;
    return {land.node != nullptr ? canonical_model_path(land.node->raw_text) : std::string{},
        outcome.set.node != nullptr ? canonical_model_path(outcome.set.node->raw_text) : std::string{}};
}

inline void scan_mentions(const data::XmlNode& node, const std::string& object_id, const std::string& provenance,
    const std::string& source_object_id, const std::map<std::string, std::vector<std::size_t>>& names,
    std::vector<PairResult>& results, const SourceHash& source_hash, const bool top) {
    if (!(top && is_link_field(node.name))) {
        std::set<std::size_t> hit;
        for (const std::string& token : tokens(node.raw_text)) {
            const auto found = names.find(token);
            if (found != names.end()) hit.insert(found->second.begin(), found->second.end());
        }
        for (const std::size_t index : hit) {
            results[index].observations.push_back(Observation{std::string(observation::text_mention), object_id, true,
                site_of(node, provenance, source_object_id, source_hash)});
        }
    }
    for (const auto& child : node.children)
        scan_mentions(child, object_id, provenance, source_object_id, names, results, source_hash, false);
}

} // namespace detail
