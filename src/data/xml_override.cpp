#include "xml_internal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace eawr::data {

namespace {

[[nodiscard]] std::vector<std::string> split_path(const std::string_view path) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        if (!part.empty()) parts.emplace_back(part);
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

[[nodiscard]] XmlNode* last_child(XmlNode& node, const std::string_view name) {
    XmlNode* found = nullptr;
    for (auto& child : node.children) if (ascii_iequals(child.name, name)) found = &child;
    return found;
}

// The element `values` are made from: the node's own occurrence, else the inherited one (so a
// schema classification and list flags carry over), else a bare element at the container.
[[nodiscard]] XmlNode value_template(const XmlNode& container, const std::string_view name, const XmlNode* inherited) {
    for (const auto& child : container.children) if (ascii_iequals(child.name, name)) return child;
    if (inherited != nullptr) return *inherited;
    XmlNode bare;
    bare.name = std::string(name);
    bare.source = container.source;
    return bare;
}

// The container's `name` children become one element per value, where its first one stood.
void replace_children(XmlNode& container, const std::string_view name, const std::vector<std::string>& values,
    const XmlNode* inherited) {
    auto made = value_template(container, name, inherited);
    // The override is the value, not a token merge with the inherited one.
    made.merge_mode = MergeMode::replace;
    made.children.clear();
    std::size_t at = container.children.size();
    std::vector<XmlNode> kept;
    kept.reserve(container.children.size() + values.size());
    for (auto& child : container.children) {
        if (ascii_iequals(child.name, name)) {
            if (at == container.children.size()) at = kept.size();
            continue;
        }
        kept.push_back(std::move(child));
    }
    if (at > kept.size()) at = kept.size();
    std::vector<XmlNode> inserted;
    for (const auto& value : values) {
        auto node = made;
        node.raw_text = value;
        inserted.push_back(std::move(node));
    }
    kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(at), inserted.begin(), inserted.end());
    container.children = std::move(kept);
}

thread_local const std::vector<DocumentOverride>* document_overrides = nullptr;

} // namespace

DocumentOverrides::DocumentOverrides(std::vector<DocumentOverride> overrides)
    : overrides_(std::move(overrides)), previous_(document_overrides) {
    document_overrides = &overrides_;
}

DocumentOverrides::~DocumentOverrides() {
    document_overrides = previous_;
}

void apply_document_overrides(XmlNode& root, const std::string_view canonical_path) {
    if (document_overrides == nullptr) return;
    for (const auto& change : *document_overrides) {
        auto wanted = vfs::canonicalize(change.logical_path);
        if (!wanted || wanted.value() != canonical_path) continue;
        const auto path = split_path(change.tag);
        if (path.empty()) continue;
        XmlNode* container = &root;
        for (std::size_t index = 0; container != nullptr && index + 1 < path.size(); ++index) {
            container = last_child(*container, path[index]);
        }
        if (container != nullptr) replace_children(*container, path.back(), change.values, nullptr);
    }
}

core::Result<Catalog> with_overrides(const Catalog& catalog, const std::span<const ValueOverride> overrides) {
    using Made = core::Result<Catalog>;
    if (!catalog.impl_) return Made::failure(diagnostic(diagnostic_codes::object_not_found, "catalog is empty"));
    auto overlay = std::make_shared<Catalog::Overlay>();
    overlay->generation = next_generation.fetch_add(1, std::memory_order_relaxed);
    if (catalog.overlay_) overlay->replaced = catalog.overlay_->replaced;
    for (const auto& change : overrides) {
        const auto winner = catalog.impl_->winners.find(ascii_lower(change.object_id));
        if (winner == catalog.impl_->winners.end()) {
            return Made::failure(diagnostic(diagnostic_codes::object_not_found,
                "override of an unknown object: " + change.object_id));
        }
        const auto path = split_path(change.tag);
        if (path.empty()) {
            return Made::failure(diagnostic(diagnostic_codes::unknown_field,
                "override of " + change.object_id + " names no tag"));
        }
        // The inherited top-level value, when the object's own definition does not author it.
        auto effective = catalog.resolve(change.object_id);
        if (!effective) return Made::failure(effective.error());
        const XmlNode* inherited = nullptr;
        for (const auto& value : effective.value().values) {
            if (ascii_iequals(value.value.name, path.front())) inherited = &value.value;
        }
        auto replaced = overlay->replaced.find(winner->second);
        if (replaced == overlay->replaced.end()) {
            replaced = overlay->replaced.emplace(winner->second, catalog.impl_->definitions[winner->second]).first;
        }
        XmlNode* container = &replaced->second.root;
        if (path.size() > 1) {
            // A nested value: the object's own copy of the top-level element carries it.
            if (last_child(*container, path.front()) == nullptr) {
                if (inherited == nullptr) {
                    return Made::failure(diagnostic(diagnostic_codes::unknown_field,
                        "override of " + change.object_id + ": " + path.front() + " is neither authored nor inherited"));
                }
                auto copy = *inherited;
                copy.merge_mode = MergeMode::replace;
                container->children.push_back(std::move(copy));
            }
            container = last_child(*container, path.front());
            inherited = nullptr;
            for (std::size_t index = 1; index + 1 < path.size(); ++index) {
                container = last_child(*container, path[index]);
                if (container == nullptr) {
                    return Made::failure(diagnostic(diagnostic_codes::unknown_field,
                        "override of " + change.object_id + ": no " + path[index] + " in " + change.tag));
                }
            }
        }
        replace_children(*container, path.back(), change.values, inherited);
    }
    Catalog result(catalog.impl_);
    result.overlay_ = std::move(overlay);
    return Made::success(std::move(result));
}

} // namespace eawr::data
