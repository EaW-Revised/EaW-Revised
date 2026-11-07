#include "eawr/data/record_index.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace eawr::data {
namespace {
std::string key(std::string_view s) {
    std::string out(s);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}
using Key = std::pair<data::Category, std::string>;
void frame(std::vector<std::uint8_t>& out, std::string_view text) {
    auto n = static_cast<std::uint64_t>(text.size());
    for (unsigned i = 0; i != 8; ++i) out.push_back(static_cast<std::uint8_t>(n >> (8U * i)));
    out.insert(out.end(), text.begin(), text.end());
}
void frame_node(std::vector<std::uint8_t>& out, const data::XmlNode& node) {
    frame(out, node.name); frame(out, node.raw_text);
    frame(out, std::to_string(node.attributes.size()));
    for (const auto& attribute : node.attributes) { frame(out, attribute.name); frame(out, attribute.value); }
    frame(out, std::to_string(node.children.size()));
    for (const auto& child : node.children) frame_node(out, child);
}
std::string digest(const data::Definition& definition) {
    std::vector<std::uint8_t> bytes;
    frame_node(bytes, definition.root);
    return core::sha256_hex(bytes);
}
RecordTagHow how(data::ValueProvenance p) {
    switch (p) {
    case data::ValueProvenance::own: return RecordTagHow::own;
    case data::ValueProvenance::overridden: return RecordTagHow::overridden;
    case data::ValueProvenance::inherited: return RecordTagHow::inherited;
    case data::ValueProvenance::merged: return RecordTagHow::merged;
    case data::ValueProvenance::added: return RecordTagHow::added;
    }
    return RecordTagHow::own;
}
RecordTagRow& row(std::map<std::string, RecordTagRow>& rows, const std::string& tag) {
    auto& result = rows[key(tag)];
    if (result.tag.empty()) result.tag = tag;
    return result;
}
std::vector<RecordTagRow> finish(std::map<std::string, RecordTagRow>& rows) {
    std::vector<RecordTagRow> result;
    result.reserve(rows.size());
    for (auto& [name, value] : rows) { (void)name; result.push_back(std::move(value)); }
    return result;
}
void add_authored(std::map<std::string, RecordTagRow>& rows, const data::Definition& def) {
    std::map<std::string, std::vector<data::XmlNode>> grouped;
    for (const auto& child : def.root.children) grouped[key(child.name)].push_back(child);
    for (auto& [name, values] : grouped) {
        auto& r = row(rows, values.front().name);
        r.authored.push_back({def.root.source, def.registry_order, def.definition_order,
                              def.namespace_winner, std::move(values)});
        (void)name;
    }
}
void add_effective(std::map<std::string, RecordTagRow>& rows, const data::EffectiveObject& object, bool vanilla) {
    for (const auto& value : object.values) {
        auto& r = row(rows, value.value.name);
        if (vanilla) r.vanilla.push_back(value);
        else { r.effective.push_back(value); r.how = how(value.provenance); }
    }
}
}

const RecordEntry* RecordIndex::find(std::string_view id, data::Category category) const {
    const auto wanted = key(id);
    for (const auto& record : records) if (record.category == category && key(record.id) == wanted) return &record;
    return nullptr;
}
core::Result<data::EffectiveObject> RecordIndex::effective(const RecordEntry& record) const {
    return profile_catalog->resolve(record.id, record.category);
}
core::Result<std::vector<RecordTagRow>> RecordIndex::tags(const RecordEntry& record) const {
    std::map<std::string, RecordTagRow> rows;
    if (vanilla_catalog->find(record.id, record.category)) {
        auto value = vanilla_catalog->resolve(record.id, record.category);
        if (!value) return core::Result<std::vector<RecordTagRow>>::failure(value.error());
        add_effective(rows, value.value(), true);
    }
    for (const auto& definition : record.definitions) add_authored(rows, *definition.definition);
    if (profile_catalog->find(record.id, record.category)) {
        auto value = profile_catalog->resolve(record.id, record.category);
        if (!value) return core::Result<std::vector<RecordTagRow>>::failure(value.error());
        add_effective(rows, value.value(), false);
    }
    for (auto& [name, value] : rows) {
        (void)name;
        if (value.effective.empty()) value.how = RecordTagHow::dropped;
    }
    return core::Result<std::vector<RecordTagRow>>::success(finish(rows));
}
core::Result<RecordIndex> build_record_index(const data::Catalog& vanilla, const data::Catalog& profile,
                                               const data::ShadowedDefinitions& shadowed, const vfs::Vfs& profile_vfs,
                                               std::span<const RecordLayer> layers) {
    RecordIndex result;
    result.vanilla_catalog = std::make_shared<data::Catalog>(vanilla);
    result.profile_catalog = std::make_shared<data::Catalog>(profile);
    result.shadowed_definitions = std::make_shared<data::ShadowedDefinitions>(shadowed);
    result.diagnostics = shadowed.diagnostics;
    result.layers.assign(layers.begin(), layers.end());
    std::map<Key, std::size_t> positions;
    auto ensure = [&](const data::Definition& def) -> RecordEntry& {
        const Key k{def.category, key(def.id)};
        const auto [at, inserted] = positions.emplace(k, result.records.size());
        if (inserted) {
            RecordEntry entry;
            entry.id = def.id; entry.type_name = def.type_name; entry.category = def.category;
            result.records.push_back(std::move(entry));
        }
        return result.records[at->second];
    };
    for (const auto& def : result.vanilla_catalog->definitions()) ensure(def);
    for (const auto& def : result.profile_catalog->definitions()) {
        auto& entry = ensure(def);
        entry.definitions.push_back({&def, digest(def), std::nullopt, result.profile_catalog, {}});
        if (def.namespace_winner) { entry.winner = &def; entry.winner_owner = result.profile_catalog; entry.type_name = def.type_name; }
    }
    for (const auto& def : result.shadowed_definitions->definitions) ensure(def).definitions.push_back({&def, digest(def), std::nullopt, {}, result.shadowed_definitions});
    std::map<std::string, std::vector<vfs::AssetRecord>> candidates_by_path;
    for (auto& entry : result.records) for (auto& indexed : entry.definitions) {
        const auto& source = indexed.definition->root.source;
        auto found = candidates_by_path.find(source.logical_path);
        if (found == candidates_by_path.end()) {
            auto candidates = profile_vfs.candidates(source.logical_path);
            if (!candidates) result.diagnostics.push_back(candidates.error());
            found = candidates_by_path.emplace(source.logical_path,
                candidates ? std::move(candidates.value()) : std::vector<vfs::AssetRecord>{}).first;
        }
        const auto& list = found->second;
        const auto candidate = std::find_if(list.begin(), list.end(), [&](const auto& item) {
            return item.layer_id == source.layer_id && item.source_id == source.source_id;
        });
        if (candidate != list.end()) indexed.candidate_rank = static_cast<std::size_t>(candidate - list.begin());
    }
    for (auto& entry : result.records) {
        std::stable_sort(entry.definitions.begin(), entry.definitions.end(), [&](const auto& left, const auto& right) {
            if (!left.candidate_rank) return false;
            if (!right.candidate_rank) return true;
            return *left.candidate_rank > *right.candidate_rank;
        });
    }
    std::set<Key> active;
    for (const auto& file : profile.registry_files()) active.emplace(file.category, key(file.included_path));
    for (const auto& file : vanilla.registry_files()) {
        if (!active.contains({file.category, key(file.included_path)})) result.registry_dropped_vanilla_files.push_back(file);
    }
    return core::Result<RecordIndex>::success(std::move(result));
}
core::Result<WholeFileView> compare_whole_file(const vfs::Vfs& vanilla, const vfs::Vfs& profile,
                                                std::string_view logical_path) {
    WholeFileView result;
    auto original = data::load_document(vanilla, logical_path);
    if (original) result.vanilla = std::move(original.value());
    else if (original.error().code != vfs::diagnostic_codes::not_found)
        return core::Result<WholeFileView>::failure(original.error());
    auto candidates = profile.candidates(logical_path);
    if (!candidates) return core::Result<WholeFileView>::failure(candidates.error());
    std::map<std::string, RecordTagRow> rows;
    if (result.vanilla) for (const auto& node : result.vanilla->root.children)
        row(rows, node.name).vanilla.push_back({node, data::ValueProvenance::own, {}, std::nullopt, std::nullopt});
    bool winner = true;
    for (const auto& candidate : candidates.value()) {
        auto document = data::load_document(profile, candidate);
        if (!document) {
            result.candidates.push_back({candidate, std::nullopt, document.error(), winner});
            winner = false;
            continue;
        }
        data::Definition def;
        def.root = document.value().root; def.namespace_winner = winner;
        add_authored(rows, def);
        if (winner) for (const auto& node : document.value().root.children)
            row(rows, node.name).effective.push_back({node, data::ValueProvenance::own, {}, std::nullopt, std::nullopt});
        result.candidates.push_back({candidate, std::move(document.value()), std::nullopt, winner});
        winner = false;
    }
    for (auto& [name, value] : rows) { (void)name; if (value.effective.empty()) value.how = RecordTagHow::dropped; }
    result.tags = finish(rows);
    return core::Result<WholeFileView>::success(std::move(result));
}
} // namespace eawr::data
